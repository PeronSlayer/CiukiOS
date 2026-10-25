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
#define POWER_SOURCE_BATTERIES 4u
/* fx-18: one APM 1.2 battery unit (Get Power Status BX=80nnh). */
struct power_source_battery {
    uint32_t present;            /* 0 absent (CH bit 4), 1 present, or UNKNOWN */
    uint32_t percent;
    uint32_t seconds;
    uint32_t flags;              /* validated APM battery flag or UNKNOWN */
};
struct power_source_status {
    uint32_t ac;                 /* 0 offline, 1 online, 2 backup, or UNKNOWN */
    uint32_t percent;            /* 0..100 or UNKNOWN */
    uint32_t seconds;            /* remaining life or UNKNOWN */
    uint32_t battery_flags;      /* provider-specific validated flags or UNKNOWN */
    /* fx-18: battery units with per-unit facts (0: aggregate only). */
    uint32_t units;
    struct power_source_battery battery[POWER_SOURCE_BATTERIES];
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
    /* Optional: the forced Off after the emergency fence
     * (power_source_mark_emergency_stopped), with the forced authorization
     * carried explicitly (APM: no orderly proof is consulted). NULL: a
     * forced Off is refused (ENOTSUP) and the user uses the power button. */
    int (*soft_off_forced)(void *);
    /* Optional and cache-only: false while firmware Off is unqualified
     * (ACPI without an evaluated \_S5). NULL: an active provider may try. */
    bool (*can_off)(void *);
    /* G11, optional: the provider reports and can enter a sleep state (APM
     * 1.2 Get Capabilities, ACPI \_S1/\_S3 with a wake path). NULL: no. */
    bool (*can_standby)(void *);
    int (*standby)(void *);
    int (*cancel_standby)(void *);
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
/* core/power_platform.c: the owner of these services on pre-ACPI machines. */
int power_platform_start(void);
bool power_source_platform_ready(void);
int power_source_quiesce_dma(uint64_t deadline_ms);
int power_source_resume(uint16_t event, uint16_t info);
/* The one orderly-shutdown path is call 73 (core/power.c): the desktop has
 * closed applications, then storage_sync and native device quiescence
 * succeeded. It latches this; the latch is never cleared because the
 * session cannot resume afterwards. */
void power_source_mark_stopped(void);
/* The forced power off's proof: devices stopped by the bounded fence,
 * storage possibly dirty (T23 c42f556e, review 2 finding 5). */
void power_source_mark_emergency_stopped(void);
bool power_source_emergency_stopped(void);
int power_source_soft_off_forced(void);   /* EBUSY without the emergency latch, ENOTSUP without a forced entry */
/* 0 once stopped, EBUSY before. Performs no shutdown work itself. */
int power_source_orderly_shutdown(uint64_t deadline_ms);
/* Reservation excludes both active and failed/uncertain providers.
 * fx-18: an ACPI reservation while APM is active and healthy asks APM to
 * yield and returns -EAGAIN; ACPI retries (bounded) until APM released. */
int power_source_reserve(enum power_source_kind, const struct power_source_ops *);
bool power_source_handoff_requested(enum power_source_kind);
void power_source_handoff_cancel(void);
/* fx-18: the ACPI inventory's ownership decision, reported once
 * (CIUKI_POWER_FIRMWARE/OWNED/QUARANTINED). APM is admitted without ACPI
 * tables, or once ACPI settled with clean firmware ownership. */
void power_source_acpi_settled(uint32_t ownership);
bool power_source_acpi_decided(void);
bool power_source_apm_admitted(bool acpi_tables);
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
/* G11 (docs/design/power-policy.md): event counters of the active provider
 * for call 73 EVENTS (sleep requests, battery low, critical, resumes, status
 * changes), monotonic for the boot; cache only. */
struct power_source_counters {
    uint32_t sleep_requests, battery_low, critical, resumes, status_changes;
};
void power_source_counters(struct power_source_counters *);
/* The first unmet standby qualification (CIUKI_POWER_STANDBY_*): an active
 * provider, its sleep capability, then a resume path for every native DMA
 * device (power_devices_resume_ready). Cache only. */
uint32_t power_source_standby_reason(void);
/* ENOTSUP unless the reason is AVAILABLE; then the provider's standby. */
int power_source_standby(void);
int power_source_cancel_standby(void);
/* G11: Win32 SYSTEM_POWER_STATUS of the call-73 record (GetSystemPowerStatus,
 * docs/design/power-policy.md 24): ac 0/1/255 (APM backup power is 0),
 * battery_flag 1/2/4/8/128/255, percent 0..100 or 255, seconds of life on
 * battery or UINT32_MAX (full life is always unknown). Cache only. */
struct power_source_win32 { uint32_t ac, battery_flag, percent, seconds; };
void power_source_win32_status(struct power_source_win32 *);
/* core/power_quiesce.c: true only when every native driver holding DMA or
 * interrupt state can suspend and restore it. */
bool power_devices_resume_ready(void);
#endif
