/* DPMI 0.9 LDT management (INT 31h 0000h-000Dh), selector translation and
 * the return-frame validator. Intel SDM Vol. 3A 3.4.5 (descriptor format),
 * 5.5-5.8 (privilege checks on segment loads and IRET); DPMI 0.9 section 8.
 * Research and decisions: docs/design/f3-acceptance.md, f3-04.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "dosvm_internal.h"

#define GDT_UCODE 0x18u   /* flat DPL3 code (SEL_UCODE) */
#define GDT_UDATA 0x20u   /* flat DPL3 data (SEL_UDATA) */

void dpmi_desc_set(uint32_t d[2], uint32_t base, uint32_t limit, uint8_t access, uint8_t ext)
{
    uint32_t raw = limit, g = 0;
    if (limit > 0xFFFFFu) { raw = limit >> 12; g = 0x80; }
    d[0] = (raw & 0xFFFFu) | (base << 16);
    d[1] = ((base >> 16) & 0xFFu) | ((uint32_t)access << 8) | (raw & 0xF0000u) |
           ((uint32_t)((ext & 0x50u) | g) << 16) | (base & 0xFF000000u);
}
uint32_t dpmi_desc_base(const uint32_t d[2])
{ return (d[0] >> 16) | ((d[1] & 0xFFu) << 16) | (d[1] & 0xFF000000u); }
uint32_t dpmi_desc_limit(const uint32_t d[2])
{
    uint32_t raw = (d[0] & 0xFFFFu) | (d[1] & 0xF0000u);
    return (d[1] & (1u << 23)) ? (raw << 12) | 0xFFFu : raw;
}
uint32_t dpmi_off(const struct dosvm *v, uint32_t reg) { return v->dpmi.bits32 ? reg : (uint16_t)reg; }

static bool decode(const uint32_t d[2], struct dpmi_seg *s)
{
    uint8_t a = (uint8_t)(d[1] >> 8);
    if (!(a & 0x10)) return false;          /* system descriptors are never client segments */
    s->valid = true;
    s->access = a;
    s->present = a & 0x80;
    s->dpl = (a >> 5) & 3;
    s->code = a & 8;
    s->conforming = s->code && (a & 4);
    s->readable = !s->code || (a & 2);
    s->writable = !s->code && (a & 2);
    s->down = !s->code && (a & 4);
    s->big = d[1] & (1u << 22);
    s->base = dpmi_desc_base(d);
    s->limit = dpmi_desc_limit(d);
    return true;
}

bool dpmi_seg(const struct dosvm *v, uint16_t sel, struct dpmi_seg *s)
{
    memset(s, 0, sizeof(*s));
    if (!(sel & ~3u)) return false;
    if (sel & 4) {
        unsigned i = sel >> 3;
        if (!v->dpmi.active || i >= DPMI_LDT_ENTRIES || v->dpmi.kind[i] == DPMI_FREE) return false;
        return decode(v->dpmi.ldt[i], s);
    }
    uint32_t flat[2];
    if ((sel & ~3u) == GDT_UCODE) dpmi_desc_set(flat, 0, 0xFFFFFFFFu, 0xFA, 0x40);
    else if ((sel & ~3u) == GDT_UDATA) dpmi_desc_set(flat, 0, 0xFFFFFFFFu, 0xF2, 0x40);
    else return false;
    return decode(flat, s);
}

/* Checked selector:offset range; the linear result is also VM-owned memory.
 * Flat descriptors may span 4 GiB; page ownership stays the boundary. */
