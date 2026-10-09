/* Host-side tests for the per-VM session PIT clock. Not part of CVSESSION. */
#include "session_clock.h"
#include <stdint.h>
#include <stdio.h>

static uint32_t fake_low, fake_high;
static unsigned checks, failures;

void CVCLK_CALL cvclock_now(uint32_t *low, uint32_t *high)
{
    *low = fake_low;
    *high = fake_high;
}

uint32_t CVCLK_CALL cvclock_div(uint32_t high, uint32_t low,
                                uint32_t divisor, uint32_t *remainder)
{
    uint64_t value = ((uint64_t)high << 32) | low;
    *remainder = (uint32_t)(value % divisor);
    return (uint32_t)(value / divisor);
}

static void expect(uint32_t got, uint32_t want)
{
    ++checks;
    if (got != want) {
        ++failures;
        fprintf(stderr, "session_clock check %u: got %lu, want %lu\n",
                checks, (unsigned long)got, (unsigned long)want);
    }
}

static void advance_cycles(uint32_t low, uint32_t high)
{
    fake_low += low;
    fake_high += high + (fake_low < low);
}

static uint16_t read_latched_count(uint32_t vm)
{
    uint16_t lo, hi;
    (void)cvclock_port(vm, 0x43, 1, 0x00); /* latch channel 0 */
    lo = (uint16_t)cvclock_port(vm, 0x40, 0, 0);
    hi = (uint16_t)cvclock_port(vm, 0x40, 0, 0);
    return (uint16_t)(lo | (hi << 8));
}

int main(void)
{
    uint32_t i, restore;
    uint64_t long_pit_clocks;

    fake_low = fake_high = 0;
    cvclock_begin(0, 1000000UL); /* 1,000,000 TSC cycles per millisecond */
    cvclock_begin(1, 1000000UL);

    /* Repeated one-microsecond polls retain both cycle and PIT fractions. */
    for (i = 0; i < 1000; ++i) {
        advance_cycles(1000, 0);
        (void)cvclock_pending(0);
    }
    expect(read_latched_count(0), 65536u - 2u * 1193u);

    /* VM 1 was not polled while VM 0 ran; it accrues its own full elapsed time. */
    advance_cycles(14000000UL, 0);      /* total elapsed: 15 ms */
    expect(cvclock_pending(0), 0);
    expect(cvclock_pending(1), 0);

    /* A different ch0 divisor changes VM 0 expiry timing only. */
    (void)cvclock_port(0, 0x43, 1, 0x36); /* lo/hi, mode 3 */
    (void)cvclock_port(0, 0x40, 1, 0x9c); /* 11932 PIT clocks */
    (void)cvclock_port(0, 0x40, 1, 0x2e);
    restore = cvclock_restore(0);
    expect(restore & 0xffffu, 11932u);
    expect((restore >> 16) & 0xffu, 0x36u);
    advance_cycles(15000000UL, 0);       /* 15 more ms */
    expect(cvclock_pending(0), 1);
    expect(cvclock_pending(1), 0);

    /* Counter phase follows the whole gap, while the PIC sees one edge. */
    cvclock_end(2);
    cvclock_begin(2, 1000000UL);
    advance_cycles(500000000UL, 0);
    expect(cvclock_pending(2), 1);
    expect(read_latched_count(2), 65536u - 2u * (596591u % 65536u));
    expect(cvclock_consume(2), 1);
    expect(cvclock_pending(2), 0);
    expect(cvclock_consume(2), 0);

    /* 1193 Hz over five seconds must not replay thousands of IRQ0s ahead
     * of the keyboard/audio. Consuming does not alter the counter phase. */
    (void)cvclock_port(2, 0x43, 1, 0x34);
    (void)cvclock_port(2, 0x40, 1, 0xe8);
    (void)cvclock_port(2, 0x40, 1, 0x03);
    advance_cycles(705032704UL, 1);   /* 5,000,000,000 cycles = 5000 ms */
    expect(cvclock_pending(2), 1);
    expect(read_latched_count(2), 90u); /* 5*1193182 clocks, mod 1000 = 910 */
    expect(cvclock_consume(2), 1);
    expect(read_latched_count(2), 90u);
    expect(cvclock_pending(2), 0);
    for (i = 0; i < 10; ++i) expect(cvclock_consume(2), 0);
    advance_cycles(1000000UL, 0);
    expect(cvclock_pending(2), 1);
    expect(cvclock_consume(2), 1);
    expect(cvclock_pending(2), 0);

    /* Ending and beginning a VM discards the prior owner's expiry debt. */
    cvclock_end(2);
    cvclock_begin(2, 1000000UL);
    expect(cvclock_pending(2), 0);

    /* The quotient's high word is nonzero here. It must be split before the
     * assembly 64/32 DIV helper so the division cannot fault on overflow. */
    fake_low = fake_high = 0;
    cvclock_begin(3, 1000000UL);
    fake_high = 1000000UL;
    fake_low = 0;
    long_pit_clocks = (((uint64_t)1u << 32) * 1193182u) / 1000u;
    expect(cvclock_pending(3), 1);
    expect(read_latched_count(3), (65536u - 2u * (uint32_t)(long_pit_clocks % 65536u)) & 0xffffu);
    expect(cvclock_consume(3), 1);
    expect(cvclock_consume(3), 0);

    cvclock_end(0);
    cvclock_end(1);
    cvclock_end(2);
    cvclock_end(3);
    if (!failures) printf("session_clock: %u checks passed\n", checks);
    return failures ? 1 : 0;
}
