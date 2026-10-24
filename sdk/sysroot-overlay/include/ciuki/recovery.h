/* G14: startup recovery (call 99, CIUKI_SYS_RECOVERY; records and flags in
 * ciuki/abi.h, docs/design/startup-recovery.md): the boot status, the
 * Automatic Skip Driver list, selective startup and ScanDisk.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_USER_RECOVERY_H
#define CIUKI_USER_RECOVERY_H
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
#endif
