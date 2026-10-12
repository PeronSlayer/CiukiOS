/* Guest CPU for the f3-04 host runner: libx86emu executes guest code; this
 * wrapper adds what a ring-3 V86/DPMI guest of Ciuki VMM sees on a real CPU
 * (Intel SDM Vol. 3): IOPL-sensitive and privileged instructions trap
 * before execution (#GP(0); INT n through DPL0 gates gives #GP(n*8+2);
 * INT3/INT 80h/ICEBP reach their gates), TF single-step after one
 * instruction with the MOV SS delay, and LAR/LSL/VERR/VERW, which libx86emu
 * lacks or fakes, plus ENTER with mixed operand and stack sizes. Evidence quality: an emulator, not hardware; QEMU and the
 * laptops remain the acceptance authority. SPDX-License-Identifier: GPL-2.0-only */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <x86emu.h>
#include "dosvm_guest.h"

#define GDT_FAKE 0xFFFE0000u
#define LDT_FAKE 0xFFFF0000u
static x86emu_t *emu;
static const struct gcpu_ops *ops;
static bool fetch(uint32_t lin, uint8_t *b);
static struct gcpu_event *event;
static bool fault_pending;
/* GUEST_TCG=1 models two QEMU TCG traits seen on qemu-t23 (2026-10-12):
 * data accesses are not checked against segment limits (translate.c adds
 * the base only) and CPUID leaf 40000000h reports "TCGTCGTCGTCG". */
static int tcg_mode = -1;
static bool tcg(void) { if (tcg_mode < 0) tcg_mode = getenv("GUEST_TCG") != 0; return tcg_mode; }
static void tcg_descriptor(uint32_t d[2])
{
    bool data = (d[1] & 0x1800u) == 0x1000u, down = d[1] & 0x400u;
    if (tcg() && data && !down) { d[0] |= 0xFFFFu; d[1] |= 0x008F0000u; }  /* limit FFFFF, G=1 */
}
static unsigned watch_hits;

static unsigned memio(x86emu_t *e, u32 addr, u32 *val, unsigned type)
{
    (void)e;
    unsigned bits = type & 0xFF, kind = type & ~0xFFu;
    unsigned n = bits == X86EMU_MEMIO_16 ? 2 : bits == X86EMU_MEMIO_32 ? 4 : 1;
    if (kind == X86EMU_MEMIO_I || kind == X86EMU_MEMIO_O) {
        ops->unexpected("port I/O executed by the emulator");
        if (kind == X86EMU_MEMIO_I) *val = 0xFFFFFFFFu;
        return 0;
    }
    if (addr >= GDT_FAKE && addr < GDT_FAKE + 0x10000) {
        uint32_t d[2] = {0, 0};
        unsigned off = addr - GDT_FAKE;
        ops->descriptor((uint16_t)(off & ~7u), false, d);
        if (kind == X86EMU_MEMIO_W) return 0;
        tcg_descriptor(d);
        uint32_t x = 0;
        memcpy(&x, (uint8_t *)d + (off & 7), n > 8 - (off & 7) ? 8 - (off & 7) : n);
        *val = x;
        return 0;
    }
    if (addr >= LDT_FAKE && addr - LDT_FAKE < 0x10000) {
        uint32_t d[2] = {0, 0};
        unsigned off = addr - LDT_FAKE;
        ops->descriptor((uint16_t)((off & ~7u) | 4), kind == X86EMU_MEMIO_W, d);
        if (kind == X86EMU_MEMIO_W) return 0;   /* accessed-bit updates are not modelled */
        tcg_descriptor(d);
        uint32_t x = 0;
        memcpy(&x, (uint8_t *)d + (off & 7), n > 8 - (off & 7) ? 8 - (off & 7) : n);
        *val = x;
        return 0;
    }
    uint32_t x = kind == X86EMU_MEMIO_W ? *val : 0;
    bool ok = kind == X86EMU_MEMIO_W ? ops->write(addr, &x, n) : ops->read(addr, &x, n);
    if (ok && kind != X86EMU_MEMIO_X && ops->watch) watch_hits |= ops->watch(addr, n, kind == X86EMU_MEMIO_W);
    if (!ok && !fault_pending) {
        fault_pending = true;
        event->vector = 14;
        event->cr2 = addr;
        event->err = (kind == X86EMU_MEMIO_W ? 2u : 0u) | 4u;
        event->fault = true;
        x86emu_stop(emu);
    }
    if (kind != X86EMU_MEMIO_W) *val = ok ? x : 0xFFFFFFFFu;
    return 0;
}

