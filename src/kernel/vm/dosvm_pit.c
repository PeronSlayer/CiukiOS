/* Migrated latch/access machinery from src/vm/guest_peripherals.c; modes and
 * gates follow Intel 8254. See f3-acceptance.md. GPL-2.0-only. */
#include <ciuki/kernel.h>
#include "dosvm_internal.h"

static uint32_t pit_modulus(const struct dosvm_pit_channel *c) { return c->bcd ? 10000u : 65536u; }
static uint16_t pit_encode(const struct dosvm_pit_channel *c, uint32_t n)
{
    if (!c->bcd) return (uint16_t)n; /* binary wrap needs no runtime division */
    n %= 10000u;
    return (n % 10) | ((n / 10 % 10) << 4) | ((n / 100 % 10) << 8) | ((n / 1000) << 12);
}
static uint16_t pit_counter(const struct dosvm_pit_channel *c)
{
    uint32_t n = c->reload - c->phase;
    if (c->mode == 3 && c->reload) {
        unsigned high = (c->reload + 1) / 2;
        unsigned t = c->phase < high ? c->phase : c->phase - high;
        n = c->phase < high ? c->reload - 2 * t : (c->reload & ~1u) - 2 * t;
    }
    return pit_encode(c, n);
}
static void pit_latch(struct dosvm_pit_channel *c, bool count, bool status)
{
    if (count && !c->latched) { c->latch = pit_counter(c); c->latched = true; c->latch_phase = 0; }
    if (status && !c->status_latched) {
        c->status = (c->out << 7) | (c->null_count << 6) | (c->access << 4) | (c->mode << 1) | c->bcd;
        c->status_latched = true;
    }
}
static int pit_load_count(struct dosvm_pit_channel *c, uint16_t raw)
{
    uint32_t n = raw;
    if (c->bcd) {
        n = 0; unsigned power = 1;
        for (unsigned i = 0; i < 4; i++, power *= 10) {
            unsigned digit = (raw >> (4 * i)) & 15;
            if (digit > 9) return -V86_EPERM;
            n += digit * power;
        }
    }
    if (!n) n = pit_modulus(c);
    c->pending = n;
    c->null_count = true;
    if ((c->mode == 2 || c->mode == 3) && c->running && c->gate) return 0;
    if (c->mode == 1 || c->mode == 5) { if (!c->running) c->reload = n; return 0; }
    c->load = true;
    c->running = true;
    return 0;
}
static void pit_gate(struct dosvm_pit_channel *c, bool gate)
{
    bool rising = gate && !c->gate;
    c->gate = gate;
    if (!gate && (c->mode == 2 || c->mode == 3)) { c->out = true; c->running = false; }
    if (rising && (c->mode == 1 || c->mode == 2 || c->mode == 3 || c->mode == 5) && c->pending) {
        c->load = true; c->running = true;
    }
}
void dosvm_pit_init(struct dosvm *v)
{
    memset(&v->pit, 0, sizeof(v->pit));
    for (unsigned i = 0; i < 3; i++) {
        struct dosvm_pit_channel *c = &v->pit.channel[i];
        c->reload = c->pending = 65536; c->access = 3;
        c->mode = i == 1 ? 2 : 3;
        c->gate = i != 2; c->running = i != 2; c->out = true;
    }
}
int dosvm_pit_port(struct dosvm *v, uint16_t port, unsigned width, bool write, uint32_t *value, void *arg)
{
    (void)arg;
    if (width != 1) return -V86_EPERM;
    uint8_t b = *value;
    if (port == 0x61) {
        if (write) { v->pit.speaker = b & 3; pit_gate(&v->pit.channel[2], b & 1); }
        else *value = v->pit.speaker | (v->pit.channel[1].out ? 0x10 : 0) | (v->pit.channel[2].out ? 0x20 : 0);
        return 0;
    }
    if (port == 0x43) {
        if (!write) { *value = 0xFF; return 0; }
        unsigned ch = b >> 6, access = b >> 4 & 3;
        if (ch == 3) {
            if (b & 1) return -V86_EPERM; /* reserved */
            for (unsigned i = 0; i < 3; i++) if (b & (2u << i))
                pit_latch(&v->pit.channel[i], !(b & 0x20), !(b & 0x10));
            return 0;
        }
        struct dosvm_pit_channel *c = &v->pit.channel[ch];
        if (!access) { pit_latch(c, true, false); return 0; }
        if (!ch) v->pit.owed = 0; /* a reprogrammed timer owes nothing */
        bool gate = c->gate;
        memset(c, 0, sizeof(*c));
        c->gate = gate; c->access = access; c->mode = b >> 1 & 7;
        if (c->mode >= 6) c->mode -= 4;
        c->bcd = b & 1; c->null_count = true; c->out = c->mode != 0;
        return 0;
    }
    if (port < 0x40 || port > 0x42) return -ENOSYS;
    struct dosvm_pit_channel *c = &v->pit.channel[port - 0x40];
    if (!write) {
        if (c->status_latched) { c->status_latched = false; *value = c->status; return 0; }
        uint16_t n = c->latched ? c->latch : pit_counter(c);
        uint8_t *phase = c->latched ? &c->latch_phase : &c->read_phase;
        *value = c->access == 2 || (c->access == 3 && *phase) ? n >> 8 : n & 255;
        if (c->access == 3) { *phase ^= 1; if (!*phase) c->latched = false; }
        else c->latched = false;
        return 0;
    }
    if (c->access == 1) return pit_load_count(c, b);
    if (c->access == 2) return pit_load_count(c, (uint16_t)b << 8);
    if (!c->write_phase) {
        c->write_byte = b; c->write_phase = 1;
        if (c->mode == 0) { c->running = false; c->out = false; }
        return 0;
    }
    int e = pit_load_count(c, c->write_byte | (uint16_t)b << 8);
    if (!e) c->write_phase = 0;
    return e;
}
static uint32_t pit_advance_channel(struct dosvm_pit_channel *c, uint32_t clocks)
{
    if (!clocks || !c->running) return 0;
    if (!c->gate && (c->mode == 0 || c->mode == 4)) return 0;
    if (c->load) {
        c->reload = c->pending; c->phase = 0; c->null_count = false; c->load = false;
        c->out = c->mode != 0 && c->mode != 1;
        if (!--clocks) return 0; /* count transfer uses one CLK */
    }
    if (!c->reload) return 0;
    unsigned edges = 0;
    if (c->mode == 2 || c->mode == 3) {
        /* A pending periodic count takes effect at the next reload/half cycle. */
        if (c->null_count) {
            uint32_t boundary = c->mode == 3 && c->phase < (c->reload + 1) / 2 ?
                                (c->reload + 1) / 2 : c->reload;
            uint32_t distance = boundary - c->phase;
            if (clocks >= distance) {
                clocks -= distance;
                bool low_half = c->mode == 3 && boundary != c->reload;
                if (!low_half) edges++;
                c->reload = c->pending; c->null_count = false;
                c->phase = low_half ? (c->reload + 1) / 2 : 0;
            }
        }
        uint32_t total = c->phase + clocks;
        if (total < c->reload) c->phase = total;
        else {
            edges += total / c->reload;
            c->phase = total % c->reload;
        }
        c->out = c->mode == 2 ? c->phase != c->reload - 1 : c->phase < (c->reload + 1) / 2;
    } else {
        uint32_t old = c->phase;
        c->phase += clocks;
        if (c->mode == 0 || c->mode == 1) {
            if (old < c->reload && c->phase >= c->reload) { c->out = true; edges = 1; }
        } else { /* modes 4/5: one-clock low strobe then a rising edge */
            c->out = c->phase != c->reload;
            if (old <= c->reload && c->phase > c->reload) edges = 1;
        }
        if (c->phase > c->reload + pit_modulus(c)) c->phase = c->reload + 1 +
            (c->phase - c->reload - 1) % pit_modulus(c);
    }
    return edges;
}
/* fx-16: an 8259A latches one request per line, so an edge that arrives
 * while IRQ0 is still requested (not yet acknowledged), or a second edge
 * of one late clock turn, used to vanish. On hardware the CPU takes IRQ0
 * within microseconds; here the VM may be off the CPU or inside a monitor
 * service for several PIT periods (T23 trial e831e685: cadence_clock
 * gap_max_ms 8-9 against DMX's 7.1 ms period, the VM descheduled
 * 44-382 ms per 5 s), and every lost edge is a lost DOOM/DMX tick: game
 * time and music tempo slip, then jump. Like KVM's i8254 in its default
 * reinject mode (linux arch/x86/kvm/i8254.c: pending ticks, injected
 * again after the ack), the edge is owed and re-raised at a later turn,
 * paced (below); DOSBox needs none of this because its PIT events are
 * placed in emulated time (hardware/timer.cpp PIT0_Event). A masked IRQ0
 * latches as before and owes nothing. */
