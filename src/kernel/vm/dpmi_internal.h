/* Kernel-owned DPMI 0.9 host: private state shared by dpmi*.c and dosvm.c.
 * Research and decisions: docs/design/f3-acceptance.md, section f3-04.
 * Included by dosvm_internal.h after dosvm_memory.h (struct vm_block).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DPMI_INTERNAL_H
#define CIUKI_DPMI_INTERNAL_H
#include <ciuki/dpmi.h>

#define DPMI_LDT_ENTRIES 1024u          /* 8 KiB per VM, supervisor memory */
#define DPMI_SPECIFIC 16u               /* 000Dh: indices 0-15 never used by the host */
#define DPMI_CLIENTS 4u                 /* nested clients per VM */
#define DPMI_CALLBACKS 16u              /* per client (0.9 minimum) */
#define DPMI_DEPTH 32u                  /* suspended transitions per VM (8 nested levels use 16) */
#define DPMI_BLOCKS 128u
#define DPMI_DOS_BLOCKS 32u
#define DPMI_WATCH 4u
#define DPMI_LOCK_PAGES (DOSVM_PAGES + VM_POOL_PAGES)
#define DPMI_HSTACK_BYTES 16384u        /* locked PM stack, segment-limit guarded */
#define DPMI_HSTACK_RESERVE 256u
/* Real-mode host stack: UMA pages C6000h-C7FFFh (writable, outside the UMB
 * arena and the EMS frame); 512 bytes per nested reflection (0.9: >= 200h). */
#define DPMI_RSTACK_SEG 0xC600u
#define DPMI_RSTACK_PAGE 0xC6u
#define DPMI_RSTACK_PAGES 2u
#define DPMI_RSTACK_BYTES 0x2000u
#define DPMI_RSTACK_LEVEL 0x200u

/* LDT layout: client allocations from index 16 up, host entries at the top. */
#define DPMI_IDX_CODE (DPMI_LDT_ENTRIES - 1u)
#define DPMI_IDX_STACK (DPMI_LDT_ENTRIES - 2u)
#define DPMI_IDX_TEMP (DPMI_LDT_ENTRIES - 2u - DPMI_DEPTH) /* callback real SS, one per depth */
#define DPMI_IDX_LAST_CLIENT (DPMI_IDX_TEMP - 1u)
#define DPMI_SEL(index) ((uint16_t)(((index) << 3) | 7u))
#define DPMI_HOST_CS DPMI_SEL(DPMI_IDX_CODE)
#define DPMI_HOST_SS DPMI_SEL(DPMI_IDX_STACK)

/* V86 stubs, segment F000h, each "INT FEh; HLT; HLT" (page F1000h). */
#define DPMI_RM_ENTRY 0x1040u
#define DPMI_RM_RETURN 0x1044u
#define DPMI_RM_RAW 0x1048u
#define DPMI_RM_SAVE 0x104Cu
#define DPMI_RM_CALLBACK 0x1100u        /* + 4 * (client * 16 + slot) */
#define DPMI_RM_END (DPMI_RM_CALLBACK + 4u * DPMI_CLIENTS * DPMI_CALLBACKS)
/* Protected-mode host stubs: HLT bytes in the host code segment at linear
 * F1200h. A ring-3 HLT faults (#GP 0) at an address only the host owns. */
#define DPMI_PM_BASE 0xF1200u
#define DPMI_PM_INT 0x000u              /* default handler of vector n: +n */
#define DPMI_PM_EXC 0x100u              /* default exception handler n: +n */
#define DPMI_PM_IRQRET 0x120u           /* hardware interrupt return (IRET) */
#define DPMI_PM_EXCRET 0x121u           /* exception handler return (RETF) */
#define DPMI_PM_CBRET 0x122u            /* real-mode callback return (IRET) */
#define DPMI_PM_RAW 0x123u              /* raw PM -> real switch (JMP FAR) */
#define DPMI_PM_SAVE 0x124u             /* state save/restore (CALL FAR) */
#define DPMI_PM_SOFTRET 0x125u          /* INT 1Ch/23h/24h reflected from real mode */
#define DPMI_PM_LIMIT 0x1FFu