/* SDM error codes for #GP: selector & ~3 only for faults while loading a
 * selector; 0 for limit and other faults (libx86emu reports the selector of
 * the segment for limit violations too). */
static bool selector_load(uint32_t lin)
{
    uint8_t op = 0, op2 = 0, modrm = 0;
    unsigned n = 0;
    for (; n < 14; n++) {
        if (!fetch(lin + n, &op)) return false;
        if (op == 0x66 || op == 0x67 || op == 0xF0 || op == 0xF2 || op == 0xF3 || op == 0x26 ||
            op == 0x2E || op == 0x36 || op == 0x3E || op == 0x64 || op == 0x65) continue;
        break;
    }
    switch (op) {
    case 0x8E: case 0x07: case 0x17: case 0x1F: case 0xC4: case 0xC5:
    case 0xEA: case 0x9A: case 0xCA: case 0xCB: case 0xCF:
        return true;
    case 0x0F:
        return fetch(lin + n + 1, &op2) && (op2 == 0xA1 || op2 == 0xA9 || op2 == 0xB2 || op2 == 0xB4 || op2 == 0xB5);
    case 0xFF:
        return fetch(lin + n + 1, &modrm) && (((modrm >> 3) & 7) == 3 || ((modrm >> 3) & 7) == 5);
    default:
        return false;
    }
}
static int intr(x86emu_t *e, u8 num, unsigned type)
{
    if (!fault_pending) {
        fault_pending = true;
        event->vector = num;
        event->err = (type & INTR_MODE_ERRCODE) ? e->x86.intr_errcode : 0;
        if (num == 13) event->err = selector_load(e->x86.R_CS_BASE + e->x86.saved_eip) ? event->err & 0xFFFCu : 0;
        event->fault = true;
        if (!(type & INTR_MODE_RESTART) && (type & 0xFF) == INTR_TYPE_SOFT && num) {
            event->fault = false;           /* executed INT n: should never reach here */
            ops->unexpected("software interrupt executed by the emulator");
        }
    }
    x86emu_stop(e);
    return 1;
}

static bool fetch(uint32_t lin, uint8_t *b) { return ops->read(lin, b, 1); }

static uint32_t *reg32(unsigned i)
{
    switch (i & 7) {
    case 0: return &emu->x86.R_EAX; case 1: return &emu->x86.R_ECX;
    case 2: return &emu->x86.R_EDX; case 3: return &emu->x86.R_EBX;
    case 4: return &emu->x86.R_ESP; case 5: return &emu->x86.R_EBP;
    case 6: return &emu->x86.R_ESI; default: return &emu->x86.R_EDI;
    }
}
/* Intel SDM LAR/LSL/VERR/VERW at CPL 3 (privilege: DPL >= max(CPL, RPL)
 * except conforming code). Returns false when the descriptor fails. */
static bool visible(uint16_t sel, uint32_t d[2])
{
    if (!(sel & ~3u)) return false;
    ops->descriptor(sel, false, d);
    uint8_t a = (uint8_t)(d[1] >> 8);
    if (!(a & 0x10)) return false;           /* no system descriptors in this VM */
    unsigned dpl = (a >> 5) & 3, rpl = sel & 3;
    bool conforming = (a & 0x1C) == 0x1C;
    return conforming || dpl >= (rpl > 3 ? rpl : 3);
}
static void set_zf(bool on) { if (on) emu->x86.R_EFLG |= 0x40; else emu->x86.R_EFLG &= ~0x40u; }