bool dpmi_ptr(struct dosvm *v, uint16_t sel, uint32_t off, uint32_t bytes, bool write, uint32_t *linear)
{
    struct dpmi_seg s;
    if (!bytes || !dpmi_seg(v, sel, &s) || !s.present) return false;
    if (write ? !s.writable : !s.readable) return false;
    uint32_t last = off + bytes - 1;
    if (last < off) return false;
    if (s.down) {
        if (off <= s.limit || last > (s.big ? 0xFFFFFFFFu : 0xFFFFu)) return false;
    } else if (last > s.limit) return false;
    uint32_t lin = s.base + off;
    if (lin + bytes - 1 < lin || !dosvm_range(v, lin, bytes, write)) return false;
    if (linear) *linear = lin;
    return true;
}
bool dpmi_client_ptr(struct dosvm *v, struct v86_frame *f, uint16_t sel, uint32_t off, uint32_t bytes, bool write, uint32_t *linear)
{ (void)f; return dpmi_ptr(v, sel, dpmi_off(v, off), bytes, write, linear); }

int dpmi_alloc_ldt(struct dosvm *v, unsigned count, uint8_t kind, uint8_t owner)
{
    struct dpmi *d = &v->dpmi;
    unsigned run = 0;
    if (!count) return -1;
    for (unsigned i = DPMI_SPECIFIC; i <= DPMI_IDX_LAST_CLIENT; i++) {
        run = d->kind[i] == DPMI_FREE ? run + 1 : 0;
        if (run < count) continue;
        unsigned first = i + 1 - count;
        for (unsigned j = first; j <= i; j++) {
            d->kind[j] = kind; d->owner[j] = owner;
            /* 0.9 section 8.1: present data, base and limit zero, client DPL. */
            dpmi_desc_set(d->ldt[j], 0, 0, 0xF2, d->bits32 ? 0x40 : 0);
        }
        return (int)first;
    }
    return -1;
}
void dpmi_free_ldt(struct dosvm *v, unsigned i)
{
    if (i >= DPMI_LDT_ENTRIES) return;
    v->dpmi.kind[i] = DPMI_FREE; v->dpmi.owner[i] = 0;
    v->dpmi.ldt[i][0] = v->dpmi.ldt[i][1] = 0;
}
int dpmi_seg_data(struct dosvm *v, uint8_t kind, uint8_t owner, uint32_t base, uint32_t limit, bool big)
{
    int i = dpmi_alloc_ldt(v, 1, kind, owner);
    if (i >= 0) dpmi_desc_set(v->dpmi.ldt[i], base, limit, 0xF2, big ? 0x40 : 0);
    return i;
}

bool dpmi_code_selector(const struct dosvm *v, uint16_t sel, uint32_t off)
{
    struct dpmi_seg s;
    if (sel == DPMI_HOST_CS) return off <= DPMI_PM_LIMIT;
    return (sel & 3) == 3 && dpmi_seg(v, sel, &s) && s.present && s.code && s.dpl == 3 && off <= s.limit;
}

/* Would a ring-0 "pop sreg" in trap_return accept this selector? A null
 * selector always loads; others must be present data or readable code with
 * RPL <= DPL (SDM Vol. 3A 5.7). Otherwise the frame value is zeroed, as a
 * DPMI 1.0 host does for freed or invalidated selectors. */
static bool loadable(const struct dosvm *v, uint16_t sel)
{
    struct dpmi_seg s;
    if (!(sel & ~3u)) return true;
    if (!dpmi_seg(v, sel, &s) || !s.present || (s.code && !s.readable)) return false;
    return s.conforming || (unsigned)(sel & 3) <= s.dpl;
}
void dpmi_zero_segments(struct dosvm *v, struct v86_frame *f, uint16_t sel)
{
    (void)v;
    uint32_t *seg[4] = {&f->tf.ds, &f->tf.es, &f->tf.fs, &f->tf.gs};
    for (unsigned i = 0; i < 4; i++)
        if (((uint16_t)*seg[i] & ~3u) == (sel & ~3u) && (sel & ~3u)) *seg[i] = 0;
}

/* Every frame returned to a client passes here: CS/SS/EIP must make the
 * kernel IRET succeed (otherwise the caller terminates the VM), data
 * selectors are zeroed when unusable, EFLAGS keeps IF=1/IOPL=0/VM=0. */