enum dpmi_kind { DPMI_FREE, DPMI_CLIENT, DPMI_FIXED, DPMI_DOS, DPMI_ALIAS, DPMI_HOST };
enum dpmi_rec_kind {
    DPMI_REC_REALCALL = 1,              /* 0300h-0302h: PM suspended in V86 */
    DPMI_REC_REFLECT,                   /* default PM handler reflected to V86 */
    DPMI_REC_HOSTCALL,                  /* host-issued real-mode INT 21h */
    DPMI_REC_PMINT,                     /* PM handler of an IRQ or INT 1Ch/23h/24h */
    DPMI_REC_CALLBACK,                  /* 0303h procedure */
    DPMI_REC_EXCEPTION,                 /* 0203h handler */
};
enum dpmi_op { DPMI_OP_ENTRY = 1, DPMI_OP_DOS_ALLOC, DPMI_OP_DOS_FREE, DPMI_OP_DOS_RESIZE, DPMI_OP_TERMCHECK };
#define DPMI_RF_IRET 1u                 /* REFLECT: emulate IRET from the PM stack on return */
#define DPMI_RF_HWINT 2u                /* restore the interrupted frame completely */
#define DPMI_RF_RESULTS 4u              /* PMINT from V86: copy registers back (INT 23h/24h) */

struct dpmi_vec { uint32_t off; uint16_t sel; };
struct dpmi_record {
    uint8_t kind, client, vector, flags, op;
    bool saved_pm, vif;
    bool decode_valid;
    uint16_t decode_cs;
    uint32_t decode_eip;             /* interrupted boundary proof, HWINT only */
    bool decode_stack_valid;
    uint32_t decode_stack[8];
    bool decode_ds_valid;
    uint32_t decode_stack_linear, decode_ds_base;
    struct v86_frame saved;
    uint32_t lock_top;
    uint16_t rstack_top;
    uint16_t sel;                       /* call structure / frame selector */
    uint32_t off;                       /* call structure / frame offset */
    uint32_t arg[4];
};
struct dpmi_callback { bool used; uint16_t sel, struct_sel; uint32_t off, struct_off; };
struct dpmi_client {
    bool suspended_if_armed;         /* parent's deferred flags restore across a child */
    bool used;
    uint16_t psp, env, psp_sel, env_sel, rm_ss, rm_sp, rm_cs, rm_ip;
    unsigned depth_base;
    struct dpmi_vec saved_vec[256], saved_exc[32];
    struct dpmi_callback cb[DPMI_CALLBACKS];
};
struct dpmi_block { struct vm_block b; uint32_t handle; uint8_t owner; };
struct dpmi_dos { uint16_t seg, paras, sel, count; uint8_t owner; };
struct dpmi_watch { bool used, hit; uint8_t owner, size, type; uint32_t addr; };
struct dpmi {
    uint32_t ldt[DPMI_LDT_ENTRIES][2] __attribute__((aligned(8)));
    uint8_t kind[DPMI_LDT_ENTRIES], owner[DPMI_LDT_ENTRIES];
    bool active, bits32, shadow, rm_a20;
    /* f3-06 POPF compatibility: a flags image saved before a CLI carries
     * TF, so its POPF/IRET traps once (dpmi_dispatch.c). */
    bool if_armed;
    bool decode_valid;
    uint16_t decode_cs;
    uint32_t decode_eip;            /* known boundary at entry/previous trap return */
    bool decode_stack_valid;
    uint32_t decode_stack[8];       /* root return proof, captured at that boundary */
    bool decode_ds_valid;
    uint32_t decode_stack_linear, decode_ds_base;
    uint16_t shadow_cs;             /* STI shadow: CS:EIP of the shadowed instruction */
    uint32_t shadow_eip;
    unsigned clients, depth;
    uint32_t lock_top, dr7, serial;
    uint16_t rstack_top;
    struct vm_block hstack;
    struct dpmi_vec vec[256], exc[32];
    struct dpmi_client client[DPMI_CLIENTS];
    struct dpmi_record rec[DPMI_DEPTH];
    struct dpmi_block block[DPMI_BLOCKS];
    struct dpmi_dos dos[DPMI_DOS_BLOCKS];
    struct dpmi_watch watch[DPMI_WATCH];
    uint8_t lock[DPMI_LOCK_PAGES], pageable[32];
    struct dpmi_stats stats;
};

struct dosvm;
/* Decoded descriptor for host-side checks and address translation. */
struct dpmi_seg {
    uint32_t base, limit;
    uint8_t access;
    bool valid, present, code, readable, writable, down, big, conforming;
    unsigned dpl;
};

