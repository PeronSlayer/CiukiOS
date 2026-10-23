/* Kernel-only power providers. No user ABI or SDK records.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_POWER_SOURCE_H
#define CIUKI_POWER_SOURCE_H
#include <stdint.h>
#include <stdbool.h>
#define POWER_SOURCE_UNKNOWN UINT32_MAX

enum power_source_kind { POWER_SOURCE_NONE, POWER_SOURCE_ACPI, POWER_SOURCE_APM };
struct power_source_status {
    uint32_t ac;                 /* 0 offline, 1 online, 2 backup, or UNKNOWN */
    uint32_t percent;            /* 0..100 or UNKNOWN */
    uint32_t seconds;            /* remaining life or UNKNOWN */
    uint32_t battery_flags;      /* provider-specific validated flags or UNKNOWN */
};
struct power_source_snapshot {
    enum power_source_kind provider;
    bool active;
    struct power_source_status status;
    uint32_t events;
    uint16_t last_event;
    uint16_t event_info;
    int error;
};
struct power_source_ops {
    void *ctx;
    /* Called in preemptible task context. Marshal onto the provider worker;
     * must complete orderly shutdown before requesting firmware Off. */
    int (*soft_off)(void *);
};
/* Installed once by the global device/time owner before either backend
 * connects. quiesce_dma and orderly_shutdown are nonblocking steps with
 * absolute PIT-millisecond deadlines: EAGAIN means pending. Expiry retains
 * uncertain DMA leases; no callback may retire another caller's live I/O.
 * resume must restore time/devices before workloads can run again.
 * orderly_shutdown includes application closure, storage_sync and DMA stop.
 * Never install a success-returning no-op for a missing device owner. */
struct power_source_platform {
    void *ctx;
    int (*quiesce_dma)(void *, uint64_t deadline_ms);
    int (*resume)(void *, uint16_t event, uint16_t info);
    int (*orderly_shutdown)(void *, uint64_t deadline_ms);
};
int power_source_set_platform(const struct power_source_platform *);
bool power_source_platform_ready(void);
int power_source_quiesce_dma(uint64_t deadline_ms);
int power_source_resume(uint16_t event, uint16_t info);
int power_source_orderly_shutdown(uint64_t deadline_ms);
/* Reservation excludes both active and failed/uncertain providers. */
int power_source_reserve(enum power_source_kind, const struct power_source_ops *);
int power_source_activate(enum power_source_kind);
void power_source_release(enum power_source_kind, bool uncertain, int error);
void power_source_publish(enum power_source_kind, const struct power_source_status *);
void power_source_event(enum power_source_kind, uint16_t event, uint16_t info);
void power_source_snapshot(struct power_source_snapshot *); /* cache only */
int power_source_soft_off(void);
#endif
