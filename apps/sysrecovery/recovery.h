/* SPDX-License-Identifier: MIT
 * G14 user-space recovery tools shared by the desktop, the Terminal,
 * ScanDisk and System Configuration (docs/design/startup-recovery.md
 * sections 2.4-2.7): Registry Checker (regcheck.c), System File Checker
 * (sfc.c) and the ScanDisk report text (scanreport.c). Every path is
 * absolute on the system drive and prefixed with `root` ("" on CiukiOS, a
 * fixture directory on the host). Functions return 0 or a negative errno. */
#ifndef CIUKI_SYSRECOVERY_H
#define CIUKI_SYSRECOVERY_H
#include <errno.h>
#include <stdio.h>
#include <ciuki/abi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if defined(__CIUKIOS__) && !defined(__linux__)
#include <ciuki/recovery.h>
#endif

/* ---- sec-sysvc: SYSTEM services (K-Q4) ------------------------------------
 * C:\SYSTEM\LOGS and BOOT.CFG are SYSTEM-only on a managed system volume. An
 * administrator appends the maintenance reports (SCANDISK.LOG, SFC.LOG)
 * through the SYSTEM log service and saves BOOT.CFG through the SYSTEM
 * boot-configuration service (ciuki/recovery.h). Host fixtures (root not ""),
 * an older kernel (ENOSYS) and Setup media without a managed system volume
 * (ENODEV) use the files directly. */
#define SR_LOG_LIMIT (256u * 1024u)   /* a report log starts over past this */
static inline int sr_log_append(const char *root, const char *name, const char *text, size_t n)
{
#if defined(__CIUKIOS__) && !defined(__linux__)
    if (!root || !*root) {
        int e = 0;
        for (size_t at = 0; !e && at < n;) {
            size_t part = n - at > CIUKI_SYSTEM_LOG_DATA_MAX ? CIUKI_SYSTEM_LOG_DATA_MAX : n - at;
            e = ciuki_system_log_write(CIUKI_SYSTEM_LOG_MACHINE, name, 0, text + at, part, SR_LOG_LIMIT) ? -errno : 0;
            at += part;
        }
        if (e != -ENOSYS && e != -ENODEV) return e;
    }
#endif
    char path[512];
    int k = snprintf(path, sizeof(path), "%s/System/LOGS/%s", root ? root : "", name);
    if (k < 0 || (size_t)k >= sizeof(path)) return -ENAMETOOLONG;
    FILE *f = fopen(path, "a");
    if (!f) return -errno;
    int e = n && fwrite(text, 1, n, f) != n ? -EIO : 0;
    if (!e && ferror(f)) e = -EIO;
    if (fclose(f) && !e) e = -errno;
    return e;
}
/* 0: saved by the service; 1: write the file yourself (host, older kernel). */
static inline int sr_boot_config(const char *root, const char *text, size_t n, int *error)
{
    *error = 0;
#if defined(__CIUKIOS__) && !defined(__linux__)
    if (!root || !*root) {
        if (!ciuki_boot_config_set(text, n)) return 0;
        if (errno != ENOSYS && errno != ENODEV) { *error = -errno; return 0; }
    }
#else
    (void)root; (void)text; (void)n;
#endif
    return 1;
}
/* ---- end sec-sysvc ---- */

/* ---- Registry Checker (Windows 98 ScanReg/ScanRegW) -------------------- */
/* sec-sysvc2: a backup holds the machine registry the SYSTEM registry broker
 * owns since sec-k06 (its native System and Programs roots and the Win32
 * machine hive slots, RC_MACHINE), BOOT.CFG, HWPROF.CFG and the .cfg files
 * of /system/settings. The backups are SYSTEM-only (the policy seals
 * RC_BACKUP_DIR). With root "" on CiukiOS each call below is a request to
 * the broker (regcheck.c): administrators only (EPERM otherwise), except
 * rc_daily (a SYSTEM job for every session) and rc_status. The broker and the
 * host fixtures run the same code locally. */
#define RC_BACKUP_DIR "/System/SysBckup"
#define RC_MACHINE "/System/Security/Registry/Machine"
#define RC_MAX_BACKUPS 5            /* Scanreg.ini MaxBackupCopies default */
#define RC_MAX_FILES 32
#define RC_MAX_BYTES (4u * 1024u * 1024u)
#define RC_COMMENT 40
#define RC_ROOTS 2                  /* the broker's native machine roots: System, Programs */
#define RC_SLOTS 2                  /* SYSTEM.CRG.0/.1, the Win32 machine hive */
#define RC_HIVE (1u << RC_ROOTS)    /* that hive in a restored mask (rc_root_name(RC_ROOTS)) */
#define RC_WIRE 80                  /* one rc_backup on the broker's wire */
struct rc_backup {
    char name[16];                  /* RB000.CRB .. RB004.CRB */
    int64_t created;                /* seconds since 1970, UTC */
    uint32_t files, bytes;
    bool started;                   /* made after a start that completed */
    bool valid;                     /* header, entries and checksums verified */
    char comment[RC_COMMENT];
};
/* Backups present, newest first. Returns the count (<= max) or -errno. */
int rc_list(const char *root, struct rc_backup *out, unsigned max);
/* A new backup; the oldest beyond RC_MAX_BACKUPS is deleted first. On
 * CiukiOS the broker takes the time and the started flag itself. */
