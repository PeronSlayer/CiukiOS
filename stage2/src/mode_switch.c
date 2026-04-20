/*
 * OPENGEM-044 Task A — Mode-switch engine (long ↔ legacy 32-bit PM).
 *
 * Stage-2: the asm trampoline is compiled in and linked, but is gated
 * behind a second arm flag (trampoline-live) that defaults to 0. While
 * the trampoline flag is 0, mode_switch_run_legacy_pm returns
 * MODE_SWITCH_ERR_NOT_IMPLEMENTED even if the API arm flag is set.
 *
 * Sentinel inspected by the gate: "OPENGEM-044-A".
 */

#include "mode_switch.h"

static const char opengem_044_a_sentinel[] = "OPENGEM-044-A";

/* Stage-1 arm state (API arm). */
static int s_mode_switch_armed = 0;

/* Stage-2 arm state (trampoline live). */
#define MODE_SWITCH_TRAMPOLINE_ARM_MAGIC   0xC1D3944Au
static int s_mode_switch_trampoline_live = 0;

/* Scratch struct: offsets MUST match mode_switch_asm.S. */
typedef struct __attribute__((packed, aligned(16))) {
    uint64_t saved_rbx;        /* 0x00 */
    uint64_t saved_rbp;        /* 0x08 */
    uint64_t saved_r12;        /* 0x10 */
    uint64_t saved_r13;        /* 0x18 */
    uint64_t saved_r14;        /* 0x20 */
    uint64_t saved_r15;        /* 0x28 */
    uint64_t saved_rsp;        /* 0x30 */
    uint64_t saved_cr3;        /* 0x38 */
    uint64_t saved_cr4;        /* 0x40 */
    uint32_t saved_efer_lo;    /* 0x48 */
    uint32_t saved_efer_hi;    /* 0x4C */
    uint64_t saved_cr0;        /* 0x50 */
    uint8_t  host_gdtr[10];    /* 0x58 */
    uint8_t  host_gdtr_pad[6]; /* 0x62 → next @ 0x68 */
    uint8_t  host_idtr[10];    /* 0x68 */
    uint8_t  host_idtr_pad[6]; /* 0x72 → next @ 0x78 */
    uint8_t  legacy_gdtr[10];  /* 0x78 */
    uint8_t  legacy_gdtr_pad[6]; /* 0x82 → next @ 0x88 */
    uint64_t pm32_stack_top;   /* 0x88 */
    uint64_t body_fn;          /* 0x90 */
    uint64_t body_user;        /* 0x98 */
    uint64_t result;           /* 0xA0 */
    uint64_t saved_rflags;     /* 0xA8 */
} mode_switch_scratch_t;

_Static_assert(__builtin_offsetof(mode_switch_scratch_t, saved_rbx)       == 0x00, "SCR_RBX");
_Static_assert(__builtin_offsetof(mode_switch_scratch_t, saved_rsp)       == 0x30, "SCR_RSP");
_Static_assert(__builtin_offsetof(mode_switch_scratch_t, saved_cr3)       == 0x38, "SCR_CR3");
_Static_assert(__builtin_offsetof(mode_switch_scratch_t, saved_cr0)       == 0x50, "SCR_CR0");
_Static_assert(__builtin_offsetof(mode_switch_scratch_t, host_gdtr)       == 0x58, "SCR_HOST_GDTR");
_Static_assert(__builtin_offsetof(mode_switch_scratch_t, host_idtr)       == 0x68, "SCR_HOST_IDTR");
_Static_assert(__builtin_offsetof(mode_switch_scratch_t, legacy_gdtr)     == 0x78, "SCR_LEGACY_GDTR");
_Static_assert(__builtin_offsetof(mode_switch_scratch_t, pm32_stack_top)  == 0x88, "SCR_PM32_STACK");
_Static_assert(__builtin_offsetof(mode_switch_scratch_t, body_fn)         == 0x90, "SCR_BODY_FN");
_Static_assert(__builtin_offsetof(mode_switch_scratch_t, body_user)       == 0x98, "SCR_BODY_USER");
_Static_assert(__builtin_offsetof(mode_switch_scratch_t, result)          == 0xA0, "SCR_RESULT");
_Static_assert(__builtin_offsetof(mode_switch_scratch_t, saved_rflags)    == 0xA8, "SCR_RFLAGS");

static mode_switch_scratch_t s_mode_switch_scratch;
static uint64_t s_legacy_gdt[4] __attribute__((aligned(16)));
static uint8_t  s_pm32_stack[64 * 1024] __attribute__((aligned(16)));

extern int mode_switch_asm_enter(mode_switch_scratch_t *scratch);
extern const char mode_switch_asm_sentinel[];

static uint64_t encode_gdt_entry(uint32_t base, uint32_t limit,
                                 uint8_t access, uint8_t flags)
{
    uint64_t d = 0;
    d |= (uint64_t)(limit & 0xFFFFu);
    d |= (uint64_t)(base & 0xFFFFu) << 16;
    d |= (uint64_t)((base >> 16) & 0xFFu) << 32;
    d |= (uint64_t)access << 40;
    d |= (uint64_t)((limit >> 16) & 0x0Fu) << 48;
    d |= (uint64_t)(flags & 0x0Fu) << 52;
    d |= (uint64_t)((base >> 24) & 0xFFu) << 56;
    return d;
}

