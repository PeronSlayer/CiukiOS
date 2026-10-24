/* SPDX-License-Identifier: MIT
 * G14 user-space recovery tools shared by the desktop, the Terminal,
 * ScanDisk and System Configuration (docs/design/startup-recovery.md
 * sections 2.4-2.7): Registry Checker (regcheck.c), System File Checker
 * (sfc.c) and the ScanDisk report text (scanreport.c). Every path is
 * absolute on the system drive and prefixed with `root` ("" on CiukiOS, a
 * fixture directory on the host). Functions return 0 or a negative errno. */
#ifndef CIUKI_SYSRECOVERY_H
#define CIUKI_SYSRECOVERY_H
#include <ciuki/abi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Registry Checker (Windows 98 ScanReg/ScanRegW) -------------------- */
#define RC_BACKUP_DIR "/System/SysBckup"
#define RC_MAX_BACKUPS 5            /* Scanreg.ini MaxBackupCopies default */
#define RC_MAX_FILES 32
#define RC_MAX_BYTES (4u * 1024u * 1024u)
#define RC_COMMENT 40
#define RC_ROOTS 4                  /* System, Desktop, Programs, Users (Hardware is not a file) */
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
/* A new backup of the registry roots, BOOT.CFG and the .cfg files of /system/settings;
 * the oldest beyond RC_MAX_BACKUPS is deleted first. */
int rc_backup(const char *root, const char *comment, int64_t now, bool started, struct rc_backup *made);
/* Once a day (local calendar day of `now`, utc_offset_minutes east).
 * *made: 1 when a backup was written. */
int rc_daily(const char *root, int64_t now, int utc_offset_minutes, int *made);
/* Root files that do not parse (bit i: rc_root_name(i)); a journal is
 * first recovered by libreg itself. */
int rc_scan(const char *root, unsigned *damaged);
const char *rc_root_name(unsigned i);
/* Restore every file of a backup (ScanReg /Restore). The registry roots use
 * libreg's journal protocol with a generation above the current one. */
int rc_restore(const char *root, const char *name);
/* Registry Checker at start: each damaged root from the newest backup that
 * holds a valid copy. *restored: roots restored; from: that backup's name. */
int rc_autorestore(const char *root, unsigned *restored, char from[16]);

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
