/* Independent APM 1.2 protocol implementation; primary-source register and
 * policy decisions are in docs/design/peripheral-driver-coverage.md,
 * "f1-121 implementation boundary". No donor implementation is imported.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include "apm.h"

static void unknown(struct apm_status *s)
{
    *s = (struct apm_status){APM_UNKNOWN, APM_UNKNOWN, APM_UNKNOWN, APM_UNKNOWN, 0, {{0}}};
    for (unsigned i = 0; i < APM_UNITS; i++)
        s->unit[i] = (struct apm_unit){APM_UNKNOWN, APM_UNKNOWN, APM_UNKNOWN, APM_UNKNOWN};
}

void apm_backend_init(struct apm_backend *b, const struct apm_ops *ops)
{
    memset(b, 0, sizeof(*b));
    if (ops) b->ops = *ops;
    unknown(&b->status);
}

/* No ring-0 far calls: every operation uses the same contained transport. */
static int call(struct apm_backend *b, uint16_t function,
                uint16_t device, uint16_t value, uint32_t ms, struct biosvm_regs *r)
{
    *r = (struct biosvm_regs){.interrupt = 0x15,
                             .eax = function, .ebx = device, .ecx = value};
    uint64_t start = b->ops.now_us ? b->ops.now_us(b->ops.ctx) : 0;
    int e = b->ops.real_call(b->ops.ctx, r, ms);
    if (e == -EAGAIN) b->cost.skipped++; /* fx-18: EC hold busy, nothing ran */
    else {
        uint64_t us = b->ops.now_us ? b->ops.now_us(b->ops.ctx) - start : 0;
        b->cost.calls++; b->cost.total_us += us;
        if (us > b->cost.max_us) b->cost.max_us = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
        if (function == 0x530b) b->cost.event_calls++;
        if (function == 0x530a) b->cost.status_calls++;
    }
    b->transport_error = e;
    b->bios_error = !e && (r->flags & V86_CF) ? (uint8_t)(r->eax >> 8) : 0;
    if (e) return e;
    return (r->flags & V86_CF) ? -EIO : 0;
}
void apm_backend_cost(struct apm_backend *b, struct apm_cost *out)
{
    if (!b || !out) return;
    *out = b->cost; b->cost = (struct apm_cost){0};
}

static int failed(struct apm_backend *b, int error)
{
    /* Disconnect once, even after transport failure; a quarantined broker
     * refuses safely. Never disconnect a BIOS-refused foreign connection. */
    bool uncertain = b->reserved;
    if (b->connected) {
        struct biosvm_regs r;
        uncertain = call(b, 0x5304, 0, 0, APM_STATUS_MS, &r) != 0;
        b->connected = false;
    }
    b->state = APM_FAILED;
    b->error = error;
    unknown(&b->status);
    if (b->ops.publish) b->ops.publish(b->ops.ctx, &b->status);
    if (b->reserved) {
        b->ops.release(b->ops.ctx, uncertain);
        b->reserved = false; /* quarantine callback owns retained resources */
    }
    return error;
}

int apm_backend_probe(struct apm_backend *b)
{
    if (!b || !b->ops.real_call || !b->ops.acpi_present) return -EINVAL;
    if (b->state != APM_UNPROBED) return -EBUSY;
    struct biosvm_regs r;
    int e = call(b, 0x5300, 0, 0, APM_STATUS_MS, &r);
    if (e == -EAGAIN) return -EAGAIN; /* nothing ran; still unprobed */
    b->state = APM_ABSENT;
    if (e) return b->error = e;
    if ((r.ebx & 0xffff) != 0x504d) return b->error = -ENODEV;
    b->version = (uint16_t)r.eax;
    b->flags = (uint16_t)r.ecx;
    /* Exclude any ACPI presence, including an incomplete takeover. The
     * installation check itself neither connects nor writes PM registers. */
    if (b->ops.acpi_present(b->ops.ctx)) {
        b->state = APM_EXCLUDED;
        return b->error = -EBUSY;
    }
    if (b->version != APM_VERSION || (b->flags & ~31u))
        return b->error = -ENOTSUP;
    b->state = APM_DISCOVERED;
    return 0;
}

/* APM 1.2 4.6.11: BH AC line, BL battery status, CH battery flag, CL
 * percentage, DX remaining life (bit 15 minutes). fx-18: when CH is
 * unknown (FFh, as APM 1.0 BIOSes report it) the battery status BL
 * (00h high, 01h low, 02h critical, 03h charging) supplies the flag. */
