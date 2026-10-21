/* Private DFV2 binding for the unified CiukiDOS guest (f3-56).
 * Wire layout and stack diagrams: docs/validation/f3-56-unified-binding.md.
 * Kernel-internal: no FreeDOS structure, no native pointer and no public
 * DOS interface. Version 1 (dos_file_hook) is separate and unchanged.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOS_UNIFIED_H
#define CIUKI_DOS_UNIFIED_H
#include "dos_file.h"

/* Proposed reservation, pending the lead: F000:1C00-1C3F of the protected
 * F1 page, after v1 (1800-1B53) and clear of BIOS/DPMI/mouse regions. */
#define DFV2_ROM_SEG 0xf000u
#define DFV2_DIRECT 0x1c00u
#define DFV2_LOCAL 0x1c04u
#define DFV2_EVENT 0x1c08u
#define DFV2_CRITICAL 0x1c0cu
#define DFV2_DESCRIPTOR 0x1c10u
#define DFV2_END 0x1c40u
#define DFV2_VERSION 2u
#define DFV2_FEATURES 0x3fu         /* direct, local, event, critical, late bind, CP437 */
#define DFV2_DESCRIPTOR_BYTES 48u
#define DFV2_CALL_BYTES 48u
#define DFV2_BIND_BYTES 0xd0u
#define DFV2_PROCESSES 32u
#define DFV2_SFT_MAX 255u
#define DFV2_SFT_BLOCKS 16u
#define DFV2_SDA_FIELDS 10u
#define DFV2_NLS_TABLES 6u
#define DFV2_ROLE_DIRECT 1u
#define DFV2_ROLE_LOCAL 2u
#define DFV2_ROLE_EVENT 3u
#define DFV2_STATUS_CHAIN 0u
#define DFV2_STATUS_DONE 1u
#define DFV2_EVENT_BIND 0u

enum dfv2_outcome {
    DFV2_REFUSED,   /* nothing written: not a valid gate call */
    DFV2_RESUME,    /* call record completed; resume after INT FE */
    DFV2_SUSPEND    /* monitor-only PENDING: run the critical callback */
};
/* SDA fields in bind-record order and their FreeDOS/RBIL offsets. */
enum dfv2_field { DFV2_ERRMODE, DFV2_INDOS, DFV2_LOCUS, DFV2_ERROR, DFV2_ACTION,
    DFV2_CLASS, DFV2_DTA, DFV2_PSP, DFV2_RETCODE, DFV2_DRIVE };
enum dfv2_nls { DFV2_COUNTRY, DFV2_UPCASE, DFV2_FUPCASE, DFV2_TERMINATOR,
    DFV2_COLLATE, DFV2_DBCS };

struct dfv2_call {
    uint32_t generation,serial;
    uint16_t role,status,flags,ax,bx,cx,dx,si,di,bp,ds,es,frame_ss,frame_sp,arg;
};
struct dfv2_layout {
    /* phase 0: the driver's resident registration */
    uint16_t driver_seg,driver_bytes,call_off,stack_off,stack_bytes,site[4];
    uint8_t driver_id[16];
    /* phase 1: the final FreeDOS layout */
    uint8_t kernel_id[16];
    uint16_t data_seg,data_bytes,text_seg,text_bytes,sda_bytes,sft_entry,sft_count;
    uint16_t cds_count,cds_stride,root,blocks;
    struct dos_file_far lol,sda,field[DFV2_SDA_FIELDS],sft_head,cds,nul,nls[DFV2_NLS_TABLES];
    struct dos_file_far block[DFV2_SFT_BLOCKS],slot[DFV2_SFT_MAX];
    uint16_t block_count[DFV2_SFT_BLOCKS];
    /* the one country-1/CP437 definition, snapshotted at the final bind */
    uint8_t country[41],upcase[130],fupcase[130],terminator[24],collate[258],dbcs[4];
    uint8_t claim[32];
};
struct dfv2_process {
    bool used,provisional,resident;
    uint8_t kind;             /* 0 root, 1 EXEC, 26h copy, 55h child */
    uint16_t psp,parent;      /* parent is immutable after creation */
};
struct dfv2_critical { uint16_t ax,di,bp,si; };
/* Native backend (fixture in f3-56, live VFS owner in f3-57). The adapter
 * finishes every guest check before prepare, so a successful prepare is
 * always followed by exactly one commit and nothing can fail in between;
 * a failing prepare must leave nothing reserved (its DOS error is
 * returned). request/complete return CHAIN, DONE or PENDING; they may
 * update the call's register fields and FLAGS only for DONE. */
struct dfv2_backend {
    int (*prepare)(void *,unsigned event,uint16_t psp,uint16_t parent);
    void (*commit)(void *,unsigned event,uint16_t psp);
    enum dos_file_result (*request)(void *,unsigned role,struct dfv2_call *,struct dfv2_critical *);
    enum dos_file_result (*complete)(void *,unsigned reply,struct dfv2_call *);
    void (*destroy)(void *);
    const uint8_t *claim;     /* 32-byte INT 21h AH bitmap for the direct role, or 0 */
    void *arg;
};
struct dfv2_stats { uint32_t refused,chained,done,suspended,completed,events,binds; };
struct dos_unified {
    struct dos_file_ops memory;
    struct dfv2_backend backend;
    struct dfv2_layout layout;
    struct dfv2_process process[DFV2_PROCESSES];
    uint32_t generation,serial;
    uint16_t selected,owning;
    bool registered,bound,dead,pending;
    uint64_t token,tokens;
    struct dfv2_call suspended;
    struct dfv2_critical critical;
    struct dfv2_stats stats;
    const char *why;          /* last refusal/failure reason (static text) */
};

void dos_unified_rom(uint8_t *page_f1,uint32_t generation);
bool dos_unified_discover(struct dos_file_frame *);
void dos_unified_init(struct dos_unified *,uint32_t generation,const struct dos_file_ops *,const struct dfv2_backend *);
/* live: CS:IP after INT FE, SS:SP at the gate, DS:SI (phase-0 record). */
enum dfv2_outcome dos_unified_gate(struct dos_unified *,const struct dos_file_frame *live);
/* Monitor: where the critical callback frame goes, and its return. */
bool dos_unified_callback_stack(const struct dos_unified *,uint16_t *ss,uint16_t *sp);
enum dfv2_outcome dos_unified_critical(struct dos_unified *,const struct dos_file_frame *live,unsigned reply);
/* VM stop: event 8. Idempotent; a late completion is refused. */
void dos_unified_destroy(struct dos_unified *);
#endif