static void build_legacy_gdt(void)
{
    s_legacy_gdt[0] = 0;
    /* CODE64 flat: access=0x9A (P|S|type=code exec/read), flags=0xA (G|L) */
    s_legacy_gdt[1] = encode_gdt_entry(0, 0xFFFFFu, 0x9A, 0xA);
    /* CODE32 flat: access=0x9A, flags=0xC (G|D) */
    s_legacy_gdt[2] = encode_gdt_entry(0, 0xFFFFFu, 0x9A, 0xC);
    /* DATA32 flat: access=0x92 (P|S|type=data rw), flags=0xC (G|D) */
    s_legacy_gdt[3] = encode_gdt_entry(0, 0xFFFFFu, 0x92, 0xC);
}

static void build_gdtr(uint8_t *dst, uint64_t base, uint16_t limit)
{
    dst[0] = (uint8_t)(limit & 0xFFu);
    dst[1] = (uint8_t)((limit >> 8) & 0xFFu);
    dst[2] = (uint8_t)(base & 0xFFu);
    dst[3] = (uint8_t)((base >> 8) & 0xFFu);
    dst[4] = (uint8_t)((base >> 16) & 0xFFu);
    dst[5] = (uint8_t)((base >> 24) & 0xFFu);
    dst[6] = (uint8_t)((base >> 32) & 0xFFu);
    dst[7] = (uint8_t)((base >> 40) & 0xFFu);
    dst[8] = (uint8_t)((base >> 48) & 0xFFu);
    dst[9] = (uint8_t)((base >> 56) & 0xFFu);
}

int mode_switch_arm(uint32_t magic)
{
    if (magic != MODE_SWITCH_ARM_MAGIC) {
        return MODE_SWITCH_ERR_BAD_INPUT;
    }
    s_mode_switch_armed = 1;
    return MODE_SWITCH_OK;
}

void mode_switch_disarm(void)
{
    s_mode_switch_armed = 0;
    s_mode_switch_trampoline_live = 0;
}

int mode_switch_is_armed(void)
{
    return s_mode_switch_armed;
}

int mode_switch_trampoline_arm(uint32_t magic)
{
    if (magic != MODE_SWITCH_TRAMPOLINE_ARM_MAGIC) {
        return MODE_SWITCH_ERR_BAD_INPUT;
    }
    s_mode_switch_trampoline_live = 1;
    return MODE_SWITCH_OK;
}

void mode_switch_trampoline_disarm(void)
{
    s_mode_switch_trampoline_live = 0;
}

int mode_switch_trampoline_is_live(void)
{
    return s_mode_switch_trampoline_live;
}

int mode_switch_run_legacy_pm(mode_switch_legacy_pm_body_fn body, void *user)
{
    if (!s_mode_switch_armed) {
        return MODE_SWITCH_ERR_NOT_ARMED;
    }
    if (body == (mode_switch_legacy_pm_body_fn)0) {
        return MODE_SWITCH_ERR_BAD_INPUT;
    }
    if (!s_mode_switch_trampoline_live) {
        return MODE_SWITCH_ERR_NOT_IMPLEMENTED;
    }

    mode_switch_scratch_t *scr = &s_mode_switch_scratch;
    for (uint64_t i = 0; i < sizeof(*scr); ++i) {
        ((volatile uint8_t *)scr)[i] = 0;
    }

    build_legacy_gdt();
    build_gdtr(scr->legacy_gdtr,
               (uint64_t)(uintptr_t)s_legacy_gdt,
               (uint16_t)(sizeof(s_legacy_gdt) - 1));

    scr->pm32_stack_top = (uint64_t)(uintptr_t)(s_pm32_stack + sizeof(s_pm32_stack));
    scr->pm32_stack_top &= ~(uint64_t)0xFu;

    scr->body_fn   = (uint64_t)(uintptr_t)body;
    scr->body_user = (uint64_t)(uintptr_t)user;

    (void)mode_switch_asm_enter(scr);
    return MODE_SWITCH_OK;
}

int mode_switch_probe(void)
{
    mode_switch_disarm();
    if (mode_switch_run_legacy_pm((mode_switch_legacy_pm_body_fn)(uintptr_t)0x1, (void *)0) != MODE_SWITCH_ERR_NOT_ARMED) {
        return -1;
    }

    if (mode_switch_arm(0xDEADBEEFu) != MODE_SWITCH_ERR_BAD_INPUT) {
        return -2;
    }
    if (mode_switch_is_armed() != 0) {
        return -3;
    }

    if (mode_switch_arm(MODE_SWITCH_ARM_MAGIC) != MODE_SWITCH_OK) {
        return -4;
    }
    if (mode_switch_is_armed() != 1) {
        return -5;
    }

    if (mode_switch_run_legacy_pm((mode_switch_legacy_pm_body_fn)0, (void *)0) != MODE_SWITCH_ERR_BAD_INPUT) {
        return -6;
    }

    if (mode_switch_run_legacy_pm((mode_switch_legacy_pm_body_fn)(uintptr_t)0x1, (void *)0) != MODE_SWITCH_ERR_NOT_IMPLEMENTED) {
        return -7;
    }

    if (mode_switch_trampoline_arm(0xBADBADu) != MODE_SWITCH_ERR_BAD_INPUT) {
        return -8;
    }
    if (mode_switch_trampoline_is_live() != 0) {
        return -9;
    }

    if (opengem_044_a_sentinel[0] != 'O') {
        return -10;
    }
    if (mode_switch_asm_sentinel[0] != 'O') {
        return -11;
    }

    mode_switch_disarm();
    return 0;
}