static void decode(const struct biosvm_regs *r, uint32_t *ac_out, uint32_t *flags_out,
                   uint32_t *percent_out, uint32_t *seconds_out)
{
    *ac_out = *flags_out = *percent_out = *seconds_out = APM_UNKNOWN;
    if (r->flags & V86_CF) return;
    uint8_t ac = (uint8_t)(r->ebx >> 8), status = (uint8_t)r->ebx, percent = (uint8_t)r->ecx;
    uint8_t flags = (uint8_t)(r->ecx >> 8);
    uint16_t life = (uint16_t)r->edx;
    if (ac <= 2) *ac_out = ac;
    if (flags != 0xff && !(flags & 0x60)) *flags_out = flags;
    else if (flags == 0xff && status <= 3) *flags_out = 1u << status;
    if (*flags_out != APM_UNKNOWN && (flags & 0x90) && flags != 0xff) return;
    if (percent <= 100) *percent_out = percent;
    if (life != 0xffff) *seconds_out = (life & 0x7fff) * ((life & 0x8000) ? 60u : 1u);
}
void apm_status_decode(const struct biosvm_regs *r, struct apm_status *s)
{
    unknown(s);
    decode(r, &s->ac, &s->battery_flags, &s->percent, &s->seconds);
}
void apm_unit_decode(const struct biosvm_regs *r, struct apm_unit *u)
{
    uint32_t ac;
    *u = (struct apm_unit){APM_UNKNOWN, APM_UNKNOWN, APM_UNKNOWN, APM_UNKNOWN};
    decode(r, &ac, &u->flags, &u->percent, &u->seconds);
    if (u->flags != APM_UNKNOWN) u->present = (u->flags & 0x10) ? 0u : 1u;
    else if (u->percent != APM_UNKNOWN || u->seconds != APM_UNKNOWN) u->present = 1;
}

int apm_backend_connect(struct apm_backend *b)
{
    if (!b) return -EINVAL;
    if (b->state != APM_DISCOVERED) return -EBUSY;
    if (b->ops.acpi_present(b->ops.ctx)) return b->error = -EBUSY;
    if (!b->ops.reserve || !b->ops.release ||
        !b->ops.now_ms || !b->ops.step || !b->ops.resume || !b->ops.publish || !b->ops.orderly_shutdown)
        return b->error = -ENOTSUP;
    int e = b->ops.reserve(b->ops.ctx);
    if (e) return b->error = e;
    b->reserved = true;
    /* reserve serializes the ACPI/APM owner, not just this backend. */
    if (b->ops.acpi_present(b->ops.ctx)) {
        b->ops.release(b->ops.ctx, false); b->reserved = false;
        return b->error = -EBUSY;
    }
    struct biosvm_regs r;
    e = call(b, 0x5301, 0, 0, APM_POWER_MS, &r);
    if (e == -EAGAIN) { /* nothing ran: retry later from DISCOVERED */
        b->ops.release(b->ops.ctx, false); b->reserved = false;
        return -EAGAIN;
    }
    if (e) {
        /* A BIOS-level refusal did not create our connection. In particular
         * never disconnect a foreign pre-existing connection (AH=02/05/07). */
        if (!b->transport_error && (r.flags & V86_CF)) {
            b->ops.release(b->ops.ctx, false); b->reserved = false;
        }
        if (b->transport_error) b->connected = true;
        return failed(b, e);
    }
    b->connected = true;
    e = call(b, 0x530e, 0, APM_VERSION, APM_STATUS_MS, &r);
    if (!e && (r.eax & 0xffff) != APM_VERSION) e = -ENOTSUP;
    if (!e && (b->flags & 8))
        e = call(b, 0x5308, 1, 1, APM_STATUS_MS, &r);
    if (!e && (b->flags & 16))
        e = call(b, 0x530f, 1, 1, APM_STATUS_MS, &r);
    if (e) return failed(b, e);
    b->state = APM_CONNECTED;
    b->next_poll = b->ops.now_ms(b->ops.ctx);
    b->next_status = b->next_poll;
    b->status_due = b->probe_units = true;
    b->units = 0;
    b->error = 0;
    return 0;
}

/* fx-18: battery units 1..APM_UNITS answer BX=80nnh until the first
 * AH=09h (no such socket, APM 1.2 4.6.11). A unit probe that cannot run
 * (EAGAIN) or a transport error leaves the count as it was. */
