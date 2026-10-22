/* IBM console amplifier, ACPI EC transport. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_THINKPAD_EC_H
#define CIUKI_THINKPAD_EC_H
#include <ciuki/abi.h>
#include <stdbool.h>
#include <stddef.h>
struct thinkpad_ec_io {
    void *ctx;
    uint8_t (*read)(void *, uint16_t);
    void (*write)(void *, uint16_t, uint8_t);
    uint64_t (*now)(void *);
    void (*pause)(void *);
};
struct thinkpad_ec {
    struct thinkpad_ec_io io;
    /* active survives the first seven failures; available gates delegation. */
    bool active, available, pending_read, timeout_logged;
    uint8_t status, failures;
    uint64_t next_poll, next_timeout_log;
    /* Volume poll interval (ms): THINKPAD_EC_DEFAULT_POLL_MS by default
     * (f2-96), THINKPAD_EC_POLL_MS with ecpoll=1, 0 (no periodic EC
     * traffic) with ecpoll=0 and until thinkpad_ec_start. */
    uint32_t poll_ms;
    /* f1-100: earliest tick of the next user-requested mirror refresh. */
    uint64_t next_refresh;
    /* f1-100 traffic accounting: port accesses and transactions (RD_EC/WR_EC
     * commands) per record window (exchanged by thinkpad_ec_window) and since
     * boot; the largest single operation; operations cut by the bound. */
    uint32_t window_io, window_transactions, window_max_io, window_refused, window_requests;
    uint32_t total_io, total_transactions;
};
/* f1-100: on the T23 every EC transaction raises the EC event line (GPI12,
 * routed to SMI# while SCI_EN is clear) and costs about two 2.1 ms SMIs.
 * f2-96: without a poll the mixer no longer followed the volume keys, so the
 * default is one RD_EC every 2 s (one transaction, about 4 ms of SMI per
 * 2 s). BOOT.CFG ecpoll=0 turns the poll off, ecpoll=1 selects the 1 Hz
 * diagnosis rate; ecpoll=0 wins over ecpoll=1. Exact tokens over the
 * bounded boot options. A failure retries at the poll cadence (1 s when
 * the poll is off and an explicit action asks). */
#define THINKPAD_EC_DEFAULT_POLL_MS 2000u /* default cadence (f2-96) */
#define THINKPAD_EC_POLL_MS 1000u        /* ecpoll=1 cadence; retry without a poll */
#define THINKPAD_EC_REFRESH_MS 1000u     /* minimum spacing of requested refreshes */
#define THINKPAD_EC_OP_TRANSACTIONS 4u   /* probe with a write: read, stable read, write, proof */
/* Returns the poll interval (0 off); *source names the deciding token
 * ("default", "ecpoll=0" or "ecpoll=1"). */
uint32_t thinkpad_ec_poll_option(const char *options, size_t size, const char **source);
/* Native poll state for records: interval in ms, 0 when off, -1 without EC. */
int thinkpad_ec_poll_state(void);
/* Firmware strings are copied/sanitized for records; eligibility is decided
 * against the complete original strings, before truncation. */
struct thinkpad_ec_smbios {
    const char *verdict;
    uint32_t entry_phys, table_phys;
    uint16_t table_bytes;
    uint8_t major, minor;
    bool valid, has_system, has_bios, eligible, hint, qemu;
    char vendor[48], product[48], version[48];
    char bios_vendor[48], bios_version[48];
};
bool thinkpad_ec_smbios_decode(const uint8_t *table, size_t length, struct thinkpad_ec_smbios *);
void thinkpad_ec_smbios_log(const struct thinkpad_ec_smbios *);
bool thinkpad_ec_gate(const struct thinkpad_ec_smbios *, uint32_t boot_flags);
bool thinkpad_ec_identity(const uint8_t *table, size_t length);
bool thinkpad_ec_entry(const uint8_t *entry, size_t length, uint32_t *address, uint16_t *bytes);
int thinkpad_ec_probe(struct thinkpad_ec *, const struct thinkpad_ec_io *, bool identified, struct ciuki_mixer_master *);
int thinkpad_ec_volume_set(struct thinkpad_ec *, struct ciuki_mixer_master *);
/* 0 unchanged/not due, 1 changed/recovered; failures retry before disabling. */
int thinkpad_ec_volume_poll(struct thinkpad_ec *, struct ciuki_mixer_master *);
/* f1-100: one mirror read for an explicit user action, independent of the
 * poll, at most once per THINKPAD_EC_REFRESH_MS; same results as the poll. */
int thinkpad_ec_volume_refresh(struct thinkpad_ec *, struct ciuki_mixer_master *);
/* f1-100: the window record "[ec] thinkpad io=... transactions=..." and a
 * counter reset; false (no text) for a window without EC traffic. */
bool thinkpad_ec_window(struct thinkpad_ec *, char *out, size_t size);
void thinkpad_ec_init(void);
void thinkpad_ec_start(void);
int thinkpad_ec_stop(void); /* bounded quirk stop; the generic EC stays available for _PTS */
int thinkpad_ec_master_set(struct ciuki_mixer_master *);
int thinkpad_ec_master_poll(struct ciuki_mixer_master *);
int thinkpad_ec_master_refresh(struct ciuki_mixer_master *);
/* Task context: ask the EC worker for one refresh (rate bounded); no-op
 * without a qualified EC. */
void thinkpad_ec_refresh_request(void);
/* cpustat window (task context): prints the traffic record, if any. */
void thinkpad_ec_window_emit(void);
#endif