int rc_backup(const char *root, const char *comment, int64_t now, bool started, struct rc_backup *made);
/* Once a day (local calendar day of `now`, utc_offset_minutes east).
 * *made: 1 when a backup was written. On CiukiOS: the broker's clock. */
int rc_daily(const char *root, int64_t now, int utc_offset_minutes, int *made);
/* Registry roots that do not parse (bit i: rc_root_name(i)); a cut write is
 * first finished by libreg's recovery. */
int rc_scan(const char *root, unsigned *damaged);
const char *rc_root_name(unsigned i);
/* Restore every file of a backup (ScanReg /Restore); the registry roots use
 * libreg's journal protocol with a generation above the current one. On
 * CiukiOS the broker records the request and restores at the next start,
 * before any program uses the registry. */
int rc_restore(const char *root, const char *name);
/* Each damaged root from the newest backup that holds a valid copy.
 * *restored: roots restored; from: that backup's name. */
int rc_autorestore(const char *root, unsigned *restored, char from[16]);
/* The same at the broker's start, before its hive is loaded: also the Win32
 * machine hive when no slot is valid (RC_HIVE in *restored). */
int rc_autorestore_start(const char *root, unsigned *restored, char from[16]);
/* What Registry Checker did at this start (the broker's boot work) and now. */
struct rc_status {
    unsigned damaged;               /* roots that do not parse now */
    unsigned restored;              /* roots restored from a backup at this start */
    char from[16];                  /* ... from this backup */
    char applied[16];               /* a requested restore done at this start */
    char pending[16];               /* a restore requested for the next start */
};
int rc_status(const char *root, struct rc_status *);
/* The broker's own steps (local only): record a restore for the next start,
 * read the request (1: name set, 0: none, -errno), and carry it out (applied:
 * its name; a damaged or missing backup drops the request). */
int rc_stage_restore(const char *root, const char *name);
int rc_pending(const char *root, char name[16]);
int rc_apply_pending(const char *root, char applied[16]);
void rc_wire_put(const struct rc_backup *, uint8_t out[RC_WIRE]);
void rc_wire_get(struct rc_backup *, const uint8_t in[RC_WIRE]);

/* ---- System File Checker (Sfc.exe, Default.sfc) ------------------------- */
#define SFC_LIST "/System/SFC.LST"
#define SFC_OVERRIDES "/System/SFC.DAT"
#define SFC_HELPDESK "/System/Helpdesk/SFC"
#define SFC_LOG "/System/LOGS/SFC.LOG"
enum sfc_state { SFC_OK, SFC_CHANGED, SFC_DELETED };
struct sfc_problem {
    char path[160];
    enum sfc_state state;
    uint64_t expected_size, size;
    char expected[65];              /* SHA-256 hex of the baseline */
};
struct sfc_result { unsigned checked, changed, deleted, count; struct sfc_problem *problems; };
/* Scan every baseline entry; problems are allocated (sfc_free). */
int sfc_scan(const char *root, struct sfc_result *out);
void sfc_free(struct sfc_result *);
/* "Update verification information": the current file becomes the baseline. */
int sfc_update(const char *root, const char *path);
/* "Restore file" from source_root + path, after checking it is the original
 * (EILSEQ otherwise); the current file is first copied to SFC_HELPDESK. */
int sfc_restore(const char *root, const char *path, const char *source_root);
/* Append one line per problem (or "no changes") to SFC_LOG. */
int sfc_log(const char *root, const struct sfc_result *, int64_t now);

/* ---- BOOT.CFG Advanced options (bootcfg.c) -------------------------------- */
#define BOOTCFG_MAX 127u            /* the loader's limit */
struct bootcfg_options { int menu, autoscan, serial; };   /* autoscan 0, 1 (message) or 2 */
void bootcfg_read(const char *text, size_t n, struct bootcfg_options *);
/* Replace the options' tokens, keep the rest; -EFBIG beyond 127 bytes. */
int bootcfg_apply(const char *text, size_t n, const struct bootcfg_options *, char out[BOOTCFG_MAX + 1], size_t *length);
int bootcfg_update(const char *root, const struct bootcfg_options *);

/* ---- ScanDisk report (Windows 98 "ScanDisk Results") --------------------- */
/* The summary paragraph and the statistics, as text with '\n'. */
size_t scandisk_text(const struct ciuki_scandisk *r, char *out, size_t cap);
/* Append the report to /System/LOGS/SCANDISK.LOG (SCANDISK.LOG of Win98). */
int scandisk_log(const char *root, const struct ciuki_scandisk *r, int64_t now);
#endif
