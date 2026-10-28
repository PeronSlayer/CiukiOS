/* sec-k05 host-volume gate type, shared by the DFV2 IFS backend and the
 * identity adapter without pulling the credential ABI into FS_HOST units.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOS_HOST_GATE_H
#define CIUKI_DOS_HOST_GATE_H
struct vfs_table;
/* sec-k05 host-volume gate of a guest view of D:/E: (dos_identity.c). It
 * authorizes the VM's retained subject through the shared resolved-object
 * policy (security_policy.h): every real directory is traversed, never a
 * collapsed string, and 'access' is required on the leaf. The VM-private
 * C: provider has no gate. Results are FS codes: 0; -FS_EACCES for a
 * denial, a revoked subject or missing policy/owner metadata (DOS error 5,
 * never a local-FAT fallback); -FS_ENOENT/-FS_ENOTDIR for an authorized
 * lookup that found nothing.
 * retain authorizes and holds the resolved object; with handle >= 0 it runs
 * after an open and the handle must name that same object (a swap between
 * check and open is refused). child authorizes one entry of a held
 * directory; recheck re-evaluates a held object's current label for the
 * live subject (each transfer); release drops a held object. */
struct dos_host_gate {
    int (*check)(const struct dos_host_gate *,struct vfs_table *,const char *native,unsigned access);
    int (*retain)(const struct dos_host_gate *,struct vfs_table *,const char *native,unsigned access,
                  int handle,void **object);
    int (*child)(const struct dos_host_gate *,struct vfs_table *,void *directory,const char *name,unsigned access);
    int (*recheck)(const struct dos_host_gate *,struct vfs_table *,void *object,unsigned access);
    void (*release)(const struct dos_host_gate *,struct vfs_table *,void *object);
    const void *identity;
};
#endif
