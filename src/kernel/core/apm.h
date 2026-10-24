/* Kernel-private APM 1.2 protocol. No user or DOS firmware-call ABI.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_APM_INTERNAL_H
#define CIUKI_APM_INTERNAL_H
#include <ciuki/biosvm.h>

#define APM_VERSION 0x0102u
#define APM_STATUS_MS 100u
#define APM_POWER_MS 2000u
#define APM_POLL_MS 1000u
#define APM_FLUSH_MS 2000u
#define APM_QUIESCE_MS 2500u
#define APM_UNKNOWN UINT32_MAX
/* fx-18: Get Power Status every 10 s (and at once after power events);
 * at most four battery units (Get Power Status BX=80nnh, APM 1.2 4.6.11);
 * one cost record per 60 s. */
#define APM_STATUS_REFRESH_MS 10000u
#define APM_UNITS 4u
#define APM_WINDOW_MS 60000u

enum apm_state { APM_UNPROBED, APM_ABSENT, APM_EXCLUDED, APM_DISCOVERED,
                 APM_CONNECTED, APM_FAILED };
enum apm_stage { APM_FLUSH, APM_STOP_DMA, APM_ACK };
/* fx-18: Battery Low (0005h) flushes only; Critical Suspend (0008h) also
 * stops DMA and acknowledges with Set Power State Suspend. */
enum apm_emergency { APM_EMERGENCY_CRITICAL, APM_EMERGENCY_LOW };
struct apm_unit {
    uint32_t present; /* 0 absent (CH bit 4), 1 present, APM_UNKNOWN */
    uint32_t percent;
    uint32_t seconds;
    uint32_t flags;
};
struct apm_status {
    uint32_t ac; /* 0 off, 1 on, 2 backup, APM_UNKNOWN */
    uint32_t percent;
    uint32_t seconds;
    uint32_t battery_flags; /* raw validated APM flags, or APM_UNKNOWN */
    uint32_t units;         /* battery units with BX=80nnh facts */
    struct apm_unit unit[APM_UNITS];
};
/* fx-18 cost record (f1-100): firmware calls in one window, wall time per
 * call including any SMI it raised; skipped = EC hold busy (EAGAIN). */
struct apm_cost {
    uint32_t calls, event_calls, status_calls, events, skipped, max_us;
    uint64_t total_us;
};

/* Single worker owns all operations. Every call, including disconnect,
 * uses the contained real-mode BIOS VM. reserve admits the exclusive owner
 * and a deadline-aware storage/DMA quiesce + resume service. release(true)
 * retains uncertain resources. step is nonblocking (-EAGAIN = pending).
 * Callers service at least every 10 ms during emergencies. No UI BIOS calls.
 */
struct apm_ops {
    void *ctx;
    int (*real_call)(void *, struct biosvm_regs *, uint32_t);
    bool (*acpi_present)(void *);
    int (*reserve)(void *);
    void (*release)(void *, bool uncertain);
    uint64_t (*now_ms)(void *);
    int (*step)(void *, enum apm_stage, uint64_t deadline);
    int (*resume)(void *, uint16_t event, uint16_t info);
    void (*publish)(void *, const struct apm_status *);
    void (*event)(void *, uint16_t, uint16_t);
    int (*orderly_shutdown)(void *, uint64_t deadline);
    void (*emergency_record)(void *, int error);
    /* fx-18, optional: microsecond clock for the cost record, the battery
     * low record, and the ACPI handoff request (ACPI replaces APM). */
    uint64_t (*now_us)(void *);
    void (*low_battery)(void *, int error);
    bool (*handoff)(void *);
    bool (*sleep_ready)(void *);
};
struct apm_backend {
    struct apm_ops ops;
    enum apm_state state;
    enum apm_stage stage;
    struct apm_status status;
    uint16_t version;
    uint16_t flags;
    uint16_t capabilities, sleep_request;
    bool capabilities_valid, capabilities_due;
    uint64_t sleep_processing_at;
    uint16_t last_event;
    uint8_t bios_error;
    uint16_t last_function; /* fx-25: function of the last firmware call */
    int transport_error;
    int error;
    int emergency_error;
    uint32_t events;
    uint32_t poll_overflows;
    uint64_t next_poll;
    uint64_t emergency_start;
    uint64_t off_deadline;
    uint64_t next_status;
    struct apm_cost cost;
    uint8_t units;           /* probed battery units, 0..APM_UNITS */
    enum apm_emergency emergency_kind;
    uint8_t rejects_pending; /* standby/suspend requests still to answer */
    bool status_due, probe_units;
    bool emergency;
    bool reserved;
    bool connected;
    bool off_started;
};
void apm_backend_init(struct apm_backend *, const struct apm_ops *);
int apm_backend_probe(struct apm_backend *);
int apm_backend_connect(struct apm_backend *);
int apm_backend_service(struct apm_backend *);
bool apm_backend_can_standby(const struct apm_backend *);
int apm_backend_standby(struct apm_backend *);
int apm_backend_cancel_standby(struct apm_backend *);
int apm_backend_off(struct apm_backend *); /* requires global orderly-shutdown proof before Off */
void apm_status_decode(const struct biosvm_regs *, struct apm_status *);
/* fx-18: one battery unit (BX=80nnh); CH bit 4 = selected unit absent. */
void apm_unit_decode(const struct biosvm_regs *, struct apm_unit *);
/* Disconnect and release cleanly for an ACPI takeover (state EXCLUDED);
 * a failed disconnect keeps the reservation uncertain. */
int apm_backend_yield(struct apm_backend *);
/* Returns the cost window and starts a new one. */
void apm_backend_cost(struct apm_backend *, struct apm_cost *);

/* Explicit boot startup; services must be installed before connection. */
void apm_start(void);
#endif
