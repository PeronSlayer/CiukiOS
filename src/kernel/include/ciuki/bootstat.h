/* G14 boot-status sector CBS1: the Windows 98 Wnbbotng.sts signature and the
 * Automatic Skip Driver agent's memory, kept in partition sector 9 of the
 * system volume (docs/design/startup-recovery.md section 2.1). The loader
 * reads it and changes only `state`; the kernel owns every other byte.
 * Pure helpers here are shared by the kernel and the host tests.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_BOOTSTAT_H
#define CIUKI_BOOTSTAT_H

#include <stdbool.h>
#include <stdint.h>
/* Freestanding kernel and hosted tests alike: no <string.h> here. */
#define BOOTSTAT_MEMCMP __builtin_memcmp
#define BOOTSTAT_MEMCPY __builtin_memcpy
#define BOOTSTAT_MEMSET __builtin_memset

#define BOOTSTAT_SECTOR        9u      /* partition-relative; black box is 8 and 32-159 */
#define BOOTSTAT_VERSION       1u
#define BOOTSTAT_CLASSES       8u
#define BOOTSTAT_SKIP_AFTER    2u      /* "After two failed attempts to load, ASD disables" */

/* Driver classes: bit numbers of disabled/skipped/active and of boot flag
 * bits 24-31 (CBI_F_SKIP_SHIFT). */
enum bootstat_class {
    BOOTSTAT_DISPLAY = 0,   /* accelerated display driver probe */
    BOOTSTAT_SOUND = 1,
    BOOTSTAT_NETWORK = 2,
    BOOTSTAT_USB = 3,
    BOOTSTAT_PORTS = 4,     /* serial, parallel, floppy, game port */
    BOOTSTAT_POWER = 5,     /* APM, ACPI power owner, platform power driver */
    BOOTSTAT_STARTUP = 6,   /* startup programs (desktop) */
};
#define BOOTSTAT_CLASS_MASK    0x7Fu

/* request bits */
#define BOOTSTAT_REQ_SCANDISK  1u
#define BOOTSTAT_REQ_THOROUGH  2u
#define BOOTSTAT_REQ_MENU      4u
#define BOOTSTAT_REQ_MASK      7u

/* Byte offsets of the 512-byte record. */
#define BOOTSTAT_OFF_STATE     4u
#define BOOTSTAT_OFF_VERSION   5u
#define BOOTSTAT_OFF_DISABLED  6u
#define BOOTSTAT_OFF_SKIPPED   7u
#define BOOTSTAT_OFF_ACTIVE    8u
#define BOOTSTAT_OFF_REQUEST   9u
#define BOOTSTAT_OFF_FAILURES  10u
#define BOOTSTAT_OFF_BLAMED    11u
#define BOOTSTAT_OFF_COUNTS    12u
#define BOOTSTAT_OFF_BOOTS     20u
#define BOOTSTAT_OFF_COMPLETED 24u
#define BOOTSTAT_OFF_SKIPPED_AT 28u
#define BOOTSTAT_OFF_CRC       508u

struct bootstat_record {
    uint8_t state, version, disabled, skipped, active, request, failures, blamed;
    uint8_t fail_count[BOOTSTAT_CLASSES];
    uint32_t boots, completed;
    uint32_t skipped_at[BOOTSTAT_CLASSES];
};

static inline const char *bootstat_class_name(unsigned c)
{
    static const char *const names[BOOTSTAT_CLASSES] = {
        "display", "sound", "network", "usb", "ports", "power", "startup", "reserved" };
    return c < BOOTSTAT_CLASSES ? names[c] : "unknown";
}

static inline uint32_t bootstat_crc_update(uint32_t x, const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        x ^= b[i];
        for (unsigned j = 0; j < 8; j++) x = (x >> 1) ^ (0xedb88320u & -(x & 1));
    }
    return x;
}
/* zlib CRC-32 of bytes 0-3 and 5-507: byte 4 (state) is the loader's. */
static inline uint32_t bootstat_crc(const uint8_t b[512])
{
    uint32_t x = bootstat_crc_update(~0u, b, BOOTSTAT_OFF_STATE);
    x = bootstat_crc_update(x, b + BOOTSTAT_OFF_STATE + 1, BOOTSTAT_OFF_CRC - BOOTSTAT_OFF_STATE - 1);
    return ~x;
}
static inline uint32_t bootstat_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void bootstat_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline bool bootstat_blank(const uint8_t b[512])
{
    for (unsigned i = 0; i < 512; i++) if (b[i]) return false;
    return true;
}
/* Valid: magic, version, CRC, a 0/1 state, no reserved class bits and a
 * zero tail. Anything else is foreign data the kernel never overwrites. */