/* Emulate one descriptor instruction at the current EIP; returns length or 0. */
static unsigned descriptor_insn(uint32_t lin, unsigned prefix_len, bool op32)
{
    uint8_t op2, modrm;
    if (!fetch(lin + prefix_len + 1, &op2) || !fetch(lin + prefix_len + 2, &modrm)) return 0;
    if ((modrm >> 6) != 3) return 0;          /* memory forms are not used by the payloads */
    unsigned reg = (modrm >> 3) & 7, rm = modrm & 7;
    uint16_t sel = (uint16_t)*reg32(rm);
    uint32_t d[2];
    bool ok = visible(sel, d);
    uint8_t a = (uint8_t)(d[1] >> 8);
    if (op2 == 0x00) {                        /* VERR/VERW */
        if (reg == 4) set_zf(ok && (!(a & 8) || (a & 2)));
        else if (reg == 5) set_zf(ok && !(a & 8) && (a & 2));
        else return 0;
        return prefix_len + 3;
    }
    set_zf(ok);
    if (!ok) return prefix_len + 3;
    uint32_t value;
    if (op2 == 0x03) {
        uint32_t raw = (d[0] & 0xFFFFu) | (d[1] & 0xF0000u);
        value = (d[1] & (1u << 23)) ? (raw << 12) | 0xFFFu : raw;
    } else value = d[1] & 0x00F0FF00u;
    uint32_t *dst = reg32(reg);
    if (op32) *dst = value;
    else *dst = (*dst & 0xFFFF0000u) | (uint16_t)value;
    return prefix_len + 3;
}

/* Effective address of a ModRM memory operand (16- or 32-bit addressing),
 * segment base included; returns the length of ModRM+SIB+displacement. */
static unsigned modrm_ea(uint32_t lin, bool a32, int seg_override, uint32_t *ea)
{
    uint8_t modrm, sib = 0, b;
    if (!fetch(lin, &modrm)) return 0;
    unsigned mod = modrm >> 6, rm = modrm & 7, len = 1;
    uint32_t off = 0;
    sel_t *seg = emu->x86.R_DS_SEL;
    if (!a32) {
        static const int base[8] = {3, 3, 5, 5, 6, 7, 5, 3}, index[8] = {6, 7, 6, 7, -1, -1, -1, -1};
        if (mod == 0 && rm == 6) { uint8_t d[2]; fetch(lin + 1, &d[0]); fetch(lin + 2, &d[1]); off = d[0] | (d[1] << 8); len = 3; }
        else {
            off = (uint16_t)*reg32((unsigned)base[rm]);
            if (index[rm] >= 0) off += (uint16_t)*reg32((unsigned)index[rm]);
            if (rm == 2 || rm == 3 || rm == 6) seg = emu->x86.R_SS_SEL;
            if (mod == 1) { fetch(lin + 1, &b); off += (uint32_t)(int8_t)b; len = 2; }
            if (mod == 2) { uint8_t d[2]; fetch(lin + 1, &d[0]); fetch(lin + 2, &d[1]); off += d[0] | (d[1] << 8); len = 3; }
        }
        off &= 0xFFFF;
    } else {
        if (rm == 4) { fetch(lin + 1, &sib); len = 2; }
        unsigned bi = rm == 4 ? (sib & 7) : rm;
        if (mod == 0 && bi == 5) { off = 0; uint8_t d[4]; for (unsigned i = 0; i < 4; i++) fetch(lin + len + i, &d[i]); off = d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24; len += 4; }
        else { off = *reg32(bi); if (bi == 4 || bi == 5) seg = emu->x86.R_SS_SEL; }
        if (rm == 4 && ((sib >> 3) & 7) != 4) off += *reg32((sib >> 3) & 7) << (sib >> 6);
        if (mod == 1) { fetch(lin + len, &b); off += (uint32_t)(int8_t)b; len += 1; }
        if (mod == 2) { uint8_t d[4]; for (unsigned i = 0; i < 4; i++) fetch(lin + len + i, &d[i]); off += d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24; len += 4; }
    }
    if (seg_override >= 0) seg = &emu->x86.seg[seg_override];
    *ea = seg->base + off;
    return len;
}
/* Minimal x87 control/status unit for FPU detection (libx86emu has none). */
static uint16_t fpu_cw = 0x037F, fpu_sw;
static int fpu_insn(uint32_t lin, unsigned n, bool a32, int seg_override, uint8_t op)
{
    uint8_t m;
    if (op == 0x9B) return (int)n + 1;                                   /* FWAIT */
    if (!fetch(lin + n + 1, &m)) return 0;
    if (op == 0xDB && m == 0xE3) { fpu_cw = 0x037F; fpu_sw = 0; return (int)n + 2; } /* FNINIT */
    if (op == 0xDB && m == 0xE2) { fpu_sw &= 0x7F00; return (int)n + 2; }          /* FNCLEX */
    if (op == 0xDF && m == 0xE0) { emu->x86.R_AX = fpu_sw; return (int)n + 2; }     /* FNSTSW AX */
    unsigned reg = (m >> 3) & 7;
    if ((m >> 6) == 3) return 0;
    uint32_t ea;
    unsigned len = modrm_ea(lin + n + 1, a32, seg_override, &ea);
    if (!len) return 0;
    if (op == 0xD9 && reg == 7) { if (!ops->write(ea, &fpu_cw, 2)) return 0; }      /* FNSTCW m16 */
    else if (op == 0xD9 && reg == 5) { if (!ops->read(ea, &fpu_cw, 2)) return 0; }  /* FLDCW m16 */
    else if (op == 0xDD && reg == 7) { if (!ops->write(ea, &fpu_sw, 2)) return 0; }  /* FNSTSW m16 */
    else return 0;
    return (int)(n + 1 + len);
}

