/* Per-VM virtual PIT0 time, independent of physical IRQ0 ownership.
 * Freestanding OpenWatcom C: no libc, allocator or 64-bit runtime helpers.
 */
#include "session_clock.h"
#include "guest_peripherals.h"

#define CVCLK_VM_COUNT 4u
#define CVCLK_PIT_HZ   1193182UL
#define CVCLK_MAX_MS_CHUNK 3590000UL

/* Implemented in session_clock.inc. The division helper returns the low
 * quotient word and writes the remainder; callers ensure quotient fits. */
extern void CVCLK_CALL cvclock_now(uint32_t *low, uint32_t *high);
extern uint32_t CVCLK_CALL cvclock_div(uint32_t high, uint32_t low,
                                      uint32_t divisor, uint32_t *remainder);

typedef struct cvclock_state {
    cvgp_state pit;
    uint32_t generation;
    uint32_t tsc_khz;
    uint32_t last_low, last_high;
    uint32_t sub_ms_cycles;
    uint32_t pit_fraction;          /* thousandths of one PIT input clock */
    uint8_t active;
} cvclock_state;

static cvclock_state clocks[CVCLK_VM_COUNT];

static cvclock_state *clock_for(uint32_t vm)
{
    if (vm >= CVCLK_VM_COUNT || !clocks[vm].active) return (cvclock_state *)0;
    return &clocks[vm];
}

static void add_milliseconds(cvclock_state *s, uint32_t high, uint32_t low)
{
    uint32_t chunk, fraction, pit_clocks;
    while (high || low) {
        if (high || low > CVCLK_MAX_MS_CHUNK) {
            chunk = CVCLK_MAX_MS_CHUNK;
        } else {
            chunk = low;
        }

        /* Split 1193182 into 1193 + 182/1000. A 3,590,000 ms
         * chunk keeps both products and the PIT-clock total in 32 bits. */
        pit_clocks = chunk * 1193u;
        fraction = chunk * 182u + s->pit_fraction;
        pit_clocks += fraction / 1000u;
        s->pit_fraction = fraction % 1000u;
        (void)cvgp_advance(&s->pit, s->generation, pit_clocks);

        low -= chunk;
        if (low > 0xffffffffUL - chunk) --high;
    }
}

/* Advance one clock by its complete unsigned 64-bit TSC delta. Division is
 * split into two safe 32/32 stages when the elapsed millisecond quotient no
 * longer fits one word. Ordinary polls take one small, constant-time path. */
static void advance_to_now(cvclock_state *s)
{
    uint32_t low, high, delta_low, delta_high;
    uint32_t q_high, q_low, remainder, combined;
    cvclock_now(&low, &high);
    delta_low = low - s->last_low;
    delta_high = high - s->last_high - (low < s->last_low);
    s->last_low = low;
    s->last_high = high;

    if (!s->tsc_khz) return;
    /* (high:low) / khz = q_high: q_low. First divide the high limb; then
     * the remaining 64-bit numerator has a quotient that fits one word. */
    q_high = delta_high / s->tsc_khz;
    remainder = delta_high % s->tsc_khz;
    q_low = cvclock_div(remainder, delta_low, s->tsc_khz, &remainder);

    combined = remainder + s->sub_ms_cycles;
    if (combined >= s->tsc_khz) {
        combined -= s->tsc_khz;
        if (++q_low == 0) ++q_high;
    }
    s->sub_ms_cycles = combined;
    add_milliseconds(s, q_high, q_low);
    /* The PIT phase follows all elapsed time, but its IRQ output feeds one
     * 8259 request bit. Replaying every elapsed period after a slow disk
     * transfer/VM timeslice creates an IRQ0 storm and starves IRQ1/IRQ7.
     * Keep one undelivered edge until clock_poll transfers it to the PIC. */
    if (s->pit.pit0_pending_irqs > 1u) s->pit.pit0_pending_irqs = 1u;
}

void CVCLK_CALL cvclock_begin(uint32_t vm, uint32_t tsc_khz)
{
    cvclock_state *s;
    if (vm >= CVCLK_VM_COUNT || !tsc_khz) return;
    s = &clocks[vm];
    if (s->active) (void)cvgp_end(&s->pit, s->generation);
    cvgp_init(&s->pit);
    s->generation = vm + 1u;
    if (cvgp_begin(&s->pit, s->generation, CVGP_CAP_PIC_PIT,
                   CVGP_CAP_PIC_PIT, (const cvgp_backend *)0) != CVGP_OK) {
        s->active = 0;
        return;
    }
    s->tsc_khz = tsc_khz;
    s->sub_ms_cycles = 0;
    s->pit_fraction = 0;
    cvclock_now(&s->last_low, &s->last_high);
    s->active = 1;
}

void CVCLK_CALL cvclock_end(uint32_t vm)
{
    cvclock_state *s = clock_for(vm);
    if (!s) return;
    (void)cvgp_end(&s->pit, s->generation);
    s->active = 0;
    s->tsc_khz = 0;
    s->last_low = s->last_high = 0;
    s->sub_ms_cycles = s->pit_fraction = 0;
}

uint32_t CVCLK_CALL cvclock_port(uint32_t vm, uint32_t port,
                                 uint32_t write, uint32_t value)
{
    cvclock_state *s = clock_for(vm);
    uint32_t result = 0xffu, byte = value & 0xffu;
    if (!s || (port != 0x40u && port != 0x43u)) return result;
    advance_to_now(s);
    if (write) {
        (void)cvgp_io_write(&s->pit, s->generation, (uint16_t)port,
                            1, 0, byte);
        return byte;
    }
    (void)cvgp_io_read(&s->pit, s->generation, (uint16_t)port,
                       1, 0, &result);
    return result & 0xffu;
}

uint32_t CVCLK_CALL cvclock_pending(uint32_t vm)
{
    cvclock_state *s = clock_for(vm);
    uint32_t count = 0;
    if (!s) return 0;
    advance_to_now(s);
    if (cvgp_pit_irq0_pending(&s->pit, s->generation, &count) != CVGP_OK)
        return 0;
    return count;
}

uint32_t CVCLK_CALL cvclock_consume(uint32_t vm)
{
    cvclock_state *s = clock_for(vm);
    int result;
    if (!s) return 0;
    advance_to_now(s);
    result = cvgp_pit_irq0_consume(&s->pit, s->generation);
    return result == 1 ? 1u : 0u;
}

uint32_t CVCLK_CALL cvclock_restore(uint32_t vm)
{
    cvclock_state *s = clock_for(vm);
    uint32_t reload, control;
    if (!s) return 0x00360000UL;        /* BIOS channel-0 default: 65536/mode 3 */
    advance_to_now(s);
    reload = s->pit.pit[0].reload;
    control = 0x30u | ((uint32_t)(s->pit.pit[0].mode & 7u) << 1);
    return (reload & 0xffffu) | (control << 16);
}