static inline bool bootstat_decode(const uint8_t b[512], struct bootstat_record *r)
{
    if (BOOTSTAT_MEMCMP(b, "CBS1", 4) || b[BOOTSTAT_OFF_VERSION] != BOOTSTAT_VERSION || b[BOOTSTAT_OFF_STATE] > 1 ||
        bootstat_rd32(b + BOOTSTAT_OFF_CRC) != bootstat_crc(b) ||
        ((b[BOOTSTAT_OFF_DISABLED] | b[BOOTSTAT_OFF_SKIPPED] | b[BOOTSTAT_OFF_ACTIVE] | b[BOOTSTAT_OFF_BLAMED]) & ~BOOTSTAT_CLASS_MASK) ||
        (b[BOOTSTAT_OFF_REQUEST] & ~BOOTSTAT_REQ_MASK))
        return false;
    for (unsigned i = BOOTSTAT_OFF_SKIPPED_AT + 4 * BOOTSTAT_CLASSES; i < BOOTSTAT_OFF_CRC; i++) if (b[i]) return false;
    r->state = b[BOOTSTAT_OFF_STATE]; r->version = b[BOOTSTAT_OFF_VERSION];
    r->disabled = b[BOOTSTAT_OFF_DISABLED]; r->skipped = b[BOOTSTAT_OFF_SKIPPED];
    r->active = b[BOOTSTAT_OFF_ACTIVE]; r->request = b[BOOTSTAT_OFF_REQUEST];
    r->failures = b[BOOTSTAT_OFF_FAILURES]; r->blamed = b[BOOTSTAT_OFF_BLAMED];
    BOOTSTAT_MEMCPY(r->fail_count, b + BOOTSTAT_OFF_COUNTS, BOOTSTAT_CLASSES);
    r->boots = bootstat_rd32(b + BOOTSTAT_OFF_BOOTS);
    r->completed = bootstat_rd32(b + BOOTSTAT_OFF_COMPLETED);
    for (unsigned i = 0; i < BOOTSTAT_CLASSES; i++) r->skipped_at[i] = bootstat_rd32(b + BOOTSTAT_OFF_SKIPPED_AT + 4 * i);
    return true;
}
static inline void bootstat_encode(uint8_t b[512], const struct bootstat_record *r)
{
    BOOTSTAT_MEMSET(b, 0, 512);
    BOOTSTAT_MEMCPY(b, "CBS1", 4);
    b[BOOTSTAT_OFF_STATE] = r->state ? 1 : 0; b[BOOTSTAT_OFF_VERSION] = BOOTSTAT_VERSION;
    b[BOOTSTAT_OFF_DISABLED] = r->disabled & BOOTSTAT_CLASS_MASK;
    b[BOOTSTAT_OFF_SKIPPED] = r->skipped & BOOTSTAT_CLASS_MASK;
    b[BOOTSTAT_OFF_ACTIVE] = r->active & BOOTSTAT_CLASS_MASK;
    b[BOOTSTAT_OFF_REQUEST] = r->request & BOOTSTAT_REQ_MASK;
    b[BOOTSTAT_OFF_FAILURES] = r->failures; b[BOOTSTAT_OFF_BLAMED] = r->blamed & BOOTSTAT_CLASS_MASK;
    BOOTSTAT_MEMCPY(b + BOOTSTAT_OFF_COUNTS, r->fail_count, BOOTSTAT_CLASSES);
    bootstat_wr32(b + BOOTSTAT_OFF_BOOTS, r->boots);
    bootstat_wr32(b + BOOTSTAT_OFF_COMPLETED, r->completed);
    for (unsigned i = 0; i < BOOTSTAT_CLASSES; i++) bootstat_wr32(b + BOOTSTAT_OFF_SKIPPED_AT + 4 * i, r->skipped_at[i]);
    bootstat_wr32(b + BOOTSTAT_OFF_CRC, bootstat_crc(b));
}

/* The Automatic Skip Driver step at the start of a boot that follows a
 * failed one: blame the classes that were active, skip a class at its
 * second failure. Returns the classes newly skipped by this call. */
static inline uint8_t bootstat_blame(struct bootstat_record *r, bool previous_failed, uint32_t boot)
{
    uint8_t newly = 0;
    if (!previous_failed) { r->failures = 0; r->blamed = 0; r->active = 0; return 0; }
    if (r->failures < 255) r->failures++;
    r->blamed = r->active & BOOTSTAT_CLASS_MASK;
    for (unsigned c = 0; c < BOOTSTAT_CLASSES; c++) {
        if (!(r->blamed & (1u << c))) continue;
        if (r->fail_count[c] < 255) r->fail_count[c]++;
        if (r->fail_count[c] >= BOOTSTAT_SKIP_AFTER && !(r->skipped & (1u << c))) {
            r->skipped |= (uint8_t)(1u << c); r->skipped_at[c] = boot; newly |= (uint8_t)(1u << c);
        }
    }
    r->active = 0;
    return newly;
}
/* A completed start: clean state, and the classes that finished this boot
 * (ran) forget their failure counts. */
static inline void bootstat_complete(struct bootstat_record *r, uint8_t ran)
{
    r->state = 0; r->active = 0; r->failures = 0;
    if (r->completed != UINT32_MAX) r->completed++;
    for (unsigned c = 0; c < BOOTSTAT_CLASSES; c++) if (ran & (1u << c)) r->fail_count[c] = 0;
}

/* Kernel module (core/bootstat.c). Every call is a no-op until bound. */
struct blkdev;
struct ciuki_recovery_status;
int bootstat_bind(struct blkdev *, uint64_t start, uint64_t count);
void bootstat_reset(void);
bool bootstat_bound(void);
bool bootstat_skip(enum bootstat_class);      /* this start's flag bits 24-31 */
void bootstat_begin(enum bootstat_class);     /* writes the record */
void bootstat_end(enum bootstat_class);       /* memory only */
void bootstat_presented(void);                /* first full desktop frame */
int bootstat_service(void);                   /* storage writer: persist completion */
void bootstat_status(struct ciuki_recovery_status *);
int bootstat_configure(uint8_t disabled, uint8_t request);
int bootstat_enable(uint8_t classes);
int bootstat_schedule_scan(bool thorough);
#endif
