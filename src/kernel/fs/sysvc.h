/* sec-sysvc: SYSTEM services for administrator operations (K-Q4) and the
 * system/per-user log model (user-security-kernel.md "SYSTEM services and
 * logs"). Kernel-internal; proc/syscalls_sysvc.c is the call-99 adapter that
 * copies user records and supplies the caller's source reader and the NDIS
 * device observations. Every entry checks the retained subject first; the
 * work then runs on a kernel table (SYSTEM) for the fixed SYSTEM targets, or
 * on a table bound to the caller's subject for the caller's own data.
 * Results are 0 or a negative errno. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FS_SYSVC_H
#define CIUKI_FS_SYSVC_H
#include "fs_port.h"
#include "path.h"
struct vfs;
struct security_subject;
struct ndp_manifest;
struct ndp_device;
struct ciuki_file;

#define SYSVC_LOGS "/System/Logs"
#define SYSVC_HOLD "/System/Logs/Users"
#define SYSVC_USER_LOGS "/Local Settings/Application Data/CiukiOS/Logs"
#define SYSVC_BOOT_CONFIG "/System/BOOT.CFG"
#define SYSVC_DRIVERS "/System/drivers"
#define SYSVC_REASON 160u

/* The system volume. Host harnesses attach their VFS; the kernel uses the
 * storage's C: when nothing is attached (not in Setup mode). */
void sysvc_attach(struct vfs *, unsigned drive);
/* A live administrator, or SYSTEM outside any session. */
bool sysvc_admin(const struct security_subject *);

int sysvc_boot_config_set(struct security_subject *, const char *text, size_t length);

/* scope: CIUKI_SYSTEM_LOG_MACHINE or _USER (abi.h). name[48] for LIST. */
int sysvc_log_list(struct security_subject *, unsigned scope, unsigned index, char name[48], uint32_t *total);
int sysvc_log_read(struct security_subject *, unsigned scope, const char *name, uint32_t offset,
                   void *out, size_t capacity, size_t *done, uint32_t *total);
int sysvc_log_write(struct security_subject *, unsigned scope, const char *name, bool replace,
                    uint32_t limit, const void *data, size_t bytes, uint32_t *total);

/* Driver packs (sysvc_drivers.c). The adapter supplies the caller-side
 * reader (relative path below the package folder, read as the caller, the
 * whole file frozen) and the device observations; reason gets the outcome. */
struct sysvc_pack_ops {
    void *ctx;
    int (*open)(void *ctx, const char *relative, struct ciuki_file *out);
    /* 1: the adapter is present and not quarantined, 0: not, <0: unknown. */
    int (*present)(void *ctx, const struct ndp_device *);
    /* 1: an imported host is active or quarantined (unknown counts as busy). */
    int (*busy)(void *ctx, const struct ndp_device *);
    /* 1: the lead qualification record admits this binary and seal. */
    int (*qualified)(void *ctx, const struct ndp_device *, const uint8_t hash[32], const uint8_t seal[32]);
};
int sysvc_pack_install(struct security_subject *, const struct sysvc_pack_ops *, const struct ndp_manifest *,
                       const char (*sources)[192], char reason[SYSVC_REASON]);
int sysvc_pack_remove(struct security_subject *, const struct sysvc_pack_ops *, const struct ndp_device *,
                      char reason[SYSVC_REASON]);

/* Shared by sysvc.c and sysvc_drivers.c. */
struct vfs_table;
int sysvc_table(struct vfs_table *, struct security_subject *bound, unsigned *drive);
int sysvc_path(char out[FS_PATH_BYTES], unsigned drive, const char *a, const char *b, const char *c);
int sysvc_error(int);
#endif
