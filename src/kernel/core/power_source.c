/* Exclusive, cached power provider arbitration. No firmware in queries.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/cpu.h>
#include <ciuki/power_source.h>

#define STATUS_UNKNOWN {.ac = POWER_SOURCE_UNKNOWN, .percent = POWER_SOURCE_UNKNOWN, \
                        .seconds = POWER_SOURCE_UNKNOWN, .battery_flags = POWER_SOURCE_UNKNOWN}
static struct power_source_snapshot source = {.status = STATUS_UNKNOWN};
static struct power_source_ops provider_ops;
static struct power_source_platform platform_ops;
static bool platform_ready, off_running, stopped;
/* ACPI's description (any ownership state); APM records are synthesized. */
static struct ciuki_power_status acpi_record;
static bool acpi_record_valid;
static uint32_t summary_sequence;
/* fx-18: the ACPI inventory's decision and a requested APM -> ACPI handoff. */
static bool acpi_settled, handoff_pending;
static uint32_t acpi_ownership = CIUKI_POWER_PENDING;

int power_source_set_platform(const struct power_source_platform *p)
{
    if (!p || !p->quiesce_dma || !p->resume) return -EINVAL;
    uint32_t f = irq_save();
    /* fx-18: installed on every machine (only APM uses it); an ACPI
     * reservation taken first does not prevent a later APM admission. */
    int e = platform_ready || source.provider == POWER_SOURCE_APM ? -EBUSY : 0;
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
    /* fx-18: ACPI ownership replaces a healthy APM connection: APM's worker
     * disconnects and releases, then this reservation succeeds. */
    if (e && kind == POWER_SOURCE_ACPI && source.provider == POWER_SOURCE_APM && source.active &&
        !source.error && !off_running && !stopped) { handoff_pending = true; e = -EAGAIN; }
    if (!e && kind == POWER_SOURCE_APM && handoff_pending) e = -EBUSY;
    if (!e) {
        source.provider = kind; source.active = false; source.error = 0;
        source.events = 0; source.last_event = source.event_info = 0;
        provider_ops = *ops;
        if (kind == POWER_SOURCE_ACPI) handoff_pending = false;
    }
    irq_restore(f);
    return e;
}
bool power_source_handoff_requested(enum power_source_kind kind)
{
    uint32_t f = irq_save(); bool r = handoff_pending && source.provider == kind && kind == POWER_SOURCE_APM; irq_restore(f);
    return r;
}
void power_source_handoff_cancel(void)
{
    uint32_t f = irq_save(); handoff_pending = false; irq_restore(f);
}
void power_source_acpi_settled(uint32_t ownership)
{
    uint32_t f = irq_save();
    if (!acpi_settled) { acpi_settled = true; acpi_ownership = ownership; }
    irq_restore(f);
}
bool power_source_acpi_decided(void) { return acpi_settled; }
bool power_source_apm_admitted(bool acpi_tables)
{
    uint32_t f = irq_save();
    bool ok = !handoff_pending && (source.provider == POWER_SOURCE_NONE || source.provider == POWER_SOURCE_APM) &&
              (!acpi_tables || (acpi_settled && acpi_ownership == CIUKI_POWER_FIRMWARE));
    irq_restore(f);
    return ok;
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
        source.status = (struct power_source_status)STATUS_UNKNOWN;
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
        if (source.status.units > POWER_SOURCE_BATTERIES) source.status.units = POWER_SOURCE_BATTERIES;
        for (unsigned i = 0; i < POWER_SOURCE_BATTERIES; i++)
            if (source.status.battery[i].percent > 100) source.status.battery[i].percent = POWER_SOURCE_UNKNOWN;
        summary_sequence++;
    }
    irq_restore(f);
}
/* G11: boot-monotonic counters for call 73 EVENTS (never reset by a new
 * reservation). APM 1.2 4.3: 0001h/0002h system and 0009h/000Ah user
 * standby/suspend requests, 0005h battery low, 0008h critical suspend,
 * 0003h/0004h/000Bh resume notifications, 0006h/000Ch status changes. */