static int probe_units(struct apm_backend *b)
{
    unsigned n = 0;
    for (; n < APM_UNITS; n++) {
        struct biosvm_regs r;
        int e = call(b, 0x530a, 0x8001 + n, 0, APM_STATUS_MS, &r);
        if (e && (b->transport_error || b->bios_error == 0x09 || b->bios_error == 0x0a)) {
            if (b->transport_error) return e;
            break;
        }
        if (e) break; /* other BIOS refusals: no per-unit facts */
        apm_unit_decode(&r, &b->status.unit[n]);
    }
    b->units = (uint8_t)n; b->probe_units = false;
    return 0;
}
static int status(struct apm_backend *b)
{
    struct biosvm_regs r;
    struct apm_status old = b->status;
    int e = call(b, 0x530a, 1, 0, APM_STATUS_MS, &r);
    if (e == -EAGAIN) return e; /* keep the last facts; retry next turn */
    apm_status_decode(&r, &b->status);
    if (e) unknown(&b->status);
    if (!e && b->probe_units) {
        int p = probe_units(b);
        if (p == -EAGAIN) { b->status = old; return p; }
        if (p) return p;
    } else if (!e) {
        for (unsigned i = 0; i < b->units; i++) {
            int u = call(b, 0x530a, 0x8001 + i, 0, APM_STATUS_MS, &r);
            if (u == -EAGAIN) { b->status = old; return u; }
            if (u && b->transport_error) return u;
            if (u && b->bios_error == 0x09) { b->units = (uint8_t)i; break; } /* socket gone */
            if (u) b->status.unit[i] = (struct apm_unit){APM_UNKNOWN, APM_UNKNOWN, APM_UNKNOWN, APM_UNKNOWN};
            else apm_unit_decode(&r, &b->status.unit[i]);
        }
    }
    b->status.units = e ? 0 : b->units;
    b->ops.publish(b->ops.ctx, &b->status);
    b->status_due = false;
    b->next_status = b->ops.now_ms(b->ops.ctx) + APM_STATUS_REFRESH_MS;
    return e;
}

static void emergency_begin(struct apm_backend *b, uint64_t now, enum apm_emergency kind)
{
    if (b->emergency) {
        /* A critical event cannot extend a critical budget. A battery low
         * already flushing is upgraded: APM 1.2 4.3.9 counts the BIOS's
         * 5 s from the critical notification, so its budget starts now. */
        if (kind != APM_EMERGENCY_CRITICAL || b->emergency_kind == APM_EMERGENCY_CRITICAL) return;
        b->emergency_kind = kind; b->emergency_start = now; b->stage = APM_FLUSH;
        return;
    }
    b->emergency = true;
    b->emergency_kind = kind;
    b->emergency_start = now;
    b->stage = APM_FLUSH;
    b->emergency_error = 0;
}

static int emergency_step(struct apm_backend *b)
{
    uint64_t now = b->ops.now_ms(b->ops.ctx);
    if (b->stage != APM_ACK) {
        uint64_t end = b->emergency_start +
            (b->stage == APM_FLUSH ? APM_FLUSH_MS : APM_QUIESCE_MS);
        /* The callback must observe an expired deadline too: STOP_DMA's
         * failure path retains uncertain claims and must never be skipped. */
        int e = b->ops.step(b->ops.ctx, b->stage, end);
        now = b->ops.now_ms(b->ops.ctx);
        if (e == -EAGAIN && now < end) return 0;
        if (!b->emergency_error && (e || now > end))
            b->emergency_error = e == -EAGAIN || now > end ? -V86_ETIMEDOUT : e;
        if (b->emergency_kind == APM_EMERGENCY_LOW) {
            /* Battery Low (APM 1.2 4.3.6) is a warning: storage is durable,
             * the event is published; never Set Power State. */
            b->emergency = false;
            if (b->ops.low_battery) b->ops.low_battery(b->ops.ctx, b->emergency_error);
            return 0;
        }
        b->stage++;
        if (b->stage != APM_ACK) return 0;
    }
    struct biosvm_regs r;
    /* Critical suspend is never answered with Processing or Rejected.
     * Failed/expired flush is diagnostic, not permission to reject it. */
    if (b->ops.emergency_record) b->ops.emergency_record(b->ops.ctx, b->emergency_error);
    int e = call(b, 0x5307, 1, 2, APM_POWER_MS, &r);
    if (e == -EAGAIN) return 0; /* EC hold busy: acknowledge next turn */
    b->emergency = false;
    if (e) return failed(b, e);
    e = b->ops.resume(b->ops.ctx, 4, 0);
    if (e) return failed(b, e);
    b->next_poll = b->ops.now_ms(b->ops.ctx);
    b->status_due = true;
    return b->emergency_error;
}

int apm_backend_yield(struct apm_backend *b)
{
    if (!b || b->state != APM_CONNECTED) return -ENODEV;
    bool uncertain = false;
    if (b->connected) {
        struct biosvm_regs r;
        uncertain = call(b, 0x5304, 0, 0, APM_STATUS_MS, &r) != 0;
        b->connected = false;
    }
    b->state = APM_EXCLUDED;
    b->error = uncertain ? -EIO : 0;
    unknown(&b->status);
    b->ops.publish(b->ops.ctx, &b->status);
    if (b->reserved) { b->ops.release(b->ops.ctx, uncertain); b->reserved = false; }
    return b->error;
}

