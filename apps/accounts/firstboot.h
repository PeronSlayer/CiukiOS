/* SPDX-License-Identifier: MIT
 * ACCOUNT first-boot adapter (sec-user-02, user-security-contract.md 3.6,
 * 5.1, 5.2 and 6.3). Two steps around acct_service_open:
 *
 *  - acct_firstboot_prepare, before open, under the ACCOUNT lease: an upgraded
 *    G32 system (a shared /System/Settings/users.dat and no v2 evidence at all)
 *    gets SYSTEM-only migration evidence in /System/Security (migration.*), an
 *    origin-3 UNPROVISIONED bootstrap.state bound to that evidence, and its
 *    shared users.dat/users.new/users.lock removed. Restartable through the
 *    protected migration.marker; never imports twice, never creates an account.
 *  - acct_firstboot_promote, after an open that found no published database:
 *    an origin 1/2 UNPROVISIONED record promotes the staged provision.seed
 *    (acct_service_bootstrap: entropy SID, owner batch, CONSUMED) and then
 *    removes the residual seed; an origin-3 record hands the protected G32
 *    evidence to acct_enrollment_source, after which only the trusted
 *    enrollment workflow (AUTH ENROLL_PLAN/ENROLL) can publish accounts.
 *
 * Neither step creates a seed, a password or a default account. Missing or
 * contradictory evidence leaves the service closed (EIO).
 */
#ifndef ACCT_FIRSTBOOT_H
#define ACCT_FIRSTBOOT_H
#include "accounts.h"

/* The shared G32 settings directory (/System/Settings): names are exactly
 * "users.dat", "users.new" and "users.lock". read reports ENOENT. */
struct acct_legacy_io {
    void *ctx;
    int (*read)(void *, const char *name, void *, size_t, size_t *);
    int (*remove)(void *, const char *name);
    int (*barrier)(void *);
};
/* SYSTEM-only migration evidence: plain names in the protected store
 * directory (/System/Security), the G32 file name after the prefix. */
#define ACCT_MIGRATION_PREFIX "migration."
#define ACCT_MIGRATION_MARKER "migration.marker"
#define ACCT_MIGRATION_SOURCE "migration.users.dat"
#define ACCT_LEGACY_MAX 1400u /* G32 users.dat bound (acct_import_g32) */

enum acct_firstboot_result {
    ACCT_FIRSTBOOT_NOTHING = 0,   /* provisioned, or no first-boot evidence */
    ACCT_FIRSTBOOT_PREPARED = 1,  /* G32 evidence protected, origin-3 state staged */
    ACCT_FIRSTBOOT_PROMOTED = 2,  /* seed promoted: database published, CONSUMED */
    ACCT_FIRSTBOOT_ENROLLMENT = 3 /* enrollment source registered; waiting for the trusted plan */
};
/* store: the sealed tables passed to acct_service_open (same volume/io).
 * Returns enum acct_firstboot_result or -errno. legacy may be NULL (no G32). */
int acct_firstboot_prepare(const struct acct_store *store, const struct acct_legacy_io *legacy);
int acct_firstboot_promote(struct acct_service *service);
/* POSIX binding of acct_legacy_io for /System/Settings (firstboot_posix.c). */
int acct_legacy_io_posix(struct acct_legacy_io *out);
#endif
