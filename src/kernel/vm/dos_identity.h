/* sec-k05: the DOS VMM's single security adapter. A DOS VM captures its
 * creator's immutable retained subject (uid, groups, session) before guest
 * boot and keeps it for its whole lifetime: V86/DPMI workers, the DFV2 file
 * gates, deferred I/O and its private disk. No guest PSP, environment,
 * command line or kernel field selects or changes it.
 * user-security-contract.md 8.4, user-security-kernel.md "Removable media,
 * DOS and Win32" and "DOSVM operations (call 72)".
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOS_IDENTITY_H
#define CIUKI_DOS_IDENTITY_H
#include <ciuki/abi.h>
#include "../fs/fs_port.h"
#include "dos_host_gate.h"
struct process;
struct security_subject;
struct vfs_table;
/* DOS spellings on the system volume are bounded by this size, including
 * the longest store file name (G<game>K<kernel><sha256>V<generation>.COW). */
#define DOS_IDENTITY_PATH 192u
struct dos_identity {
    struct security_subject *subject;  /* one counted reference; NULL only for a
                                        * kernel-internal VM on a host build
                                        * without the credential service */
    struct ciuki_credential credential; /* immutable copy taken at capture */
    char profile[64];  /* committed canonical profile root ("/Windows/Profiles/u...");
                        * empty for the interim bootstrap session and SYSTEM */
    bool system;       /* kernel-internal VM (probes, conformance): never user-reachable */
    bool admin, guest;
};
/* Data roots, DOS-spelled on the system volume (drive C: of the host VFS).
 * With a committed profile every location is inside PROFILE(uid); the
 * legacy machine locations remain only for the interim bootstrap session
 * (no committed profile, an administrator) and kernel-internal VMs. */
enum dos_identity_root {
    DOS_ROOT_STORE,     /* persistent per-title COW and its archives */
    DOS_ROOT_SCRATCH,   /* transient overlays of shell/fixture VMs */
    DOS_ROOT_EXPORT,    /* destination folder of SAVE_EXPORT */
};
/* Capture the calling process's identity for an own-session operation:
 * a live retained subject in an ACTIVE interactive session. SYSTEM/session
 * 0 or a closing/dead session is refused with -EPERM; nothing is retained
 * on failure. */
int dos_identity_capture(struct dos_identity *, struct process *);
/* Kernel-internal VM (no user caller): the explicit SYSTEM subject of PID 1
 * when the credential service is present. */
void dos_identity_internal(struct dos_identity *);
int dos_identity_copy(struct dos_identity *, const struct dos_identity *);
void dos_identity_release(struct dos_identity *);
/* The retained subject is still live (session ACTIVE, not revoked). */
bool dos_identity_live(const struct dos_identity *);
/* p is the same live credential (uid, groups and session) as the identity. */
bool dos_identity_owner(const struct dos_identity *, const struct process *);
/* Predicates on a calling process's live retained subject (call 72):
 * interactive = an ACTIVE authenticated session (own-session class);
 * administrator = uid 0 or Administrators (544). Never a path or flag. */
bool dos_process_interactive(const struct process *);
bool dos_process_admin(const struct process *);
/* Identity of the session (0 for SYSTEM/internal). */
static inline uint32_t dos_identity_session(const struct dos_identity *id)
{ return id->system ? 0 : id->credential.session_id; }
int dos_identity_root(const struct dos_identity *, enum dos_identity_root, char *out, size_t size);
/* Text naming an export folder to the user, at most 31 characters:
 * "Documents\Saves\G1-0" inside a profile, the full legacy path otherwise. */
int dos_identity_export_name(const struct dos_identity *, const char *folder, char out[32]);
/* Create the missing components of a data root below the profile (or below
 * the legacy machine directory). Existing directories are accepted. */
int dos_identity_prepare(const struct dos_identity *, struct vfs_table *, enum dos_identity_root);
/* sec-k03 seam: bind the retained subject to a table the VMM uses for this
 * VM, so the common VFS entry points authorize every operation on it as
 * that subject. No-op until sec-k03 provides vfs_table_subject. */
int dos_identity_bind(const struct dos_identity *, struct vfs_table *);
/* Host-volume gate for a guest view of D:/E:; NULL for SYSTEM/internal VMs,
 * whose views are not user-reachable. The gate lives in the identity's VM. */
void dos_identity_gate(const struct dos_identity *, struct dos_host_gate *);
#endif