static struct power_source_counters counters;
/* Weak: host harnesses link this arbiter without core/power_quiesce.c. */
bool power_devices_resume_ready(void) __attribute__((weak));
static void count(uint16_t event)
{
    switch (event) {
    case 1: case 2: case 9: case 10: counters.sleep_requests++; break;
    case 5: counters.battery_low++; break;
    case 8: counters.critical++; break;
    case 3: case 4: case 11: counters.resumes++; break;
    case 6: case 12: counters.status_changes++; break;
    default: break;
    }
}
void power_source_event(enum power_source_kind kind, uint16_t event, uint16_t info)
{
    uint32_t f = irq_save();
    if (source.active && source.provider == kind) {
        source.last_event = event; source.event_info = info; source.events++;
        count(event);
    }
    irq_restore(f);
}
void power_source_counters(struct power_source_counters *out)
{
    if (!out) return;
    uint32_t f = irq_save(); *out = counters; irq_restore(f);
}
uint32_t power_source_standby_reason(void)
{
    uint32_t f = irq_save();
    struct power_source_ops ops = provider_ops;
    bool active = source.active && !stopped && !off_running;
    irq_restore(f);
    if (!active) return CIUKI_POWER_STANDBY_NO_PROVIDER;
    if (!ops.can_standby || !ops.standby || !ops.can_standby(ops.ctx)) return CIUKI_POWER_STANDBY_NO_FIRMWARE;
    if (!power_devices_resume_ready || !power_devices_resume_ready()) return CIUKI_POWER_STANDBY_NO_RESUME;
    return CIUKI_POWER_STANDBY_AVAILABLE;
}
int power_source_standby(void)
{
    if (power_source_standby_reason() != CIUKI_POWER_STANDBY_AVAILABLE) return -ENOTSUP;
    uint32_t f = irq_save();
    if (off_running || stopped || handoff_pending || !source.active) { irq_restore(f); return -EBUSY; }
    struct power_source_ops ops = provider_ops; off_running = true; irq_restore(f);
    int e = ops.standby ? ops.standby(ops.ctx) : -ENOTSUP;
    f = irq_save(); off_running = false; irq_restore(f);
    return e;
}
int power_source_cancel_standby(void)
{
    uint32_t f=irq_save();
    if (off_running || stopped) { irq_restore(f); return -EBUSY; }
    struct power_source_ops ops=provider_ops;
    bool active=source.active && ops.cancel_standby;
    if (active) off_running=true;
    irq_restore(f);
    int e=active ? ops.cancel_standby(ops.ctx) : 0;
    f=irq_save();if (active) off_running=false;irq_restore(f);
    return e;
}
/* Charge only when every present battery reports the same unit and known
 * values: capacities are never combined across different units. */