static void pit_irq0(struct dosvm *v, uint32_t edges)
{
    uint32_t flags = irq_save();
    bool masked = v->monitor.pic[0].imr & 1u;
    bool requested = v->monitor.pic[0].irr & 1u;
    bool in_service = v->monitor.pic[0].isr & 1u;
    irq_restore(flags);
    if (masked) {
        v->pit.owed = 0;
        if (edges) { dosvm_irq_raise(v->info.key, 0); v->pit.irq0_clock = v->pit.clocks; }
        return;
    }
    uint32_t owed = v->pit.owed + edges;
    if (!owed) return;
    /* A new edge raises as on the 8259A. An owed one waits until the guest
     * has finished the previous tick (not requested, not in service) and
     * half a period has passed: a handler slower than its own timer must
     * not be re-entered back to back (a livelock reinjection can cause). */
    uint32_t half = v->pit.channel[0].reload / 2u;
    bool paced = edges || (!in_service && v->pit.clocks - v->pit.irq0_clock >= half);
    if (!requested && paced) {
        dosvm_irq_raise(v->info.key, 0);
        v->pit.irq0_clock = v->pit.clocks;
        if (!edges) v->pit.reinjected++;
        owed--;
    }
    if (owed > DOSVM_PIT_OWED_MAX) { v->pit.lost_edges += owed - DOSVM_PIT_OWED_MAX; owed = DOSVM_PIT_OWED_MAX; }
    if (owed > v->pit.owed_max) v->pit.owed_max = owed;
    v->pit.owed = owed;
}
void dosvm_pit_advance(struct dosvm *v, uint64_t ticks)
{
    if (ticks > DOSVM_PIT_CATCHUP) { v->pit.dropped_ticks += ticks - DOSVM_PIT_CATCHUP; ticks = DOSVM_PIT_CATCHUP; }
    uint32_t clocks = (uint32_t)ticks * 1193u;
    v->pit.clocks += clocks;
    for (unsigned i = 0; i < 3; i++) {
        uint32_t edges = pit_advance_channel(&v->pit.channel[i], clocks);
        if (!i) { v->pit.edges += edges; pit_irq0(v, edges); }
    }
}
