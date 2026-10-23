/* Kernel-only power providers: ACPI (f1-110) and APM 1.2 (f1-121) behind one
 * exclusive interface. Call-73 STATUS, the desktop tray, the Control Panel
 * Power page and soft-off read or use only this interface.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_POWER_SOURCE_H
#define CIUKI_POWER_SOURCE_H
#include <stdint.h>
#include <stdbool.h>
#define POWER_SOURCE_UNKNOWN UINT32_MAX
struct ciuki_power_status;

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
    /* Called in preemptible task context, only after the orderly-shutdown
     * latch (power_source_mark_stopped). Marshal onto the provider worker. */
    int (*soft_off)(void *);
    /* Optional and cache-only: false while firmware Off is unqualified
     * (ACPI without an evaluated \_S5). NULL: an active provider may try. */
    bool (*can_off)(void *);
};
/* Installed once by the global device/time owner before APM connects.
 * quiesce_dma is a nonblocking step with an absolute PIT-millisecond
 * deadline: EAGAIN means pending. Expiry retains uncertain DMA leases; no
 * callback may retire another caller's live I/O. resume must restore
 * time/devices before workloads can run again. Never install a
 * success-returning no-op for a missing device owner. */
struct power_source_platform {
    void *ctx;
    int (*quiesce_dma)(void *, uint64_t deadline_ms);
    int (*resume)(void *, uint16_t event, uint16_t info);
};
int power_source_set_platform(const struct power_source_platform *);
bool power_source_platform_ready(void);
int power_source_quiesce_dma(uint64_t deadline_ms);
int power_source_resume(uint16_t event, uint16_t info);
/* The one orderly-shutdown path is call 73 (core/power.c): the desktop has
 * closed applications, then storage_sync and native device quiescence
 * succeeded. It latches this; the latch is never cleared because the
 * session cannot resume afterwards. */
void power_source_mark_stopped(void);
/* 0 once stopped, EBUSY before. Performs no shutdown work itself. */
int power_source_orderly_shutdown(uint64_t deadline_ms);
/* Reservation excludes both active and failed/uncertain providers. */
int power_source_reserve(enum power_source_kind, const struct power_source_ops *);
int power_source_activate(enum power_source_kind);
void power_source_release(enum power_source_kind, bool uncertain, int error);
void power_source_publish(enum power_source_kind, const struct power_source_status *);
void power_source_event(enum power_source_kind, uint16_t event, uint16_t info);
/* ACPI's firmware description and readings, in any ownership state (also
 * firmware-retained or quarantined). Only POWER_SOURCE_ACPI is accepted:
 * APM facts come from power_source_publish while APM holds the reservation. */
void power_source_publish_record(enum power_source_kind, const struct ciuki_power_status *);
void power_source_snapshot(struct power_source_snapshot *); /* cache only */
/* Call-73 STATUS record of the reserved provider (ACPI record otherwise);
 * cache only, callable from any context, never calls firmware. */
void power_source_status(struct ciuki_power_status *);
bool power_source_can_off(void);
int power_source_soft_off(void); /* EBUSY before power_source_mark_stopped */
#endif
