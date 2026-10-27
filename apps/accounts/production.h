/* SPDX-License-Identifier: MIT
 * Sealed production bindings of the OWNER and protected-storage adapter
 * tables used by the ACCOUNT service image (main.c) and by the host
 * integration harness. The kernel table is kernel_adapter.h.
 */
#ifndef ACCT_PRODUCTION_H
#define ACCT_PRODUCTION_H
#include "adapter.h"
/* Interim OWNER backend (sec-k02 not integrated, decision u01-D2): it is
 * compiled into the sealed image, so authenticate succeeds, and every
 * owner-store operation (generation, bindings, root occupancy, staging,
 * tree digests, snapshots, orphan recovery, checkpoints and every
 * RESERVE/PREPARE/COMMIT/ABORT/STATUS call) refuses with ENOSYS. The
 * service therefore never opens on a provisioned store and never commits
 * an owner transaction until the real kernel OWNER endpoint is bound. */
#define ACCT_INTERIM_VOLUME UINT64_C(1)
void acct_owner_interim(struct acct_owner *out);
/* Protected storage over the native file API below one SYSTEM-only
 * directory; see storage_posix.c. */
#define ACCT_IO_PATH 256
#define ACCT_IO_DIRTY 8
struct acct_posix_io {
    char base[128];
    char dirty[ACCT_IO_DIRTY][ACCT_IO_PATH]; /* changed directories below base */
    unsigned dirty_count;
    int overflow;
};
int acct_io_posix(struct acct_posix_io *state, const char *base, struct acct_io *out);
#endif
