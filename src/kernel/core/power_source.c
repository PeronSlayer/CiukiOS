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
static bool platform_ready, off_running;

int power_source_set_platform(const struct power_source_platform *p)
{
    if (!p || !p->quiesce_dma || !p->resume || !p->orderly_shutdown) return -EINVAL;
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
int power_source_orderly_shutdown(uint64_t end)
{
    return platform_ready ? platform_ops.orderly_shutdown(platform_ops.ctx, end) : -ENOTSUP;
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
void power_source_snapshot(struct power_source_snapshot *out)
{
    if (!out) return;
    uint32_t f = irq_save(); *out = source; irq_restore(f);
}
int power_source_soft_off(void)
{
    uint32_t f = irq_save();
    if (!source.active || !provider_ops.soft_off) { irq_restore(f); return -ENOTSUP; }
    if (off_running) { irq_restore(f); return -EBUSY; }
    off_running = true;
    struct power_source_ops ops = provider_ops;
    irq_restore(f);
    int e = ops.soft_off(ops.ctx);
    f = irq_save(); off_running = false; irq_restore(f);
    return e ? e : -EIO; /* returning cannot prove physical power removal */
}
