/* Exclusive, cached power provider arbitration. No firmware in queries.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/cpu.h>
#include <ciuki/power_source.h>

static struct power_source_snapshot source = {
    .status = {POWER_SOURCE_UNKNOWN, POWER_SOURCE_UNKNOWN,
               POWER_SOURCE_UNKNOWN, POWER_SOURCE_UNKNOWN}
};
static struct power_source_ops provider_ops;
static struct power_source_platform platform_ops;
static bool platform_ready, off_running, stopped;
/* ACPI's description (any ownership state); APM records are synthesized. */
static struct ciuki_power_status acpi_record;
static bool acpi_record_valid;
static uint32_t summary_sequence;

int power_source_set_platform(const struct power_source_platform *p)
{
    if (!p || !p->quiesce_dma || !p->resume) return -EINVAL;
    uint32_t f = irq_save();
    int e = platform_ready || source.provider != POWER_SOURCE_NONE ? -EBUSY : 0;
    if (!e) { platform_ops = *p; platform_ready = true; }
    irq_restore(f);
    return e;
}
bool power_source_platform_ready(void) { return platform_ready; }
int power_source_quiesce_dma(uint64_t end)
{
    return platform_ready ? platform_ops.quiesce_dma(platform_ops.ctx, end) : -ENOTSUP;
}
int power_source_resume(uint16_t event, uint16_t info)
{
    return platform_ready ? platform_ops.resume(platform_ops.ctx, event, info) : -ENOTSUP;
}
void power_source_mark_stopped(void)
{
    uint32_t f = irq_save(); stopped = true; irq_restore(f);
}
int power_source_orderly_shutdown(uint64_t end)
{
    (void)end; /* the proof is the latch; no shutdown work is started here */
    return stopped ? 0 : -EBUSY;
}
int power_source_reserve(enum power_source_kind kind, const struct power_source_ops *ops)
{
    if ((kind != POWER_SOURCE_ACPI && kind != POWER_SOURCE_APM) || !ops || !ops->soft_off)
        return -EINVAL;
    uint32_t f = irq_save();
    int e = source.provider == POWER_SOURCE_NONE && !off_running ? 0 : -EBUSY;
    if (!e) {
        source.provider = kind; source.active = false; source.error = 0;
        source.events = 0; source.last_event = source.event_info = 0;
        provider_ops = *ops;
    }
    irq_restore(f);
    return e;
}
int power_source_activate(enum power_source_kind kind)
{
    uint32_t f = irq_save();
    int e = kind != POWER_SOURCE_NONE && source.provider == kind && !source.active && !source.error ? 0 : -EBUSY;
    if (!e) source.active = true;
    irq_restore(f);
    return e;
}
void power_source_release(enum power_source_kind kind, bool uncertain, int error)
{
    uint32_t f = irq_save();
    if (kind != POWER_SOURCE_NONE && source.provider == kind) {
        source.active = false;
        source.error = error;
        source.status = (struct power_source_status){POWER_SOURCE_UNKNOWN, POWER_SOURCE_UNKNOWN,
                                                    POWER_SOURCE_UNKNOWN, POWER_SOURCE_UNKNOWN};
        if (!uncertain) source.provider = POWER_SOURCE_NONE;
        provider_ops = (struct power_source_ops){0};
    }
    irq_restore(f);
}
void power_source_publish(enum power_source_kind kind, const struct power_source_status *s)
{
    if (!s) return;
    uint32_t f = irq_save();
    if (source.active && source.provider == kind) {
        source.status = *s;
        if (source.status.ac > 2) source.status.ac = POWER_SOURCE_UNKNOWN;
        if (source.status.percent > 100) source.status.percent = POWER_SOURCE_UNKNOWN;
        summary_sequence++;
    }
    irq_restore(f);
}
void power_source_event(enum power_source_kind kind, uint16_t event, uint16_t info)
{
    uint32_t f = irq_save();
    if (source.active && source.provider == kind) {
        source.last_event = event; source.event_info = info; source.events++;
    }
    irq_restore(f);
}
/* Charge only when every present battery reports the same unit and known
 * values: capacities are never combined across different units. */
