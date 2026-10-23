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

enum apm_state { APM_UNPROBED, APM_ABSENT, APM_EXCLUDED, APM_DISCOVERED,
                 APM_CONNECTED, APM_FAILED };
enum apm_stage { APM_FLUSH, APM_STOP_DMA, APM_ACK };
struct apm_status {
    uint32_t ac; /* 0 off, 1 on, 2 backup, APM_UNKNOWN */
    uint32_t percent;
    uint32_t seconds;
    uint32_t battery_flags; /* raw validated APM flags, or APM_UNKNOWN */
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
};
struct apm_backend {
    struct apm_ops ops;
    enum apm_state state;
    enum apm_stage stage;
    struct apm_status status;
    uint16_t version;
    uint16_t flags;
    uint16_t last_event;
    uint8_t bios_error;
    int transport_error;
    int error;
    int emergency_error;
    uint32_t events;
    uint32_t poll_overflows;
    uint64_t next_poll;
    uint64_t emergency_start;
    uint64_t off_deadline;
    bool emergency;
    bool reserved;
    bool connected;
    bool off_started;
};
void apm_backend_init(struct apm_backend *, const struct apm_ops *);
int apm_backend_probe(struct apm_backend *);
int apm_backend_connect(struct apm_backend *);
int apm_backend_service(struct apm_backend *);
int apm_backend_off(struct apm_backend *); /* requires global orderly-shutdown proof before Off */
void apm_status_decode(const struct biosvm_regs *, struct apm_status *);

/* Explicit boot startup; services must be installed before connection. */
void apm_start(void);
#endif
