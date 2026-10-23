/* Independent APM 1.2 protocol implementation; primary-source register and
 * policy decisions are in docs/design/peripheral-driver-coverage.md,
 * "f1-121 implementation boundary". No donor implementation is imported.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include "apm.h"

static void unknown(struct apm_status *s)
{
    *s = (struct apm_status){APM_UNKNOWN, APM_UNKNOWN, APM_UNKNOWN, APM_UNKNOWN};
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
    int e = b->ops.real_call(b->ops.ctx, r, ms);
    b->transport_error = e;
    b->bios_error = !e && (r->flags & V86_CF) ? (uint8_t)(r->eax >> 8) : 0;
    if (e) return e;
    return (r->flags & V86_CF) ? -EIO : 0;
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

void apm_status_decode(const struct biosvm_regs *r, struct apm_status *s)
{
    unknown(s);
    if (r->flags & V86_CF) return;
    uint8_t ac = (uint8_t)(r->ebx >> 8), percent = (uint8_t)r->ecx;
    uint8_t flags = (uint8_t)(r->ecx >> 8);
    uint16_t life = (uint16_t)r->edx;
    if (ac <= 2) s->ac = ac;
    if (flags != 0xff && !(flags & 0x60)) s->battery_flags = flags;
    if (s->battery_flags != APM_UNKNOWN && (flags & 0x90)) return;
    if (percent <= 100) s->percent = percent;
    if (life != 0xffff) s->seconds = (life & 0x7fff) * ((life & 0x8000) ? 60u : 1u);
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
    b->error = 0;
    return 0;
}

static int status(struct apm_backend *b)
{
    struct biosvm_regs r;
    int e = call(b, 0x530a, 1, 0, APM_STATUS_MS, &r);
    apm_status_decode(&r, &b->status);
    if (e) unknown(&b->status);
    b->ops.publish(b->ops.ctx, &b->status);
    return e;
}

static void emergency_begin(struct apm_backend *b, uint64_t now)
{
    if (b->emergency) return; /* a critical event cannot extend the budget */
    b->emergency = true;
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
        b->stage++;
        if (b->stage != APM_ACK) return 0;
    }
    struct biosvm_regs r;
    /* Critical suspend is never answered with Processing or Rejected.
     * Failed/expired flush is diagnostic, not permission to reject it. */
    if (b->ops.emergency_record) b->ops.emergency_record(b->ops.ctx, b->emergency_error);
    int e = call(b, 0x5307, 1, 2, APM_POWER_MS, &r);
    b->emergency = false;
    if (e) return failed(b, e);
    e = b->ops.resume(b->ops.ctx, 4, 0);
    if (e) return failed(b, e);
    b->next_poll = b->ops.now_ms(b->ops.ctx);
    return b->emergency_error;
}

int apm_backend_service(struct apm_backend *b)
{
    if (!b || b->state != APM_CONNECTED) return -ENODEV;
    if (b->ops.acpi_present(b->ops.ctx)) return failed(b, -EBUSY);
    uint64_t now = b->ops.now_ms(b->ops.ctx);
    if (now >= b->next_poll) {
        b->next_poll = now + APM_POLL_MS;
        unsigned i;
        for (i = 0; i < 8; i++) {
            struct biosvm_regs r;
            int e = call(b, 0x530b, 0, 0, APM_STATUS_MS, &r);
            if (e) {
                if (b->bios_error == 0x80) break; /* empty event queue */
                return failed(b, e);
            }
            uint16_t event = (uint16_t)r.ebx, info = (uint16_t)r.ecx;
            b->last_event = event; b->events++;
            if (b->ops.event) b->ops.event(b->ops.ctx, event, info);
            switch (event) {
            case 1: case 2: case 9: case 10:
                e = call(b, 0x5307, 1, 5, APM_STATUS_MS, &r);
                if (e) return failed(b, e);
                break;
            case 5: case 8:
                emergency_begin(b, b->ops.now_ms(b->ops.ctx));
                break;
            case 3: case 4: case 7: case 11:
                e = b->ops.resume(b->ops.ctx, event, info);
                if (e) return failed(b, e);
                break;
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
        if (!b->emergency) {
            int e = status(b);
            if (e && !b->bios_error) return failed(b, e);
        }
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
