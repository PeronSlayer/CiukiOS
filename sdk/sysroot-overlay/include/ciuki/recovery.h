/* G14: startup recovery (call 99, CIUKI_SYS_RECOVERY; records and flags in
 * ciuki/abi.h, docs/design/startup-recovery.md): the boot status, the
 * Automatic Skip Driver list, selective startup and ScanDisk.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_USER_RECOVERY_H
#define CIUKI_USER_RECOVERY_H
#include <stddef.h>
#include <ciuki/abi.h>
/* Each returns 0, or -1 with errno. Version and size are filled in.
 * status: this start's boot flags and the boot-status record (flags lacks
 * CIUKI_RECOVERY_VALID when the system volume has none). */
int ciuki_recovery_status(struct ciuki_recovery_status *status);
/* Selective startup classes and requests for the next start (EPERM
 * without the recovery authority, ENODEV without a boot-status sector). */
int ciuki_recovery_configure(uint8_t disabled_classes, uint8_t requests);
/* The Automatic Skip Driver agent may load these classes again. */
int ciuki_recovery_enable(uint8_t classes);
/* An on-demand check of drive (0 = A:) with CIUKI_SCANDISK_* flags; the
 * report is in *out (out->result is the check's own outcome). */
int ciuki_scandisk(unsigned drive, uint32_t flags, struct ciuki_scandisk *out);
/* The check that ran on drive at this start (RAN clear: none). */
int ciuki_scandisk_report(unsigned drive, struct ciuki_scandisk *out);
/* sec-sysvc: SYSTEM services of call 99 (abi.h "SYSTEM services"). Each
 * returns 0, or -1 with errno (EPERM: not an administrator; ENOENT: no such
 * log, or LIST past the last entry).
 * boot_config_set: C:\SYSTEM\BOOT.CFG becomes text (administrators).
 * system_log_list/read: scope CIUKI_SYSTEM_LOG_MACHINE (the system logs,
 * administrators) or CIUKI_SYSTEM_LOG_USER (the caller's own logs); read
 * copies at most CIUKI_SYSTEM_LOG_DATA_MAX bytes from offset, *total is the
 * file size. system_log_write: append (replace 0; the file starts over past
 * limit, 0 for CIUKI_SYSTEM_LOG_LIMIT) or rewrite one of the caller's own
 * logs; MACHINE accepts only an administrator's append of SCANDISK.LOG and
 * SFC.LOG. driver_pack: the job record (version, size and action filled in by
 * the caller); job->reason receives the outcome even on failure. */
int ciuki_boot_config_set(const char *text, size_t length);
int ciuki_system_log_list(unsigned scope, unsigned index, char name[CIUKI_SYSTEM_LOG_NAME_MAX], uint32_t *total);
int ciuki_system_log_read(unsigned scope, const char *name, uint32_t offset, void *buffer, size_t capacity,
                          size_t *got, uint32_t *total);
int ciuki_system_log_write(unsigned scope, const char *name, int replace, const void *data, size_t bytes, uint32_t limit);
int ciuki_driver_pack(struct ciuki_driver_pack_job *job);
/* System File Checker "Restore file" (administrators): source + path must be
 * the baseline's original (EILSEQ otherwise, ENOENT: not in SFC.LST). */
int ciuki_sfc_restore(const char *path, const char *source);
#endif