static uint32_t record_percent(const struct ciuki_power_status *p)
{
    uint64_t full = 0, remaining = 0; uint32_t unit = POWER_SOURCE_UNKNOWN;
    for (unsigned i = 0; i < p->battery_count; i++) {
        const struct ciuki_power_battery *b = &p->batteries[i];
        if (b->present == 0) continue; /* fx-18: an empty bay is not unknown charge */
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
        source.status = (struct power_source_status){.ac = p->ac_online <= 1 ? p->ac_online : POWER_SOURCE_UNKNOWN,
            .percent = record_percent(p), .seconds = POWER_SOURCE_UNKNOWN, .battery_flags = POWER_SOURCE_UNKNOWN};
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
/* APM 1.2 section 4.6.11 (Get Power Status): BX=0001h reports the
 * aggregate of all batteries, BX=80nnh battery unit nn as a percentage and
 * remaining life; apm_status_decode validated the fields. */
static uint32_t apm_state(uint32_t flags, uint32_t ac)
{
    /* Charging and critical are reported; discharging only follows from a
     * known AC state (off-line or backup power). Otherwise it stays unknown. */
    if (flags == POWER_SOURCE_UNKNOWN) return CIUKI_POWER_UNKNOWN;
    uint32_t critical = flags & 4 ? 4u : 0u;
    if (flags & 8) return 2u | critical;
    if (ac == 1) return critical;
    if (ac == 0 || ac == 2) return 1u | critical;
    return CIUKI_POWER_UNKNOWN;
}
static void apm_battery(struct ciuki_power_battery *b, uint32_t id, uint32_t present, uint32_t percent,
                        uint32_t seconds, uint32_t flags, uint32_t ac)
{
    memset(b, 0xff, sizeof(*b));
    b->id = id; b->present = present;
    if (present == 0) return; /* an empty bay: nothing else is known */
    b->unit = CIUKI_POWER_UNIT_PERCENT; b->full = 100;
    b->remaining = percent; b->seconds = seconds;
    b->state = apm_state(flags, ac);
}
static void apm_record(struct ciuki_power_status *out, const struct power_source_snapshot *s,
                       uint32_t requests, uint32_t sequence, const struct ciuki_power_status *acpi)
{
    memset(out, 0xff, sizeof(*out));
    out->version = CIUKI_POWER_VERSION; out->size = sizeof(*out);
    out->provider = CIUKI_POWER_PROVIDER_APM;
    out->ownership = s->active ? CIUKI_POWER_OWNED : s->error ? CIUKI_POWER_QUARANTINED : CIUKI_POWER_PENDING;
    out->error = s->error < 0 ? (uint32_t)-s->error : 0;
    out->sequence = sequence; out->shutdown_requests = requests;
    out->ac_online = s->status.ac <= CIUKI_POWER_AC_BACKUP ? s->status.ac : CIUKI_POWER_UNKNOWN;
    out->zone_count = 0; out->event_overflows = 0; out->reserved = 0;
    /* fx-18: lid and thermal descriptions from ACPI's firmware-owned
     * record when one exists (APM reports neither). */
    if (acpi) {
        out->lid_open = acpi->lid_open;
        out->zone_count = acpi->zone_count <= CIUKI_POWER_ZONES ? acpi->zone_count : 0;
        memcpy(out->zones, acpi->zones, sizeof(out->zones));
    }
    out->battery_count = 0;
    if (!s->active) return;
    uint32_t flags = s->status.battery_flags;
    if (s->status.units) {
        for (unsigned i = 0; i < s->status.units && i < CIUKI_POWER_BATTERIES; i++) {
            const struct power_source_battery *u = &s->status.battery[i];
            apm_battery(&out->batteries[out->battery_count++], i + 1, u->present == 0 ? 0u : 1u,
                        u->percent, u->seconds, u->flags, s->status.ac);
        }
        return;
    }
    bool absent = flags != POWER_SOURCE_UNKNOWN && (flags & 0x80);
    bool known = flags != POWER_SOURCE_UNKNOWN || s->status.percent != POWER_SOURCE_UNKNOWN ||
                 s->status.seconds != POWER_SOURCE_UNKNOWN;
    if (absent || !known) return;
    out->battery_count = 1;
    apm_battery(&out->batteries[0], 1, 1, s->status.percent, s->status.seconds, flags, s->status.ac);
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
    struct ciuki_power_status acpi = acpi_record;
    bool acpi_valid = acpi_record_valid;
    irq_restore(f);
    apm_record(out, &s, requests, sequence, acpi_valid ? &acpi : NULL);
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
/* G11: SYSTEM_POWER_STATUS (learn.microsoft.com, ns-winbase-system_power_status):
 * BatteryFlag 1 high (>66 %), 2 low (<33 %), 4 critical (<5 %), 8 charging,
 * 128 no system battery, 255 unknown; percent 255 and seconds -1 unknown;
 * the remaining time is -1 on AC. Batteries combine as record_percent does. */
void power_source_win32_status(struct power_source_win32 *out)
{
    struct ciuki_power_status p; power_source_status(&p);
    *out = (struct power_source_win32){255, 255, 255, UINT32_MAX};
    if (p.version != CIUKI_POWER_VERSION || p.battery_count > CIUKI_POWER_BATTERIES) return;
    out->ac = p.ac_online == 1 ? 1u : p.ac_online == 0 || p.ac_online == CIUKI_POWER_AC_BACKUP ? 0u : 255u;
    unsigned present = 0, charging = 0, critical = 0;
    uint32_t seconds = UINT32_MAX;
    for (unsigned i = 0; i < p.battery_count; i++) {
        const struct ciuki_power_battery *b = &p.batteries[i];
        if (b->present != 1) continue;
        present++;
        if (b->state == CIUKI_POWER_UNKNOWN) continue;
        if (b->state & 2) charging = 1;
        if (b->state & 4) critical = 1;
        if ((b->state & 1) && b->seconds != CIUKI_POWER_UNKNOWN && (seconds == UINT32_MAX || b->seconds > seconds)) seconds = b->seconds;
    }
    uint32_t percent = record_percent(&p);
    if (percent <= 100) out->percent = percent;
    if (!present) out->battery_flag = p.battery_count || out->ac != 255 ? 128u : 255u;
    else if (percent > 100) out->battery_flag = charging ? 8u : 255u;
    else out->battery_flag = (percent > 66 ? 1u : percent < 33 ? 2u : 0u) | (percent < 5 || critical ? 4u : 0u) | (charging ? 8u : 0u);
    if (out->ac != 1 && present) out->seconds = seconds;
}