/* Decide whether the next instruction traps before executing. */
/* ENTER imm16,0 (SDM Vol. 2A): the frame-pointer push has the operand
 * size; SS.B only selects SP or ESP. libx86emu sizes the push by the stack
 * size, which breaks 16-bit code on a 32-bit stack (the DOS/4GW kernel).
 * Returns the length when emulated here, 0 to let libx86emu run it. */
static int enter_insn(uint32_t lin, unsigned n, bool op32, bool s32)
{
    uint8_t b[3];
    for (unsigned i = 0; i < 3; i++) if (!fetch(lin + n + 1 + i, &b[i])) return 0;
    if (op32 == s32) return 0;
    if (b[2] & 31) { ops->unexpected("ENTER with nesting and mixed operand/stack size"); return 0; }
    unsigned w = op32 ? 4 : 2;
    uint32_t esp = (s32 ? emu->x86.R_ESP : emu->x86.R_SP) - w;
    if (!s32) esp &= 0xFFFF;
    uint32_t bp = op32 ? emu->x86.R_EBP : emu->x86.R_BP;
    if (!ops->write(emu->x86.R_SS_BASE + esp, &bp, w)) return 0;
    if (op32) emu->x86.R_EBP = esp; else emu->x86.R_BP = (uint16_t)esp;
    esp -= (uint32_t)(b[0] | b[1] << 8);
    if (s32) emu->x86.R_ESP = esp; else emu->x86.R_SP = (uint16_t)esp;
    return (int)n + 4;
}