int apm_backend_service(struct apm_backend *b)
{
    if (!b || b->state != APM_CONNECTED) return -ENODEV;
    /* fx-18: ACPI ownership replaces APM cleanly (not mid-emergency). */
    if (!b->emergency && b->ops.handoff && b->ops.handoff(b->ops.ctx)) return apm_backend_yield(b);
    if (b->ops.acpi_present(b->ops.ctx)) return failed(b, -EBUSY);
    uint64_t now = b->ops.now_ms(b->ops.ctx);
    if (now >= b->next_poll) {
        b->next_poll = now + APM_POLL_MS;
        unsigned i;
        for (i = 0; i < 8; i++) {
            struct biosvm_regs r;
            int e = call(b, 0x530b, 0, 0, APM_STATUS_MS, &r);
            if (e == -EAGAIN) { b->next_poll = now; break; } /* retry next turn */
            if (e) {
                if (b->bios_error == 0x80) break; /* empty event queue */
                return failed(b, e);
            }
            uint16_t event = (uint16_t)r.ebx, info = (uint16_t)r.ecx;
            b->last_event = event; b->events++; b->cost.events++;
            if (b->ops.event) b->ops.event(b->ops.ctx, event, info);
            switch (event) {
            case 1: case 2: case 9: case 10:
                if (b->rejects_pending < 8) b->rejects_pending++;
                break;
            case 5:
                b->status_due = true;
                emergency_begin(b, b->ops.now_ms(b->ops.ctx), APM_EMERGENCY_LOW);
                break;
            case 8:
                emergency_begin(b, b->ops.now_ms(b->ops.ctx), APM_EMERGENCY_CRITICAL);
                break;
            case 3: case 4: case 7: case 11:
                if (event != 7) b->status_due = true;
                e = b->ops.resume(b->ops.ctx, event, info);
                if (e) return failed(b, e);
                break;
            case 6: b->status_due = true; break;              /* 4.3.7 */
            case 12: b->status_due = b->probe_units = true; break; /* 4.3.13 */
            default: break; /* unknown events grant no power operation */
            }
            /* Avoid unbounded firmware work on a malicious event stream;
             * drain the next batch on the next worker iteration. */
            if (b->ops.now_ms(b->ops.ctx) - now >= APM_STATUS_MS) { i++; break; }
        }
        if (i == 8 || b->ops.now_ms(b->ops.ctx) - now >= APM_STATUS_MS) {
            b->poll_overflows++;
            b->next_poll = b->ops.now_ms(b->ops.ctx);
        }
    }
    /* Standby/suspend requests are rejected (APM 1.2 4.3.2/4.3.3: Set Power
     * State CX=0005h), one answer per request; a busy EC hold retries the
     * remaining answers on the next turn. */
    while (b->rejects_pending) {
        struct biosvm_regs r;
        int e = call(b, 0x5307, 1, 5, APM_STATUS_MS, &r);
        if (e == -EAGAIN) break;
        b->rejects_pending--;
        if (e) return failed(b, e);
    }
    /* A critical suspend is acknowledged before any status call; a battery
     * low may refresh the status while its flush runs. */
    bool critical = b->emergency && b->emergency_kind == APM_EMERGENCY_CRITICAL;
    if (!critical && (b->status_due || b->ops.now_ms(b->ops.ctx) >= b->next_status)) {
        int e = status(b);
        if (e && e != -EAGAIN && (b->transport_error || !b->bios_error)) return failed(b, e);
    }
    return b->emergency ? emergency_step(b) : 0;
}

int apm_backend_off(struct apm_backend *b)
{
    if (!b || b->state != APM_CONNECTED) return -ENODEV;
    if (b->ops.acpi_present(b->ops.ctx)) return failed(b, -EBUSY);
    if (b->emergency) return -EBUSY;
    /* The global owner closes applications, flushes storage and stops DMA.
     * Pending/error proofs never authorize an Off request. */
    uint64_t now = b->ops.now_ms(b->ops.ctx);
    if (!b->off_started) {
        b->off_started = true;
        b->off_deadline = now + APM_QUIESCE_MS;
    }
    if (now >= b->off_deadline) { b->off_started = false; return -V86_ETIMEDOUT; }
    int e = b->ops.orderly_shutdown(b->ops.ctx, b->off_deadline);
    if (e) { if (e != -EAGAIN) b->off_started = false; return e; }
    b->off_started = false;
    if (b->ops.now_ms(b->ops.ctx) >= b->off_deadline) return -V86_ETIMEDOUT;
    if (b->ops.acpi_present(b->ops.ctx)) return failed(b, -EBUSY);
    struct biosvm_regs r;
    e = call(b, 0x5307, 1, 3, APM_POWER_MS, &r);
    /* Successful Off must remove power. Returning cannot count as success. */
    return failed(b, e ? e : -EIO);
}
