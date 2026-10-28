/* f3-57 live native backend of the unified CiukiDOS binding: the FreeDOS
 * fork's redirector callouts (INT 2Fh/11xxh at the seven int2f.asm sites)
 * for the drives this VMM owns, over the production VFS. The fork keeps
 * FreeDOS's SFT/JFT/CDS/SDA bookkeeping; this file owns only the native
 * descriptions (one per SFT slot), search cursors and the activation
 * transaction that converts the owned CDS entries to IFS drives (C000h).
 * Decisions and sources: docs/validation/f3-57-unified-file-channel.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOS_UNIFIED_IFS_H
#define CIUKI_DOS_UNIFIED_IFS_H
#include "dos_unified.h"
#include "dos_host_gate.h"
#define DFV2_IFS_SEARCHES 32u
/* FreeDOS K2043 SDA offsets (kernel.asm:584-760), all inside 78Ch bytes. */
#define DFV2_SDA_DTA 0x0cu
#define DFV2_SDA_PSP 0x10u
#define DFV2_SDA_FN1 0x9eu
#define DFV2_SDA_FN2 0x11eu
#define DFV2_SDA_SDB 0x19eu
#define DFV2_SDA_ATTR 0x24du
#define DFV2_SDA_EXT_ACTION 0x2ddu
#define DFV2_SDA_EXT_ATTR 0x2dfu
#define DFV2_SDA_EXT_MODE 0x2e1u
#define DFV2_CDS_IFS 0xc000u        /* CDSNETWDRV|CDSPHYSDRV: installable FS */

/* sec-k05: a host view (D:, E:) carries the VM identity's gate; the
 * private C: provider has none. */
struct dfv2_ifs_view { struct vfs_table *table; uint8_t drive; bool readonly; uint32_t generation;
    const struct dos_host_gate *gate; };
struct dfv2_ifs_file { bool used; uint8_t guest; int handle; uint32_t nonce,generation; struct vfs_table *table;
    void *object; /* gated view: the authorized object, rechecked per transfer */ };
struct dfv2_ifs_search { bool used; uint8_t guest; uint16_t psp; uint32_t stamp,nonce; struct vfs_table *table; struct vfs_find find;
    void *directory; /* gated view: the held, authorized directory whose entries are filtered */ };
struct dfv2_ifs_stats {
    uint32_t requests,owned,chained,failed,critical,retries,expired,forged,reclaimed;
    uint32_t opens,closes,reads,writes,finds,activations,refusals;
    uint32_t denied,hidden; /* sec-k05: host-view gate denials, entries not listed */
    uint64_t read_bytes,written_bytes;
};
/* f3-61c: the first owned requests that ended in a DOS error, for evidence
 * records: redirector function (11xxh), DOS error and the 8.3 leaf name
 * (path calls: FN1; handle calls: the SFT name; none for find-next). */
#define DFV2_IFS_FAILURES 8u
struct dfv2_ifs_failure { uint16_t fn,error; char name[13]; };
struct dfv2_ifs;
/* Host side of the activation barrier (production: dos_file_vm.c mounts the
 * VM's logical FAT16 view through f3-47; tests: an in-memory provider).
 * mount maps views with dfv2_ifs_map and fails without leaving anything;
 * unmount undoes a successful mount (failed later step or VM stop). */
struct dfv2_ifs_host {
    int (*mount)(void *,struct dfv2_ifs *);
    void (*unmount)(void *,struct dfv2_ifs *);
    void *arg;
};
struct dfv2_ifs {
    struct dos_unified *unified;
    struct dos_file_ops ops;
    struct dfv2_ifs_host host;
    uint32_t vm,generation,stamp,nonce;
    struct dfv2_ifs_view view[26];
    uint32_t owned;               /* drives whose CDS this binding converted */
    bool mounted,active;
    struct dfv2_ifs_file file[DFV2_SFT_MAX];
    struct dfv2_ifs_search search[DFV2_IFS_SEARCHES];
    struct vfs_request request; struct vfs_wait_info wait; int cleanup_status;
    struct { bool armed; struct dfv2_call call; uint64_t deadline; } retry;
    uint64_t started;
    struct dfv2_ifs_stats stats;
    struct dfv2_ifs_failure failure[DFV2_IFS_FAILURES];   /* first stats.failed entries */
    const char *why;
    uint8_t io[4096];
};
void dfv2_ifs_init(struct dfv2_ifs *,struct dos_unified *,const struct dos_file_ops *,
                   const struct dfv2_ifs_host *,uint32_t vm,uint32_t generation);
/* During host.mount only: guest drive -> native VFS drive of a table. */
int dfv2_ifs_map(struct dfv2_ifs *,unsigned guest,struct vfs_table *,unsigned native,bool readonly);
/* sec-k05: a read-only host view (D:/E:) whose every path, enumeration and
 * transfer is authorized through gate as the VM's retained subject. A host
 * view without a gate is refused. */
int dfv2_ifs_map_host(struct dfv2_ifs *,unsigned guest,struct vfs_table *,unsigned native,
                      const struct dos_host_gate *gate);
/* Fill the DFV2 backend; prepare/commit of event 0 phase 1 is the barrier. */
void dfv2_ifs_backend(struct dfv2_ifs *,struct dfv2_backend *);
/* Number of native descriptions/searches still held (ledger checks). */
unsigned dfv2_ifs_held(const struct dfv2_ifs *);
#endif