/* dpmi_desc.c */
void dpmi_desc_set(uint32_t d[2], uint32_t base, uint32_t limit, uint8_t access, uint8_t ext);
uint32_t dpmi_desc_base(const uint32_t d[2]);
uint32_t dpmi_desc_limit(const uint32_t d[2]);
bool dpmi_seg(const struct dosvm *, uint16_t sel, struct dpmi_seg *);
bool dpmi_ptr(struct dosvm *, uint16_t sel, uint32_t off, uint32_t bytes, bool write, uint32_t *linear);
int dpmi_alloc_ldt(struct dosvm *, unsigned count, uint8_t kind, uint8_t owner);
void dpmi_free_ldt(struct dosvm *, unsigned index);
int dpmi_seg_data(struct dosvm *, uint8_t kind, uint8_t owner, uint32_t base, uint32_t limit, bool big);
bool dpmi_code_selector(const struct dosvm *, uint16_t sel, uint32_t off);
bool dpmi_frame_check(struct dosvm *, struct v86_frame *);
int dpmi_ldt_service(struct dosvm *, struct v86_frame *, unsigned ax);
void dpmi_zero_segments(struct dosvm *, struct v86_frame *, uint16_t sel);
bool dpmi_client_ptr(struct dosvm *, struct v86_frame *, uint16_t sel, uint32_t off, uint32_t bytes, bool write, uint32_t *linear);
uint32_t dpmi_off(const struct dosvm *, uint32_t reg);

/* dpmi_switch.c */
int dpmi_push_record(struct dosvm *, const struct v86_frame *, uint8_t kind, struct dpmi_record **);
void dpmi_lock_below(struct dosvm *, const struct v86_frame *);
void dpmi_set_pm(struct dosvm *, bool pm);
int dpmi_rm_stack(struct dosvm *, uint16_t *ss, uint16_t *sp);
int dpmi_to_real_int(struct dosvm *, struct v86_frame *, uint8_t kind, uint8_t vector, uint8_t flags);
int dpmi_hostcall(struct dosvm *, struct v86_frame *, uint8_t op, const struct v86_frame *regs);
int dpmi_real_int(struct dosvm *, struct v86_frame *, uint8_t vector, uint16_t image);
bool dpmi_rm_pop(struct dosvm *, struct v86_frame *, uint16_t *);
int dpmi_v86_return(struct dosvm *, struct v86_frame *);
int dpmi_translation(struct dosvm *, struct v86_frame *, unsigned ax);
int dpmi_callback_enter(struct dosvm *, struct v86_frame *, unsigned slot);
int dpmi_callback_return(struct dosvm *, struct v86_frame *);
int dpmi_raw_to_pm(struct dosvm *, struct v86_frame *);
int dpmi_raw_to_real(struct dosvm *, struct v86_frame *);
int dpmi_far_return(struct dosvm *, struct v86_frame *);
bool dpmi_push(struct dosvm *, struct v86_frame *, uint32_t value, unsigned width);
bool dpmi_pop(struct dosvm *, struct v86_frame *, uint32_t *value, unsigned width);
void dpmi_unwind(struct dosvm *, unsigned depth);

/* dpmi_dispatch.c */
int dpmi_pm_interrupt(struct dosvm *, struct v86_frame *, uint8_t vector);
int dpmi_exception(struct dosvm *, struct v86_frame *, unsigned vector, uint32_t error);
int dpmi_default_int(struct dosvm *, struct v86_frame *, uint8_t vector, bool iret);
int dpmi_enter_pm_handler(struct dosvm *, struct v86_frame *, uint8_t vector, uint8_t flags);
int dpmi_stub_trap(struct dosvm *, struct v86_frame *, uint32_t offset);
bool dpmi_handler_live(struct dosvm *, struct dpmi_vec);

/* dpmi.c */
int dpmi_int31(struct dosvm *, struct v86_frame *);
int dpmi_entry(struct dosvm *, struct v86_frame *);
int dpmi_entry_complete(struct dosvm *, struct v86_frame *, uint16_t psp);
void dpmi_reload_debug(struct dosvm *);
int dpmi_terminate(struct dosvm *, bool from_pm);
int dpmi_terminate_int(struct dosvm *, struct v86_frame *, uint8_t vector, const struct v86_frame *regs);
int dpmi_dos_complete(struct dosvm *, struct v86_frame *, struct dpmi_record *, const struct v86_frame *result);
void dpmi_stat(struct dosvm *, unsigned ax);
bool dpmi_linear_owned(struct dosvm *, uint32_t linear, uint32_t bytes);
static inline bool dpmi_is_pm_stub(uint16_t cs, uint32_t eip, uint32_t off) { return cs == DPMI_HOST_CS && eip == off; }

static inline void dpmi_carry(struct v86_frame *f, bool set)
{ f->tf.eflags = (f->tf.eflags & ~1u) | (set ? 1u : 0); }
static inline void dpmi_low(uint32_t *reg, uint32_t value)
{ *reg = (*reg & 0xFFFF0000u) | (uint16_t)value; }
#endif