static bool trap_check(struct gcpu_state *s)
{
    uint32_t lin = emu->x86.R_CS_BASE + emu->x86.R_EIP;
    bool big = s->pm && (emu->x86.R_CS_ACC & (1u << 10));
    bool op32 = big, a32 = big;
    int seg_override = -1;
    unsigned n = 0;
    uint8_t op = 0;
    for (; n < 14; n++) {
        if (!fetch(lin + n, &op)) return false;
        if (op == 0x66) { op32 = !big; continue; }
        if (op == 0x67) { a32 = !big; continue; }
        if (op == 0x26) { seg_override = R_ES_INDEX; continue; }
        if (op == 0x2E) { seg_override = R_CS_INDEX; continue; }
        if (op == 0x36) { seg_override = R_SS_INDEX; continue; }
        if (op == 0x3E) { seg_override = R_DS_INDEX; continue; }
        if (op == 0x64) { seg_override = R_FS_INDEX; continue; }
        if (op == 0x65) { seg_override = R_GS_INDEX; continue; }
        if (op == 0xF0 || op == 0xF2 || op == 0xF3) continue;
        break;
    }
    if (op == 0x9B || op == 0xD9 || op == 0xDB || op == 0xDD || op == 0xDF) {
        int len = fpu_insn(lin, n, a32, seg_override, op);
        if (len > 0) { emu->x86.R_EIP += (uint32_t)len; event->vector = 0; event->executed_descriptor = true; return false; }
        if (op != 0x9B) ops->unexpected("x87 instruction outside the runner's control/status subset");
    }
    if (op == 0x0F && tcg()) {                      /* CPUID 40000000h under GUEST_TCG */
        uint8_t next = 0;
        if (fetch(lin + n + 1, &next) && next == 0xA2 && emu->x86.R_EAX == 0x40000000u) {
            emu->x86.R_EAX = 0x40000001u; emu->x86.R_EBX = 0x54474354u;
            emu->x86.R_ECX = 0x43544743u; emu->x86.R_EDX = 0x47435447u;
            emu->x86.R_EIP += n + 2; event->vector = 0; event->executed_descriptor = true;
            return false;
        }
    }
    if (op == 0xC8) {
        int len = enter_insn(lin, n, op32, s->pm && (emu->x86.R_SS_ACC & (1u << 10)));
        if (len > 0) { emu->x86.R_EIP += (uint32_t)len; event->vector = 0; event->executed_descriptor = true; return false; }
    }
    if (!s->pm) {
        switch (op) {
        case 0xFA: case 0xFB: case 0x9C: case 0x9D: case 0xCD: case 0xCC: case 0xCE: case 0xCF:
        case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xEC: case 0xED: case 0xEE: case 0xEF:
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0xF4:
            event->vector = 13; event->err = 0; event->fault = true;
            return true;
        default: return false;
        }
    }
    uint8_t imm = 0, op2 = 0, modrm = 0;
    switch (op) {
    case 0xCD:
        if (!fetch(lin + n + 1, &imm)) return false;
        if (imm == 3 || imm == 0x80) {
            emu->x86.R_EIP += n + 2;
            event->vector = imm; event->err = 0; event->fault = false;
        } else {
            event->vector = 13; event->err = (uint32_t)imm << 3 | 2; event->fault = true;
        }
        return true;
    case 0xCC:
        emu->x86.R_EIP += n + 1;
        event->vector = 3; event->err = 0; event->fault = false;
        return true;
    case 0xF1:
        emu->x86.R_EIP += n + 1;
        event->vector = 1; event->err = 0; event->fault = false;
        return true;
    case 0xCE:
        if (!(emu->x86.R_EFLG & 0x800)) return false;
        event->vector = 13; event->err = 4u << 3 | 2; event->fault = true;
        return true;
    case 0xFA: case 0xFB: case 0xF4:
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xEC: case 0xED: case 0xEE: case 0xEF:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        event->vector = 13; event->err = 0; event->fault = true;
        return true;
    case 0x0F:
        if (!fetch(lin + n + 1, &op2)) return false;
        if (op2 == 0x00 || op2 == 0x01) { if (!fetch(lin + n + 2, &modrm)) return false; }
        if ((op2 == 0x00 && ((modrm >> 3) & 7) >= 4) || op2 == 0x02 || op2 == 0x03) {
            unsigned len = descriptor_insn(lin, n, op32);
            if (!len) { event->vector = 6; event->err = 0; event->fault = true; return true; }
            emu->x86.R_EIP += len;
            event->vector = 0; event->executed_descriptor = true;
            return false;
        }
        if ((op2 == 0x00 && (((modrm >> 3) & 7) == 2 || ((modrm >> 3) & 7) == 3)) ||
            (op2 == 0x01 && (((modrm >> 3) & 7) == 2 || ((modrm >> 3) & 7) == 3 || ((modrm >> 3) & 7) >= 6)) ||
            op2 == 0x06 || op2 == 0x08 || op2 == 0x09 || (op2 >= 0x20 && op2 <= 0x23) || op2 == 0x30 || op2 == 0x32) {
            event->vector = 13; event->err = 0; event->fault = true;
            return true;
        }
        return false;
    default:
        return false;
    }
}

static void load_seg(sel_t *seg, uint16_t sel, bool pm, bool code)
{
    if (!pm) {
        seg->sel = sel; seg->base = (uint32_t)sel << 4; seg->limit = 0xFFFF;
        seg->acc = code ? 0x9B : 0x93;
        return;
    }
    x86emu_set_seg_register(emu, seg, sel);
}

void gcpu_init(const struct gcpu_ops *o)
{
    ops = o;
    emu = x86emu_new(X86EMU_PERM_RWX, X86EMU_PERM_RW);
    x86emu_set_memio_handler(emu, memio);
    x86emu_set_intr_handler(emu, intr);
}

void gcpu_done(void) { x86emu_done(emu); emu = 0; }

