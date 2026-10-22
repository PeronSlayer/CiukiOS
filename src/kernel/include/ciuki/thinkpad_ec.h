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
    /* f1-94: volume poll interval (ms); 0 means no periodic EC traffic. */
    uint32_t poll_ms;
};
/* f1-94 amendment 1: 1 Hz by default (was 4 Hz); BOOT.CFG ecpoll=0 turns
 * the poll off. Exact token over the bounded boot options. */
#define THINKPAD_EC_POLL_DEFAULT_MS 1000u
uint32_t thinkpad_ec_poll_option(const char *options, size_t size);
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
void thinkpad_ec_init(void);
void thinkpad_ec_start(void);
int thinkpad_ec_master_set(struct ciuki_mixer_master *);
int thinkpad_ec_master_poll(struct ciuki_mixer_master *);
#endif