bool dpmi_frame_check(struct dosvm *v, struct v86_frame *f)
{
    uint32_t *seg[4] = {&f->tf.ds, &f->tf.es, &f->tf.fs, &f->tf.gs};
    for (unsigned i = 0; i < 4; i++) {
        uint16_t sel = (uint16_t)*seg[i];
        *seg[i] = loadable(v, sel) ? sel : 0;
    }
    struct dpmi_seg cs, ss;
    uint16_t c = (uint16_t)f->tf.cs, s = (uint16_t)f->tf.user_ss;
    if ((c & 3) != 3 || !dpmi_seg(v, c, &cs) || !cs.present || !cs.code || cs.conforming || cs.dpl != 3 ||
        f->tf.eip > cs.limit)
        return false;
    if ((s & 3) != 3 || !dpmi_seg(v, s, &ss) || !ss.present || !ss.writable || ss.dpl != 3)
        return false;
    f->tf.cs = c;
    f->tf.user_ss = s;
    f->tf.eflags = (f->tf.eflags & 0x00250DD5u) | V86_IF | 2u;
    return true;
}

/* 0.9 section 8.9 rules for CL/CH (and bytes 5/6 of 000Ch). */
static bool rights_ok(uint8_t a, uint8_t ext)
{
    if (!(a & 0x10) || ((a >> 5) & 3) != 3 || (ext & 0x20)) return false;
    if (!(a & 0x80)) return true;
    return !(a & 8) || ((a & 2) && !(a & 4));
}
/* Index of a descriptor the current client may modify, or -1. */
static int mine(const struct dosvm *v, uint16_t sel)
{
    unsigned i = sel >> 3;
    if (!(sel & 4) || i >= DPMI_LDT_ENTRIES) return -1;
    if (v->dpmi.kind[i] != DPMI_CLIENT || v->dpmi.owner[i] != dpmi_id(v)) return -1;
    return (int)i;
}
static int ldt_allocated(const struct dosvm *v, uint16_t sel)
{
    unsigned i = sel >> 3;
    if (!(sel & 4) || i >= DPMI_LDT_ENTRIES || v->dpmi.kind[i] == DPMI_FREE) return -1;
    return (int)i;
}