int gcpu_run(struct gcpu_state *s, unsigned max, struct gcpu_event *ev)
{
    memset(ev, 0, sizeof(*ev));
    event = ev;
    fault_pending = false;
    emu->x86.R_CR0 = s->pm ? 0x80000011u : 0x10u;
    emu->x86.R_GDT_BASE = GDT_FAKE; emu->x86.R_GDT_LIMIT = 0x47;
    emu->x86.R_LDT_BASE = LDT_FAKE; emu->x86.R_LDT_LIMIT = s->ldt_limit;
    emu->x86.R_EAX = s->eax; emu->x86.R_EBX = s->ebx; emu->x86.R_ECX = s->ecx; emu->x86.R_EDX = s->edx;
    emu->x86.R_ESI = s->esi; emu->x86.R_EDI = s->edi; emu->x86.R_EBP = s->ebp; emu->x86.R_ESP = s->esp;
    emu->x86.R_EIP = s->eip;
    emu->x86.R_EFLG = (s->eflags & 0x00000DD5u) | 0x202u;
    load_seg(emu->x86.R_CS_SEL, s->cs, s->pm, true);
    load_seg(emu->x86.R_SS_SEL, s->ss, s->pm, false);
    load_seg(emu->x86.R_DS_SEL, s->ds, s->pm, false);
    load_seg(emu->x86.R_ES_SEL, s->es, s->pm, false);
    load_seg(emu->x86.R_FS_SEL, s->fs, s->pm, false);
    load_seg(emu->x86.R_GS_SEL, s->gs, s->pm, false);
    if (emu->x86.intr_type) { emu->x86.intr_type = 0; ops->unexpected("invalid selector in a loaded frame"); }
    bool tf = s->eflags & 0x100;
    unsigned executed = 0, budget = tf ? 1 : max;
    int rc = GCPU_BUDGET;
    while (executed < budget) {
        /* TF loaded inside the slice (POPF/IRET of an image with TF, which
         * libx86emu loads but never acts on): the trap follows the first
         * instruction that starts with TF set, not the one that set it
         * (Intel SDM Vol. 3, "Single-Step Exception Condition"). */
        bool tf_loaded = !tf && (emu->x86.R_EFLG & 0x100u);
        if (tf_loaded && budget > executed + 1) budget = executed + 1;
        ev->executed_descriptor = false;
        if (trap_check(s)) { rc = ev->fault && ev->vector == 13 ? GCPU_TRAP : GCPU_TRAP; break; }
        if (ev->executed_descriptor) { executed++; continue; }
        uint8_t first = 0, second = 0;
        fetch(emu->x86.R_CS_BASE + emu->x86.R_EIP, &first);
        fetch(emu->x86.R_CS_BASE + emu->x86.R_EIP + 1, &second);
        uint32_t saved_eip = emu->x86.R_EIP;
        watch_hits = 0;
        emu->max_instr = emu->x86.R_TSC + 1;
        emu->x86.mode &= ~0x80u;               /* clear a previous stop */
        x86emu_run(emu, X86EMU_RUN_MAX_INSTR);
        if (fault_pending) {
            emu->x86.R_EIP = saved_eip;        /* faults restart the instruction */
            rc = GCPU_FAULT;
            break;
        }
        executed++;
        ev->ss_shadow = first == 0x17 || (first == 0x8E && ((second >> 3) & 7) == 2);
        if (watch_hits) {                  /* data breakpoint: #DB trap after the access */
            ev->vector = 1; ev->err = 0; ev->fault = false; ev->dr6 = watch_hits | ((tf || tf_loaded) ? 0x4000u : 0);
            rc = GCPU_TRAP;
            break;
        }
        /* MOV SS / POP SS delay a single-step trap by one instruction. */
        if ((tf || tf_loaded) && executed == budget && (first == 0x17 || (first == 0x8E && ((second >> 3) & 7) == 2)))
            budget++;
        if (tf_loaded && executed == budget) { tf = true; break; }
    }
    ev->executed = executed;
    if (rc == GCPU_BUDGET && tf && executed) { ev->vector = 1; ev->err = 0; ev->fault = false; ev->dr6 = 0x4000; rc = GCPU_TRAP; }
    s->eax = emu->x86.R_EAX; s->ebx = emu->x86.R_EBX; s->ecx = emu->x86.R_ECX; s->edx = emu->x86.R_EDX;
    s->esi = emu->x86.R_ESI; s->edi = emu->x86.R_EDI; s->ebp = emu->x86.R_EBP; s->esp = emu->x86.R_ESP;
    s->eip = emu->x86.R_EIP;
    s->eflags = (emu->x86.R_EFLG & 0x00000DD5u) | 0x202u;
    s->cs = emu->x86.R_CS; s->ss = emu->x86.R_SS; s->ds = emu->x86.R_DS; s->es = emu->x86.R_ES;
    s->fs = emu->x86.R_FS; s->gs = emu->x86.R_GS;
    return rc;
}