static uint32_t record_percent(const struct ciuki_power_status *p)
{
    uint64_t full = 0, remaining = 0; uint32_t unit = POWER_SOURCE_UNKNOWN;
    for (unsigned i = 0; i < p->battery_count; i++) {
        const struct ciuki_power_battery *b = &p->batteries[i];
        if (b->present != 1 || b->unit > CIUKI_POWER_UNIT_PERCENT || !b->full || b->full == CIUKI_POWER_UNKNOWN ||
            b->remaining == CIUKI_POWER_UNKNOWN || (unit != POWER_SOURCE_UNKNOWN && unit != b->unit))
            return POWER_SOURCE_UNKNOWN;
        unit = b->unit; full += b->full; remaining += b->remaining;
    }
    if (!full) return POWER_SOURCE_UNKNOWN;
    return remaining >= full ? 100u : (uint32_t)(remaining * 100u / full);
}
void power_source_publish_record(enum power_source_kind kind, const struct ciuki_power_status *p)
{
    if (kind != POWER_SOURCE_ACPI || !p || p->version != CIUKI_POWER_VERSION || p->size != sizeof(*p) ||
        p->battery_count > CIUKI_POWER_BATTERIES || p->zone_count > CIUKI_POWER_ZONES) return;
    uint32_t f = irq_save();
    acpi_record = *p; acpi_record_valid = true;
    if (source.active && source.provider == POWER_SOURCE_ACPI) {
        source.status = (struct power_source_status){p->ac_online <= 1 ? p->ac_online : POWER_SOURCE_UNKNOWN,
            record_percent(p), POWER_SOURCE_UNKNOWN, POWER_SOURCE_UNKNOWN};
        summary_sequence++;
    }
    irq_restore(f);
}
void power_source_snapshot(struct power_source_snapshot *out)
{
    if (!out) return;
    uint32_t f = irq_save(); *out = source; irq_restore(f);
}
static void pending(struct ciuki_power_status *out)
{
    memset(out, 0xff, sizeof(*out));
    out->version = CIUKI_POWER_VERSION; out->size = sizeof(*out);
    out->ownership = CIUKI_POWER_PENDING; out->battery_count = out->zone_count = 0;
    out->shutdown_requests = out->sequence = out->event_overflows = out->reserved = 0;
}
/* APM 1.2 section 4.6.8 (Get Power Status) reports one system battery as a
 * percentage and remaining life; apm_status_decode validated the fields. */
static void apm_record(struct ciuki_power_status *out, const struct power_source_snapshot *s,
                       uint32_t requests, uint32_t sequence)
{
    memset(out, 0xff, sizeof(*out));
    out->version = CIUKI_POWER_VERSION; out->size = sizeof(*out);
    out->provider = CIUKI_POWER_PROVIDER_APM;
    out->ownership = s->active ? CIUKI_POWER_OWNED : s->error ? CIUKI_POWER_QUARANTINED : CIUKI_POWER_PENDING;
    out->error = s->error < 0 ? (uint32_t)-s->error : 0;
    out->sequence = sequence; out->shutdown_requests = requests;
    out->ac_online = s->status.ac <= CIUKI_POWER_AC_BACKUP ? s->status.ac : CIUKI_POWER_UNKNOWN;
    out->zone_count = 0; out->event_overflows = 0; out->reserved = 0;
    uint32_t flags = s->status.battery_flags;
    bool absent = flags != POWER_SOURCE_UNKNOWN && (flags & 0x80);
    bool known = flags != POWER_SOURCE_UNKNOWN || s->status.percent != POWER_SOURCE_UNKNOWN ||
                 s->status.seconds != POWER_SOURCE_UNKNOWN;
    out->battery_count = !absent && known ? 1 : 0;
    if (!out->battery_count) return;
    struct ciuki_power_battery *b = &out->batteries[0];
    b->id = 1; b->present = 1; b->unit = CIUKI_POWER_UNIT_PERCENT; b->full = 100;
    b->remaining = s->status.percent; b->seconds = s->status.seconds;
    /* Charging and critical are reported; discharging only follows from a
     * known AC state (off-line or backup power). Otherwise it stays unknown. */
    if (flags == POWER_SOURCE_UNKNOWN) return;
    uint32_t critical = flags & 4 ? 4u : 0u;
    if (flags & 8) b->state = 2u | critical;
    else if (s->status.ac == 1) b->state = critical;
    else if (s->status.ac == 0 || s->status.ac == 2) b->state = 1u | critical;
}
void power_source_status(struct ciuki_power_status *out)
{
    if (!out) return;
    uint32_t f = irq_save();
    struct power_source_snapshot s = source;
    uint32_t requests = acpi_record_valid ? acpi_record.shutdown_requests : 0, sequence = summary_sequence;
    if (s.provider != POWER_SOURCE_APM) {
        if (acpi_record_valid) *out = acpi_record; else pending(out);
        out->provider = s.provider == POWER_SOURCE_ACPI ? CIUKI_POWER_PROVIDER_ACPI : CIUKI_POWER_PROVIDER_NONE;
        irq_restore(f);
        return;
    }
    irq_restore(f);
    apm_record(out, &s, requests, sequence);
}
bool power_source_can_off(void)
{
    uint32_t f = irq_save();
    struct power_source_ops ops = provider_ops;
    bool active = source.active;
    irq_restore(f);
    return active && ops.soft_off && (!ops.can_off || ops.can_off(ops.ctx));
}
int power_source_soft_off(void)
{
    uint32_t f = irq_save();
    if (!source.active || !provider_ops.soft_off) { irq_restore(f); return -ENOTSUP; }
    if (off_running || !stopped) { irq_restore(f); return -EBUSY; }
    off_running = true;
    struct power_source_ops ops = provider_ops;
    irq_restore(f);
    int e = ops.soft_off(ops.ctx);
    f = irq_save(); off_running = false; irq_restore(f);
    return e ? e : -EIO; /* returning cannot prove physical power removal */
}