/* Returns 0 success, 1 failure (CF, AX unchanged), <0 VM fault. */
int dpmi_ldt_service(struct dosvm *v, struct v86_frame *f, unsigned ax)
{
    struct dpmi *d = &v->dpmi;
    uint16_t bx = (uint16_t)f->tf.ebx;
    int i;
    switch (ax) {
    case 0x0000: {
        i = dpmi_alloc_ldt(v, (uint16_t)f->tf.ecx, DPMI_CLIENT, (uint8_t)dpmi_id(v));
        if (i < 0) return 1;
        dpmi_low(&f->tf.eax, DPMI_SEL(i));
        return 0;
    }
    case 0x0001:
        if ((i = mine(v, bx)) < 0) return 1;
        dpmi_free_ldt(v, (unsigned)i);
        dpmi_zero_segments(v, f, bx);
        return 0;
    case 0x0002: {
        uint32_t base = (uint32_t)bx << 4;
        for (unsigned j = DPMI_SPECIFIC; j < DPMI_LDT_ENTRIES; j++)
            if (d->kind[j] == DPMI_ALIAS && dpmi_desc_base(d->ldt[j]) == base) {
                dpmi_low(&f->tf.eax, DPMI_SEL(j)); return 0;
            }
        if ((i = dpmi_seg_data(v, DPMI_ALIAS, 0, base, 0xFFFF, false)) < 0) return 1;
        dpmi_low(&f->tf.eax, DPMI_SEL(i));
        return 0;
    }
    case 0x0003: dpmi_low(&f->tf.eax, 8); return 0;
    case 0x0006:
        if ((i = ldt_allocated(v, bx)) < 0) return 1;
        dpmi_low(&f->tf.ecx, dpmi_desc_base(d->ldt[i]) >> 16);
        dpmi_low(&f->tf.edx, dpmi_desc_base(d->ldt[i]));
        return 0;
    case 0x0007: {
        if ((i = mine(v, bx)) < 0) return 1;
        uint32_t base = ((uint32_t)(uint16_t)f->tf.ecx << 16) | (uint16_t)f->tf.edx;
        uint32_t *e = d->ldt[i];
        e[0] = (e[0] & 0xFFFFu) | (base << 16);
        e[1] = (e[1] & 0x00FFFF00u) | ((base >> 16) & 0xFFu) | (base & 0xFF000000u);
        return 0;
    }
    case 0x0008: {
        if ((i = mine(v, bx)) < 0) return 1;
        uint32_t limit = ((uint32_t)(uint16_t)f->tf.ecx << 16) | (uint16_t)f->tf.edx;
        if (limit > 0xFFFFFu && (limit & 0xFFFu) != 0xFFFu) return 1;
        uint32_t *e = d->ldt[i];
        uint32_t base = dpmi_desc_base(e);
        uint8_t access = (uint8_t)(e[1] >> 8), ext = (uint8_t)(e[1] >> 16) & 0x50;
        dpmi_desc_set(e, base, limit, access, ext);
        return 0;
    }
    case 0x0009: {
        if ((i = mine(v, bx)) < 0) return 1;
        uint8_t cl = (uint8_t)f->tf.ecx, ch = (uint8_t)(f->tf.ecx >> 8);
        if (!rights_ok(cl, ch)) return 1;
        uint32_t *e = d->ldt[i];
        e[1] = (e[1] & 0xFF0F00FFu) | ((uint32_t)cl << 8) | ((uint32_t)(ch & 0xD0u) << 16);
        return 0;
    }
    case 0x000A: {
        struct dpmi_seg s;
        if (ldt_allocated(v, bx) < 0 || !dpmi_seg(v, bx, &s) || !s.code) return 1;
        uint32_t *src = d->ldt[bx >> 3];
        if ((i = dpmi_alloc_ldt(v, 1, DPMI_CLIENT, (uint8_t)dpmi_id(v))) < 0) return 1;
        d->ldt[i][0] = src[0];
        d->ldt[i][1] = (src[1] & 0xFFFF00FFu) | ((uint32_t)((s.access & 0xE0u) | 0x12u) << 8);
        dpmi_low(&f->tf.eax, DPMI_SEL(i));
        return 0;
    }
    case 0x000B: {
        uint32_t lin;
        if ((i = ldt_allocated(v, bx)) < 0) return 1;
        if (!dpmi_client_ptr(v, f, (uint16_t)f->tf.es, f->tf.edi, 8, true, &lin)) return 1;
        return dosvm_write(v, lin, d->ldt[i], 8) ? 1 : 0;
    }
    case 0x000C: {
        uint32_t lin, e[2];
        if ((i = mine(v, bx)) < 0) return 1;
        if (!dpmi_client_ptr(v, f, (uint16_t)f->tf.es, f->tf.edi, 8, false, &lin) || dosvm_read(v, lin, e, 8))
            return 1;
        if (!rights_ok((uint8_t)(e[1] >> 8), (uint8_t)(e[1] >> 16) & 0xF0u)) return 1;
        d->ldt[i][0] = e[0];
        d->ldt[i][1] = e[1] & ~(1u << 21);  /* L bit is reserved (must be 0) */
        return 0;
    }
    case 0x000D: {
        unsigned j = bx >> 3;
        if (!(bx & 4) || j >= DPMI_SPECIFIC || d->kind[j] != DPMI_FREE) return 1;
        d->kind[j] = DPMI_CLIENT; d->owner[j] = (uint8_t)dpmi_id(v);
        dpmi_desc_set(d->ldt[j], 0, 0, 0xF2, d->bits32 ? 0x40 : 0);
        return 0;
    }
    default:
        return 1;                       /* 0004h/0005h reserved; 000Eh/000Fh are DPMI 1.0 */
    }
}
