/* sec-own: the system volume's OWNER store, policy binding and evidence
 * reader. Kernel-private; the ACCOUNT role reaches it only through the
 * role-bound OWNER link (owner_service.c), never through a public call.
 *
 * Wire extension of the OWNER service (user-security-contract.md 3.5 lists
 * PREPARE=1..RESERVE=7): operations 8..14 carry one fixed 160-byte query
 * record and get one fixed 160-byte reply. The account service mirrors these
 * layouts in apps/accounts/owner_kernel.c; no SDK header exports them.
 *
 * Query request (little endian):
 *   0 u32 version=1   4 u32 bytes=160   8 u32 operation (8..14)  12 u32 uid
 *  16 u64 volume (0 only for IDENTITY)  24 u64 transaction (RECOVER_ORPHANS:
 *     active G or 0; SNAPSHOT: ACCOUNT allocation high G)
 *  32 u32 uid_high (SNAPSHOT)  36 u32 reserved=0
 *  40 u64 accounts_generation (SNAPSHOT)  48 u8 accounts_sha256[32] (SNAPSHOT)
 *  80 char path[64] (ROOT_OCCUPIED, NUL padded)  144 u8 reserved[16]=0
 * Reply:
 *   0 u32 version=1   4 u32 bytes=160   8 u32 operation  12 u32 store state
 *     (0 no store yet, 1 open, 2 unavailable: IDENTITY answers; others EIO,
 *      or EAGAIN while reversible mount recovery is still waiting)
 *  16 u64 volume  24 u64 owner generation  32 u64 checkpoint floor
 *  40 u32 receipt count  44 u32 result (ROOT_OCCUPIED: 1 occupied)
 *  48 u64 owner key  56 u64 binding generation  64 u32 uid  68 u32 action
 *     (1 live, BINDING)  72 char path[64] (BINDING) or, for SNAPSHOT,
 *     u64 image bytes @72 and the image SHA-256 @80  136 u8 reserved[24]=0
 * SNAPSHOT writes the checkpoint image to /System/Security/Owner/snapshot
 * (SYSTEM-only) instead of a service body: it may exceed 8192 bytes.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FS_OWNER_SYSTEM_H
#define CIUKI_FS_OWNER_SYSTEM_H
#include "fs_port.h"
struct vfs;
struct security_subject;
struct ciuki_security_profile;
enum owner_query_op { OWNER_IDENTITY=8, OWNER_GENERATION, OWNER_BINDING, OWNER_ROOT_OCCUPIED,
                      OWNER_SNAPSHOT, OWNER_RECOVER_ORPHANS, OWNER_CHECKPOINT_FLOOR };
#define OWNER_QUERY_BYTES 160u
enum owner_system_state { OWNER_SYSTEM_NONE, OWNER_SYSTEM_UNPROVISIONED, OWNER_SYSTEM_OPEN, OWNER_SYSTEM_FAILED,
                          OWNER_SYSTEM_RECOVERING };
/* Boot binding of the mounted system volume (drive, volume identity
 * partition LBA << 32 | FAT serial, 0 = not managed). An existing store
 * (machine.sid present) is opened and the volume's policy and OWNER lease
 * are bound; without any SID/store the volume stays unmanaged until the
 * gated first RESERVE creates the store. Registers the OWNER dispatcher. */
int owner_system_attach(struct vfs *, unsigned drive, uint64_t volume, bool readonly);
/* Kernel boot glue: storage_get()'s C:, normal boots only (not Setup). */
void owner_system_boot(void);
int owner_system_state(void);
uint64_t owner_system_volume(void);
/* Committed live profile descriptor of uid (interim bootstrap session). */
int owner_system_profile(uint32_t uid, struct ciuki_security_profile *);
int owner_system_dispatch(const struct security_subject *, const void *, size_t, void *, size_t, size_t *);
/* Host harness teardown: closes the store, unbinds the policy. */
void owner_system_detach(void);
#endif
