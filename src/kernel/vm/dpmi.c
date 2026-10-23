/* Kernel-owned DPMI 0.9 host for DOS VMs (directive f3-04): discovery and
 * entry (INT 2Fh/1686h/1687h), client lifetime and cleanup, the complete
 * INT 31h table of dos-dpmi-contract.md with its refusals, and the V86
 * monitor hooks. DPMI 0.9 (July 1990) sections 4-19; DJGPP DPMI index for
 * numbering; DPMI 1.0 notes on 0.9 version numbers (DL/AL = 90 decimal).
 * Research and decisions: docs/design/f3-acceptance.md, f3-04.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "dosvm_internal.h"

/* INT 31h row index: the 0.9 table plus the DPMI 1.0-only functions the
 * contract lists as unsupported, in numeric order (77 rows). */
static const uint16_t rows[] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, 0x000A, 0x000B,
    0x000C, 0x000D, 0x000E, 0x000F, 0x0100, 0x0101, 0x0102, 0x0200, 0x0201, 0x0202, 0x0203, 0x0204,
    0x0205, 0x0210, 0x0211, 0x0212, 0x0213, 0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305, 0x0306,
    0x0400, 0x0401, 0x0500, 0x0501, 0x0502, 0x0503, 0x0504, 0x0505, 0x0506, 0x0507, 0x0508, 0x0509,
    0x050A, 0x050B, 0x0600, 0x0601, 0x0602, 0x0603, 0x0604, 0x0700, 0x0701, 0x0702, 0x0703, 0x0800,
    0x0801, 0x0900, 0x0901, 0x0902, 0x0A00, 0x0B00, 0x0B01, 0x0B02, 0x0B03, 0x0C00, 0x0C01, 0x0D00,
    0x0D01, 0x0D02, 0x0D03, 0x0E00, 0x0E01,
};
_Static_assert(sizeof(rows) / sizeof(rows[0]) <= DPMI_ROW_WORDS * 32, "row bitmap");
int dpmi_row(unsigned ax)
{
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        if (rows[i] == ax) return (int)i;
    return -1;
}
static void mark(struct dosvm *v, unsigned ax, bool ok)
{
    int r = dpmi_row(ax);
    if (r < 0) return;
    uint32_t *w = ok ? v->dpmi.stats.rows_ok : v->dpmi.stats.rows_fail;
    w[r / 32] |= 1u << (r % 32);
}

/* ROM stubs: "INT FEh; HLT; HLT" real-mode entries at F000:1040h-11FFh and
 * HLT-only protected-mode stubs at F1200h-F13FFh (page F1000h is shared
 * with the EMS entry F100:0000 and the INT 33h continuation F000:1020). */
int dpmi_rom_init(struct dosvm *v)
{
    uint8_t *page = P2V(v->frames[0xF1]);
    for (uint32_t off = DPMI_RM_ENTRY; off < DPMI_RM_END; off += 4) {
        if (off > DPMI_RM_SAVE && off < DPMI_RM_CALLBACK) continue;
        page[(off & 0xFFF)] = 0xCD; page[(off & 0xFFF) + 1] = 0xFE;
        page[(off & 0xFFF) + 2] = 0xF4; page[(off & 0xFFF) + 3] = 0xF4;
    }
    memset(page + (DPMI_PM_BASE & 0xFFF), 0xF4, DPMI_PM_LIMIT + 1);
    return 0;
}

bool dpmi_linear_owned(struct dosvm *v, uint32_t linear, uint32_t bytes)
{
    struct dpmi *d = &v->dpmi;
    if (linear < DOSVM_LINEAR_END && bytes <= DOSVM_LINEAR_END - linear) return true;
    const struct vm_block *video=&v->vbe_buffer;
    if (video->used && linear>=video->base && bytes<=video->bytes && linear-video->base<=video->bytes-bytes)
        return true;
    for (unsigned i = 0; i < DPMI_BLOCKS; i++) {
        const struct vm_block *b = &d->block[i].b;
        if (b->used && d->block[i].owner && d->block[i].owner <= d->clients &&
            linear >= b->base && bytes <= b->pages * PAGE_SIZE && linear - b->base <= b->pages * PAGE_SIZE - bytes)
            return true;
    }
    for (unsigned i = 0; i < VM_MEM_HANDLES; i++) {
        const struct vm_block *b = &v->memory.xms[i];
        if (b->used && linear >= b->base && bytes <= b->pages * PAGE_SIZE &&
            linear - b->base <= b->pages * PAGE_SIZE - bytes)
            return true;
    }
    return false;
}

void dpmi_reload_debug(struct dosvm *v)
{
    struct dpmi *d = &v->dpmi;
    uint32_t flags = irq_save();
    uint32_t address[4] = {0}, dr7 = 0;
    static const uint8_t len[5] = {0, 0, 1, 0, 3}, rw[3] = {0, 1, 3};
    for (unsigned i = 0; i < DPMI_WATCH; i++) {
        if (!d->watch[i].used) continue;
        address[i] = d->watch[i].addr;
        dr7 |= (1u << (i * 2)) | ((uint32_t)rw[d->watch[i].type] << (16 + i * 4)) |
               ((uint32_t)len[d->watch[i].size] << (18 + i * 4));
    }
    /* Unallocated slots carry no client state. Keeping the temporary
     * address there also lets the existing task-switch hook reload it;
     * .used and .hit remain exclusively the client's 0Bxx service state. */
    for (unsigned i = 0; i < DPMI_WATCH; i++)
        if (!d->watch[i].used) d->watch[i].addr = 0;
    d->return_slot = DPMI_WATCH;
    if (!v->return_tracker_tf && d->return_steps && !d->return_step && v->frame_pm &&
        d->depth == d->return_depth && d->clients == d->return_client) {
        for (unsigned i = DPMI_WATCH; i-- > 0;) if (!d->watch[i].used) {
            d->return_slot = (uint8_t)i;
            d->watch[i].addr = address[i] = d->return_linear;
            dr7 |= 1u << (i * 2);       /* local, execute, length 1 */
            break;
        }
    }
    if (dr7) dr7 |= 1u << 8;            /* LE: exact data breakpoints */
    d->dr7 = dr7;
    if (g_current == v->task) arch_debug_load(address, dr7);
    irq_restore(flags);
}

void dpmi_return_cancel(struct dosvm *v)
{
    if (!v->dpmi.return_steps && !v->dpmi.return_step && v->dpmi.return_slot >= DPMI_WATCH) return;
    v->dpmi.return_steps = 0;
    v->dpmi.return_step = v->dpmi.return_replan = false;
    dpmi_reload_debug(v);
}

/* f3-46 (DPMI 0.9 section 15): a lock count above zero keeps a pool page
 * pinned resident; low pages are always resident. Clearing returns the pin. */
static void lock_clear(struct dosvm *v, uint32_t page)
{
    if (page >= DPMI_LOCK_PAGES || !v->dpmi.lock[page]) return;
    if (page >= DOSVM_PAGES) dosvm_pool_unpin(v, page << 12, 1);
    v->dpmi.lock[page] = 0;
}
/* Free every host resource without guest help; stats survive. */
static void reset(struct dosvm *v)
{
    struct dpmi *d = &v->dpmi;
    struct dpmi_stats stats = d->stats;
    for (uint32_t p = DOSVM_PAGES; p < DPMI_LOCK_PAGES; p++) lock_clear(v, p);
    for (unsigned i = 0; i < DPMI_BLOCKS; i++)
        if (d->block[i].b.used) dosvm_pool_free(v, &d->block[i].b);
    if (d->hstack.used) {
        dosvm_pool_unpin(v, d->hstack.base, d->hstack.pages);
        dosvm_pool_free(v, &d->hstack);
    }
    memset(d, 0, sizeof(*d));
    d->stats = stats;
    if (g_current == v->task) {
        arch_ldt_load(0, 0);
        arch_debug_load((const uint32_t[4]){0}, 0);
    }
}
void dpmi_close(struct dosvm *v)
{
    if (v->dpmi.active) reset(v);
}

static int activate(struct dosvm *v, bool bits32)
{
    struct dpmi *d = &v->dpmi;
    struct dpmi_stats stats = d->stats;
    memset(d, 0, sizeof(*d));
    d->stats = stats;
    d->bits32 = bits32;
    if (!dosvm_pool_alloc(v, &d->hstack, DPMI_HSTACK_BYTES)) { d->stats.entry_failures++; return -ENOMEM; }
    /* f3-46: the locked host stack takes interrupt/exception frames from
     * the monitor; it stays resident for the host's lifetime. */
    if (dosvm_pool_pin(v, d->hstack.base, d->hstack.pages)) {
        dosvm_pool_free(v, &d->hstack); d->stats.entry_failures++; return -ENOMEM;
    }
    d->active = true;
    d->kind[DPMI_IDX_CODE] = d->kind[DPMI_IDX_STACK] = DPMI_HOST;
    dpmi_desc_set(d->ldt[DPMI_IDX_CODE], DPMI_PM_BASE, DPMI_PM_LIMIT, 0xFA, 0);
    dpmi_desc_set(d->ldt[DPMI_IDX_STACK], d->hstack.base, DPMI_HSTACK_BYTES - 1, 0xF2, bits32 ? 0x40 : 0);
    for (unsigned i = 0; i < 256; i++) d->vec[i] = (struct dpmi_vec){DPMI_PM_INT + i, DPMI_HOST_CS};
    for (unsigned i = 0; i < 32; i++) d->exc[i] = (struct dpmi_vec){DPMI_PM_EXC + i, DPMI_HOST_CS};
    d->lock_top = DPMI_HSTACK_BYTES;
    d->rstack_top = DPMI_RSTACK_BYTES;
    d->rm_a20 = v->a20;
    if (g_current == v->task) arch_ldt_load(d->ldt, DPMI_LDT_ENTRIES);
    return 0;
}

static int entry_fail(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    uint16_t ip, cs;
    struct v86_frame n = *f;
    if (!dpmi_rm_pop(v, &n, &ip) || !dpmi_rm_pop(v, &n, &cs)) return -EFAULT;
    n.tf.eip = ip; n.tf.cs = cs;
    dpmi_carry(&n, true);
    *f = n;
    d->stats.entry_failures++;
    if (d->active && !d->clients && !d->depth) reset(v);
    return 0;
}

/* Far call to F000:DPMI_RM_ENTRY (0.9 section 5.2). The PSP comes from a
 * real-mode INT 21h/62h issued by the host, then the switch completes. */
int dpmi_entry(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    bool bits32 = f->tf.eax & 1;
    if (!v->dpmi_enabled || d->clients >= DPMI_CLIENTS || (d->clients && d->bits32 != bits32))
        return entry_fail(v, f);
    if (!d->active && activate(v, bits32)) return entry_fail(v, f);
    struct v86_frame regs;
    memset(&regs, 0, sizeof(regs));
    regs.tf.eax = 0x6200;
    if (dpmi_hostcall(v, f, DPMI_OP_ENTRY, &regs)) return entry_fail(v, f);
    return 0;
}

static uint32_t env_limit(struct dosvm *v, uint16_t env)
{
    uint8_t mcb[5];
    if (!env || dosvm_read(v, ((uint32_t)env - 1) << 4, mcb, sizeof(mcb))) return 0xFFFF;
    uint16_t paras = vm_u16(mcb + 3);
    if ((mcb[0] != 'M' && mcb[0] != 'Z') || !paras || paras > 0x1000) return 0xFFFF;
    return (uint32_t)paras * 16 - 1;
}

int dpmi_entry_complete(struct dosvm *v, struct v86_frame *f, uint16_t psp)
{
    struct dpmi *d = &v->dpmi;
    struct v86_frame n = *f;
    uint16_t ip, rcs, env = 0;
    if (!dpmi_rm_pop(v, &n, &ip) || !dpmi_rm_pop(v, &n, &rcs)) return -EFAULT;
    unsigned c = d->clients;
    uint8_t id = (uint8_t)(c + 1);
    uint16_t rds = (uint16_t)f->ds, rss = (uint16_t)n.tf.user_ss;
    int ics = dpmi_alloc_ldt(v, 1, DPMI_CLIENT, id);
    if (ics >= 0) dpmi_desc_set(d->ldt[ics], (uint32_t)rcs << 4, 0xFFFF, 0xFA, 0);
    int ids = dpmi_seg_data(v, DPMI_CLIENT, id, (uint32_t)rds << 4, 0xFFFF, d->bits32);
    int iss = rss == rds ? ids : dpmi_seg_data(v, DPMI_CLIENT, id, (uint32_t)rss << 4, 0xFFFF, d->bits32);
    int ipsp = dpmi_seg_data(v, DPMI_FIXED, id, (uint32_t)psp << 4, 0xFF, false);
    int ienv = 0;
    if (dosvm_read(v, ((uint32_t)psp << 4) + 0x2C, &env, 2)) env = 0;
    if (env) ienv = dpmi_seg_data(v, DPMI_FIXED, id, (uint32_t)env << 4, env_limit(v, env), false);
    uint16_t env_sel = ienv > 0 ? DPMI_SEL(ienv) : 0;
    if (ics < 0 || ids < 0 || iss < 0 || ipsp < 0 || ienv < 0 ||
        (env && dosvm_write(v, ((uint32_t)psp << 4) + 0x2C, &env_sel, 2))) {
        for (unsigned i = 0; i < DPMI_LDT_ENTRIES; i++) if (d->owner[i] == id) dpmi_free_ldt(v, i);
        return entry_fail(v, f);
    }
    struct dpmi_client *cl = &d->client[c];
    memset(cl, 0, sizeof(*cl));
    cl->used = true;
    cl->psp = psp; cl->env = env;
    cl->psp_sel = DPMI_SEL(ipsp); cl->env_sel = env_sel;
    cl->rm_ss = rss; cl->rm_sp = (uint16_t)n.tf.user_esp;
    cl->rm_cs = rcs; cl->rm_ip = ip;
    cl->depth_base = d->depth;
    memcpy(cl->saved_vec, d->vec, sizeof(d->vec));
    memcpy(cl->saved_exc, d->exc, sizeof(d->exc));
    if (c) d->client[c - 1].suspended_if_armed = d->if_armed;
    d->clients++;
    dpmi_cli_sites_reset(v); /* fx-16: proofs belong to one client's code */
    struct v86_frame p;
    memset(&p, 0, sizeof(p));
    p.tf.eax = n.tf.eax; p.tf.ebx = n.tf.ebx; p.tf.ecx = n.tf.ecx; p.tf.edx = n.tf.edx;
    p.tf.esi = n.tf.esi; p.tf.edi = n.tf.edi; p.tf.ebp = n.tf.ebp;
    p.tf.cs = DPMI_SEL(ics);
    p.tf.eip = ip;
    p.tf.user_ss = DPMI_SEL(iss);
    p.tf.user_esp = (uint16_t)n.tf.user_esp;   /* 32-bit client: high word zero */
    p.tf.ds = DPMI_SEL(ids);
    p.tf.es = DPMI_SEL(ipsp);
    p.tf.eflags = (n.tf.eflags & 0x0CD5u & ~(V86_TF | V86_CF)) | V86_IF | 2u;
    dpmi_set_pm(v, true);
    *f = p;
    d->if_armed = false;
    d->decode_valid = true;
    d->decode_cs = (uint16_t)p.tf.cs;
    d->decode_eip = p.tf.eip;
    d->decode_stack_valid = d->decode_ds_valid = false; /* entry is a new stack context */
    d->stats.entries++;
    return 0;
}

/* One cleanup path for 4Ch/00h/31h/INT 20h/27h from either mode and for
 * fault termination: records, memory, callbacks, watchpoints, selectors,
 * the environment pointer and the parent's vectors. No guest code runs. */
int dpmi_terminate(struct dosvm *v, bool from_pm)
{
    struct dpmi *d = &v->dpmi;
    (void)from_pm;
    if (!d->clients) return 0;
    d->decode_valid = false;
    dpmi_cli_sites_reset(v);
    unsigned c = d->clients - 1;
    v->vbe_mapped[c]=false;
    uint8_t id = (uint8_t)(c + 1);
    if (d->return_steps && d->return_client == id) dpmi_return_cancel(v);
    struct dpmi_client *cl = &d->client[c];
    dpmi_unwind(v, cl->depth_base);
    if (cl->env_sel) dosvm_write(v, ((uint32_t)cl->psp << 4) + 0x2C, &cl->env, 2);
    for (unsigned i = 0; i < DPMI_BLOCKS; i++)
        if (d->block[i].b.used && d->block[i].owner == id) {
            uint32_t first = d->block[i].b.base >> 12;
            for (uint32_t p = 0; p < d->block[i].b.pages && first + p < DPMI_LOCK_PAGES; p++) lock_clear(v, first + p);
            dosvm_pool_free(v, &d->block[i].b);
            memset(&d->block[i], 0, sizeof(d->block[i]));
        }
    for (unsigned i = 0; i < DPMI_DOS_BLOCKS; i++)
        if (d->dos[i].owner == id) memset(&d->dos[i], 0, sizeof(d->dos[i]));
    for (unsigned i = 0; i < DPMI_WATCH; i++)
        if (d->watch[i].owner == id) memset(&d->watch[i], 0, sizeof(d->watch[i]));
    for (unsigned i = 0; i < DPMI_LDT_ENTRIES; i++)
        if (d->owner[i] == id && d->kind[i] != DPMI_ALIAS) dpmi_free_ldt(v, i);
    memcpy(d->vec, cl->saved_vec, sizeof(d->vec));
    memcpy(d->exc, cl->saved_exc, sizeof(d->exc));
    memset(cl, 0, sizeof(*cl));
    d->clients--;
    d->stats.terminations++;
    if (!d->clients) { dpmi_unwind(v, 0); reset(v); }
    else {
        d->if_armed = d->client[d->clients - 1].suspended_if_armed;
        dpmi_reload_debug(v);
    }
    return 0;
}

/* Terminating DOS call issued by the client in protected mode (directly,
 * through the default handler, or through 0300h): clean up, then run the
 * call in V86 on the host real-mode stack. DOS does not return here. */
int dpmi_terminate_int(struct dosvm *v, struct v86_frame *f, uint8_t vector, const struct v86_frame *regs)
{
    struct dpmi *d = &v->dpmi;
    struct v86_frame n;
    memset(&n, 0, sizeof(n));
    if (regs) n = *regs;
    else {
        n.tf.eax = f->tf.eax; n.tf.ebx = f->tf.ebx; n.tf.ecx = f->tf.ecx; n.tf.edx = f->tf.edx;
        n.tf.esi = f->tf.esi; n.tf.edi = f->tf.edi; n.tf.ebp = f->tf.ebp;
    }
    uint16_t image = (uint16_t)((f->tf.eflags & 0x0CD5u) | (v->monitor.vif ? V86_IF : 0) | 2u);
    /* The DOS call carries the program's own return CS:IP (its entry far
     * return): CIUKIDOS selects the COM or MZ termination trampoline from
     * the INT 21h caller's CS (int21 exit path, current_com_load_seg). */
    uint16_t rcs = d->clients ? d->client[d->clients - 1].rm_cs : 0xF000;
    uint16_t rip = d->clients ? d->client[d->clients - 1].rm_ip : DPMI_RM_RETURN;
    dpmi_set_pm(v, false);
    dpmi_terminate(v, true);
    uint16_t ss = DPMI_RSTACK_SEG, sp = DPMI_RSTACK_BYTES;
    if (d->active && dpmi_rm_stack(v, &ss, &sp)) { ss = DPMI_RSTACK_SEG; sp = DPMI_RSTACK_LEVEL; }
    n.tf.user_ss = ss;
    n.tf.user_esp = sp;
    if (dpmi_real_int(v, &n, vector, image)) return -EFAULT;
    /* Replace the host return address dpmi_real_int pushed (IP, CS). */
    uint32_t frame = ((uint32_t)ss << 4) + (uint16_t)n.tf.user_esp;
    if (dosvm_write(v, frame, &rip, 2) || dosvm_write(v, frame + 2, &rcs, 2)) return -EFAULT;
    *f = n;
    return 0;
}

/* ---- DOS memory 0100h-0102h through the personality (real-mode INT 21h
 * 48h/49h/4Ah issued by the host, owned by the client's PSP). ---- */
static void dos_desc(struct dosvm *v, unsigned first, unsigned count, uint16_t seg, uint16_t paras)
{
    uint32_t total = (uint32_t)paras * 16;
    for (unsigned i = 0; i < count; i++) {
        uint32_t start = i * 0x10000u, remain = total > start ? total - start : 0;
        uint32_t limit = i == 0 ? (total ? total - 1 : 0) : (remain > 0x10000u ? 0xFFFFu : (remain ? remain - 1 : 0));
        dpmi_desc_set(v->dpmi.ldt[first + i], ((uint32_t)seg << 4) + start, limit, 0xF2, 0);
    }
}
static unsigned dos_count(uint16_t paras) { return paras ? (paras + 0xFFFu) >> 12 : 1; }
static int dos_find(struct dosvm *v, uint16_t sel)
{
    for (unsigned i = 0; i < DPMI_DOS_BLOCKS; i++)
        if (v->dpmi.dos[i].owner == dpmi_id(v) && v->dpmi.dos[i].sel == sel && v->dpmi.dos[i].count) return (int)i;
    return -1;
}
static int dos_memory(struct dosvm *v, struct v86_frame *f, unsigned ax)
{
    struct dpmi *d = &v->dpmi;
    uint16_t bx = (uint16_t)f->tf.ebx, dx = (uint16_t)f->tf.edx;
    struct v86_frame regs;
    memset(&regs, 0, sizeof(regs));
    int slot = -1;
    if (ax == 0x0100) {
        for (unsigned i = 0; i < DPMI_DOS_BLOCKS && slot < 0; i++) if (!d->dos[i].count) slot = (int)i;
        unsigned count = dos_count(bx);
        int first = slot < 0 ? -1 : dpmi_alloc_ldt(v, count, DPMI_DOS, dpmi_id(v));
        regs.tf.eax = 0x4800; regs.tf.ebx = bx;
        if (first < 0 || dpmi_hostcall(v, f, DPMI_OP_DOS_ALLOC, &regs)) {
            if (first >= 0) for (unsigned i = 0; i < count; i++) dpmi_free_ldt(v, (unsigned)first + i);
            dpmi_low(&f->tf.eax, 8); dpmi_low(&f->tf.ebx, 0);
            return 1;
        }
        struct dpmi_record *r = &d->rec[d->depth - 1];
        r->arg[0] = (uint32_t)first; r->arg[1] = count; r->arg[2] = (uint32_t)slot; r->arg[3] = bx;
        return 2;
    }
    if ((slot = dos_find(v, dx)) < 0) { dpmi_low(&f->tf.eax, 9); return 1; }
    struct dpmi_dos *b = &d->dos[slot];
    if (ax == 0x0101) {
        regs.tf.eax = 0x4900; regs.es = b->seg;
        if (dpmi_hostcall(v, f, DPMI_OP_DOS_FREE, &regs)) return 1;
        d->rec[d->depth - 1].arg[2] = (uint32_t)slot;
        return 2;
    }
    unsigned count = dos_count(bx), first = b->sel >> 3;
    if (count > b->count) {
        for (unsigned i = first + b->count; i < first + count; i++)
            if (i > DPMI_IDX_LAST_CLIENT || d->kind[i] != DPMI_FREE) {
                dpmi_low(&f->tf.eax, 8); dpmi_low(&f->tf.ebx, b->paras);
                return 1;
            }
        for (unsigned i = first + b->count; i < first + count; i++) { d->kind[i] = DPMI_DOS; d->owner[i] = dpmi_id(v); }
    }
    regs.tf.eax = 0x4A00; regs.tf.ebx = bx; regs.es = b->seg;
    if (dpmi_hostcall(v, f, DPMI_OP_DOS_RESIZE, &regs)) {
        for (unsigned i = first + b->count; i < first + count; i++) dpmi_free_ldt(v, i);
        return 1;
    }
    struct dpmi_record *r = &d->rec[d->depth - 1];
    r->arg[1] = count; r->arg[2] = (uint32_t)slot; r->arg[3] = bx;
    return 2;
}
int dpmi_dos_complete(struct dosvm *v, struct v86_frame *f, struct dpmi_record *rec, const struct v86_frame *res)
{
    struct dpmi *d = &v->dpmi;
    bool cf = res->tf.eflags & V86_CF;
    unsigned slot = rec->arg[2];
    if (slot >= DPMI_DOS_BLOCKS) return -EFAULT;
    struct dpmi_dos *b = &d->dos[slot];
    if (rec->op == DPMI_OP_DOS_ALLOC) {
        unsigned first = rec->arg[0], count = rec->arg[1];
        if (cf) {
            for (unsigned i = 0; i < count; i++) dpmi_free_ldt(v, first + i);
            dpmi_low(&f->tf.eax, res->tf.eax); dpmi_low(&f->tf.ebx, res->tf.ebx);
        } else {
            uint16_t seg = (uint16_t)res->tf.eax;
            dos_desc(v, first, count, seg, (uint16_t)rec->arg[3]);
            *b = (struct dpmi_dos){seg, (uint16_t)rec->arg[3], DPMI_SEL(first), (uint16_t)count, dpmi_id(v)};
            dpmi_low(&f->tf.eax, seg); dpmi_low(&f->tf.edx, DPMI_SEL(first));
        }
        mark(v, 0x0100, !cf);
    } else if (rec->op == DPMI_OP_DOS_FREE) {
        if (cf) dpmi_low(&f->tf.eax, res->tf.eax);
        else {
            for (unsigned i = 0; i < b->count; i++) dpmi_free_ldt(v, (b->sel >> 3) + i);
            memset(b, 0, sizeof(*b));
        }
        mark(v, 0x0101, !cf);
    } else {
        unsigned count = rec->arg[1], first = b->sel >> 3;
        if (cf) {
            for (unsigned i = first + b->count; i < first + count; i++) dpmi_free_ldt(v, i);
            dpmi_low(&f->tf.eax, res->tf.eax); dpmi_low(&f->tf.ebx, res->tf.ebx);
        } else {
            for (unsigned i = first + count; i < first + b->count; i++) dpmi_free_ldt(v, i);
            dos_desc(v, first, count, b->seg, (uint16_t)rec->arg[3]);
            b->paras = (uint16_t)rec->arg[3]; b->count = (uint16_t)count;
        }
        mark(v, 0x0102, !cf);
    }
    dpmi_carry(f, cf);
    return 0;
}

/* ---- 0500h-0503h: the per-VM pool shared with XMS/EMS (f3-03). ---- */
static struct dpmi_block *block_find(struct dosvm *v, uint32_t handle)
{
    for (unsigned i = 0; i < DPMI_BLOCKS; i++)
        if (v->dpmi.block[i].b.used && v->dpmi.block[i].handle == handle && v->dpmi.block[i].owner == dpmi_id(v))
            return &v->dpmi.block[i];
    return 0;
}
static uint32_t new_handle(struct dosvm *v, unsigned slot)
{ return ((++v->dpmi.serial & 0xFFFFFFu) << 8) | (slot + 1); }
static void put32(uint32_t *hi, uint32_t *lo, uint32_t value) { dpmi_low(hi, value >> 16); dpmi_low(lo, value); }
static uint32_t get32(uint32_t hi, uint32_t lo) { return ((uint32_t)(uint16_t)hi << 16) | (uint16_t)lo; }

static int memory_service(struct dosvm *v, struct v86_frame *f, unsigned ax)
{
    struct dpmi *d = &v->dpmi;
    if (ax == 0x0500) {
        uint32_t lin, largest, total, info[12];
        if (!dpmi_client_ptr(v, f, (uint16_t)f->tf.es, f->tf.edi, sizeof(info), true, &lin)) return 1;
        dosvm_pool_query(v, &largest, &total);
        for (unsigned i = 0; i < 12; i++) info[i] = 0xFFFFFFFFu;
        info[0] = largest;
        info[1] = info[2] = largest / PAGE_SIZE;
        info[3] = v->memory.quota / PAGE_SIZE;
        info[5] = total / PAGE_SIZE;
        info[6] = v->memory.quota / PAGE_SIZE;
        info[7] = total / PAGE_SIZE;
        if (dosvm_write(v, lin, info, sizeof(info))) return 1;
        uint32_t flags = irq_save();
        struct dosvm_memory_cost *m = &v->perf.memory;
        if (!m->queries++) { m->first_largest = largest; m->first_total = total; }
        m->largest = largest; m->total = total;
        irq_restore(flags);
        return 0;
    }
    uint32_t size = get32(f->tf.ebx, f->tf.ecx), handle = get32(f->tf.esi, f->tf.edi);
    if (ax == 0x0501) {
        int slot = -1;
        for (unsigned i = 0; i < DPMI_BLOCKS && slot < 0; i++) if (!d->block[i].b.used) slot = (int)i;
        if (!size || slot < 0 || !dosvm_pool_alloc(v, &d->block[slot].b, size)) return 1;
        d->block[slot].owner = dpmi_id(v);
        d->block[slot].handle = new_handle(v, (unsigned)slot);
        /* Reporting only: keep the allocator, handles and returned registers
         * unchanged. Emit at the regular perf cadence, never on this path. */
        uint32_t largest, total;
        dosvm_pool_query(v, &largest, &total);
        uint32_t flags = irq_save();
        v->perf.memory.allocations++; v->perf.memory.requested = size;
        v->perf.memory.after_largest = largest; v->perf.memory.after_total = total;
        irq_restore(flags);
        put32(&f->tf.ebx, &f->tf.ecx, d->block[slot].b.base);
        put32(&f->tf.esi, &f->tf.edi, d->block[slot].handle);
        return 0;
    }
    struct dpmi_block *b = block_find(v, handle);
    if (!b) return 1;
    uint32_t first = b->b.base >> 12, pages = b->b.pages;
    if (ax == 0x0502) {
        for (uint32_t p = 0; p < pages; p++) lock_clear(v, first + p);
        dosvm_pool_free(v, &b->b);
        memset(b, 0, sizeof(*b));
        return 0;
    }
    if (ax == 0x0503) {
        if (!size || !dosvm_pool_resize(v, &b->b, size)) return 1;
        uint32_t now = b->b.base >> 12;
        if (now != first) {
            /* Locks follow the relocated pages: pin the new page before
             * the old one's pin returns (a failed pin drops that lock). */
            for (uint32_t p = 0; p < pages && p < b->b.pages; p++) {
                uint8_t n = d->lock[first + p];
                if (n && !dosvm_pool_pin(v, (now + p) << 12, 1)) d->lock[now + p] = n;
                lock_clear(v, first + p);
            }
            for (uint32_t p = 0; p < pages; p++) lock_clear(v, first + p);
            b->handle = new_handle(v, (unsigned)(b - d->block));
        } else for (uint32_t p = b->b.pages; p < pages; p++) lock_clear(v, first + p);
        put32(&f->tf.ebx, &f->tf.ecx, b->b.base);
        put32(&f->tf.esi, &f->tf.edi, b->handle);
        return 0;
    }
    return 1;
}

/* ---- 0600h-0604h, 0702h/0703h: validated page ranges. f3-46: the pool
 * is pageable; lock counts pin pool pages resident (0.9 sections 14-15). ---- */
static bool page_valid(struct dosvm *v, uint32_t page)
{
    if (page < DOSVM_PAGES) return true;
    uint32_t k = page - DOSVM_PAGES;
    return k < v->memory.quota / PAGE_SIZE && v->memory.occupied[k];
}
static bool range(uint32_t lin, uint32_t size, uint32_t *first, uint32_t *last)
{
    if (!size || lin + size - 1 < lin) return false;
    *first = lin >> 12;
    *last = (lin + size - 1) >> 12;
    return *last < DPMI_LOCK_PAGES;
}
static struct dpmi_block *block_of(struct dosvm *v, uint32_t lin, uint32_t size)
{
    for (unsigned i = 0; i < DPMI_BLOCKS; i++) {
        struct dpmi_block *b = &v->dpmi.block[i];
        if (b->b.used && b->owner == dpmi_id(v) && lin >= b->b.base &&
            size <= b->b.pages * PAGE_SIZE && lin - b->b.base <= b->b.pages * PAGE_SIZE - size)
            return b;
    }
    return 0;
}
static int paging_service(struct dosvm *v, struct v86_frame *f, unsigned ax)
{
    struct dpmi *d = &v->dpmi;
    uint32_t lin = get32(f->tf.ebx, f->tf.ecx), size = get32(f->tf.esi, f->tf.edi), first, last;
    if (ax == 0x0604) { dpmi_low(&f->tf.ebx, 0); dpmi_low(&f->tf.ecx, PAGE_SIZE); return 0; }
    if (!size) return 0;                /* empty region: nothing to validate or change */
    if (ax == 0x0602 || ax == 0x0603) {
        /* Real-mode region: below 1 MiB; only whole pages change state. */
        if (!size || lin >= 0x100000u || size > 0x100000u - lin) return 1;
        uint32_t lo = (lin + PAGE_SIZE - 1) >> 12, hi = (lin + size) >> 12;
        for (uint32_t p = lo; p < hi; p++) {
            if (ax == 0x0602) d->pageable[p / 8] |= (uint8_t)(1u << (p % 8));
            else d->pageable[p / 8] &= (uint8_t)~(1u << (p % 8));
        }
        return 0;
    }
    if (!range(lin, size, &first, &last)) return 1;
    for (uint32_t p = first; p <= last; p++) if (!page_valid(v, p)) return 1;
    switch (ax) {
    case 0x0600:
        for (uint32_t p = first; p <= last; p++) if (d->lock[p] == 255) return 1;
        /* f3-46: a first lock pins (resolving) the pool page; on any
         * failure none of the region is locked (DPMI 0.9, 0600h). */
        for (uint32_t p = first; p <= last; p++)
            if (!d->lock[p] && p >= DOSVM_PAGES && dosvm_pool_pin(v, p << 12, 1)) {
                for (uint32_t q = first; q < p; q++)
                    if (!d->lock[q] && q >= DOSVM_PAGES) dosvm_pool_unpin(v, q << 12, 1);
                return 1;
            }
        for (uint32_t p = first; p <= last; p++) d->lock[p]++;
        return 0;
    case 0x0601:
        for (uint32_t p = first; p <= last; p++) if (!d->lock[p]) return 1;
        for (uint32_t p = first; p <= last; p++)
            if (!--d->lock[p] && p >= DOSVM_PAGES) dosvm_pool_unpin(v, p << 12, 1);
        return 0;
    case 0x0702: {
        /* Advisory: whole pages become early replacement candidates;
         * contents are preserved (DPMI 0.9, 0702h). */
        uint32_t lo = (lin + PAGE_SIZE - 1) >> 12, hi = (lin + size) >> 12;
        if (hi > lo) dosvm_pool_advise(v, lo << 12, hi - lo);
        return 0;
    }
    case 0x0703: {
        /* Discard whole pages of the client's own memory blocks only;
         * partial edge pages keep their contents (0.9 15.3). */
        if (!block_of(v, lin, size)) return 1;
        uint32_t lo = (lin + PAGE_SIZE - 1) >> 12, hi = (lin + size) >> 12;
        /* f3-46: locked pages are not discarded (0.9, 0703h); others
         * return to demand-zero, releasing frame and pager slot. */
        for (uint32_t p = lo; p < hi; p++) if (!d->lock[p]) dosvm_pool_zero(v, p << 12, 1);
        return 0;
    }
    default: return 1;
    }
}

static int watch_service(struct dosvm *v, struct v86_frame *f, unsigned ax)
{
    struct dpmi *d = &v->dpmi;
    unsigned h = (uint16_t)f->tf.ebx;
    if (ax == 0x0B00) {
        uint32_t lin = get32(f->tf.ebx, f->tf.ecx);
        uint8_t size = (uint8_t)f->tf.edx, type = (uint8_t)(f->tf.edx >> 8);
        if ((size != 1 && size != 2 && size != 4) || type > 2 || (type == 0 && size != 1) || (lin & (size - 1u)) ||
            !dosvm_range(v, lin, size, false))
            return 1;
        for (unsigned i = 0; i < DPMI_WATCH; i++) if (!d->watch[i].used) {
            d->watch[i] = (struct dpmi_watch){true, false, dpmi_id(v), size, type, lin};
            dpmi_reload_debug(v);
            dpmi_low(&f->tf.ebx, i);
            return 0;
        }
        return 1;
    }
    if (h >= DPMI_WATCH || !d->watch[h].used || d->watch[h].owner != dpmi_id(v)) return 1;
    switch (ax) {
    case 0x0B01: memset(&d->watch[h], 0, sizeof(d->watch[h])); dpmi_reload_debug(v); return 0;
    case 0x0B02: dpmi_low(&f->tf.eax, d->watch[h].hit ? 1 : 0); return 0;
    case 0x0B03: d->watch[h].hit = false; return 0;
    default: return 1;
    }
}

static bool vector_ok(struct dosvm *v, uint16_t sel, uint32_t off, uint32_t base, unsigned count)
{
    if (sel == DPMI_HOST_CS) return off >= base && off < base + count;
    return dpmi_code_selector(v, sel, off);
}
static void put_offset(struct dosvm *v, uint32_t *reg, uint32_t value)
{ if (v->dpmi.bits32) *reg = value; else dpmi_low(reg, value); }

/* Returns 0 success, 1 failure (CF), 2 transition started, <0 VM fault. */
static int service(struct dosvm *v, struct v86_frame *f, unsigned ax)
{
    struct dpmi *d = &v->dpmi;
    uint8_t bl = (uint8_t)f->tf.ebx;
    uint16_t cx = (uint16_t)f->tf.ecx, dx = (uint16_t)f->tf.edx;
    switch (ax >> 8) {
    case 0x00: return dpmi_ldt_service(v, f, ax);
    case 0x01: return ax <= 0x0102 ? dos_memory(v, f, ax) : 1;
    case 0x02:
        switch (ax) {
        case 0x0200: {
            uint16_t ivt[2];
            if (dosvm_read(v, (uint32_t)bl * 4, ivt, 4)) return 1;
            dpmi_low(&f->tf.ecx, ivt[1]); dpmi_low(&f->tf.edx, ivt[0]);
            return 0;
        }
        case 0x0201: {
            uint16_t ivt[2] = {dx, cx};
            int e = dosvm_write(v, (uint32_t)bl * 4, ivt, 4) ? 1 : 0;
            if (!e) dosvm_trace(v, DOSVM_TRACE_VECTOR, f, (ax << 8) | bl, cx, dx);
            return e;
        }
        case 0x0202: case 0x0204: {
            if (ax == 0x0202 && bl > 0x1F) return 1;
            struct dpmi_vec h = ax == 0x0202 ? d->exc[bl] : d->vec[bl];
            dpmi_low(&f->tf.ecx, h.sel); put_offset(v, &f->tf.edx, h.off);
            return 0;
        }
        case 0x0203: case 0x0205: {
            uint32_t off = dpmi_off(v, f->tf.edx);
            if (ax == 0x0203) {
                if (bl > 0x1F || !vector_ok(v, cx, off, DPMI_PM_EXC, 32)) return 1;
                d->exc[bl] = (struct dpmi_vec){off, cx};
            } else {
                if (!vector_ok(v, cx, off, DPMI_PM_INT, 256)) return 1;
                d->vec[bl] = (struct dpmi_vec){off, cx};
                if (bl >= 8 && bl <= 15)
                    dpmi_trace_irq_target(v, f, "pm_irq_vector", bl, d->vec[bl]);
            }
            dosvm_trace(v, DOSVM_TRACE_VECTOR, f, (ax << 8) | bl, cx, off);
            return 0;
        }
        default: return 1;              /* 0210h-0213h are DPMI 1.0 */
        }
    case 0x03:
        switch (ax) {
        case 0x0300: case 0x0301: case 0x0302: {
            int e = dpmi_translation(v, f, ax);
            return e == 0 ? 2 : e;
        }
        case 0x0303: {
            uint32_t lin, off = dpmi_off(v, f->tf.esi), soff = dpmi_off(v, f->tf.edi);
            if (!d->clients || !dpmi_code_selector(v, (uint16_t)f->tf.ds, off) ||
                !dpmi_ptr(v, (uint16_t)f->tf.es, soff, 0x32, true, &lin)) return 1;
            struct dpmi_client *cl = &d->client[d->clients - 1];
            for (unsigned k = 0; k < DPMI_CALLBACKS; k++) if (!cl->cb[k].used) {
                cl->cb[k] = (struct dpmi_callback){true, (uint16_t)f->tf.ds, (uint16_t)f->tf.es, off, soff};
                dpmi_low(&f->tf.ecx, 0xF000);
                dpmi_low(&f->tf.edx, DPMI_RM_CALLBACK + 4 * ((d->clients - 1) * DPMI_CALLBACKS + k));
                return 0;
            }
            return 1;
        }
        case 0x0304: {
            unsigned base = DPMI_RM_CALLBACK + 4 * (d->clients - 1) * DPMI_CALLBACKS;
            if (!d->clients || cx != 0xF000 || dx < base || dx >= base + 4 * DPMI_CALLBACKS || (dx - base) % 4) return 1;
            struct dpmi_callback *cb = &d->client[d->clients - 1].cb[(dx - base) / 4];
            if (!cb->used) return 1;
            memset(cb, 0, sizeof(*cb));
            return 0;
        }
        case 0x0305:
            dpmi_low(&f->tf.eax, 0);
            dpmi_low(&f->tf.ebx, 0xF000); dpmi_low(&f->tf.ecx, DPMI_RM_SAVE);
            dpmi_low(&f->tf.esi, DPMI_HOST_CS); put_offset(v, &f->tf.edi, DPMI_PM_SAVE);
            return 0;
        case 0x0306:
            dpmi_low(&f->tf.ebx, 0xF000); dpmi_low(&f->tf.ecx, DPMI_RM_RAW);
            dpmi_low(&f->tf.esi, DPMI_HOST_CS); put_offset(v, &f->tf.edi, DPMI_PM_RAW);
            return 0;
        default: return 1;
        }
    case 0x04:
        if (ax != 0x0400) return 1;     /* 0401h is DPMI 1.0 */
        dpmi_low(&f->tf.eax, 0x005A);   /* 0.90: AH=0, AL=90 decimal */
        dpmi_low(&f->tf.ebx, 1);        /* 386 host; V86 reflection; no virtual memory */
        f->tf.ecx = (f->tf.ecx & ~0xFFu) | arch_cpu_family();
        dpmi_low(&f->tf.edx, ((uint16_t)v->monitor.pic[0].base << 8) | v->monitor.pic[1].base);
        return 0;
    case 0x05: return ax <= 0x0503 ? memory_service(v, f, ax) : 1;
    case 0x06: return ax <= 0x0604 ? paging_service(v, f, ax) : 1;
    case 0x07: return ax == 0x0702 || ax == 0x0703 ? paging_service(v, f, ax) : 1;
    case 0x08: return dosvm_vbe_map(v,f,ax); /* only the private VBE token */
    case 0x09: {
        if (ax > 0x0902) return 1;
        bool old = v->monitor.vif;
        if (ax == 0x0900) {
            v->monitor.vif = false;
            d->if_waiting = false;
            d->if_steps = 0;
        }
        else if (ax == 0x0901) dosvm_vif_restore(v, true, DOSVM_IF_API);
        f->tf.eax = (f->tf.eax & ~0xFFu) | (old ? 1u : 0u);
        return 0;
    }
    case 0x0A: return 1;                /* no vendor extension */
    case 0x0B: return ax <= 0x0B03 ? watch_service(v, f, ax) : 1;
    default: return 1;                  /* unknown and 1.0-only: CF, no state change */
    }
}

int dpmi_int31(struct dosvm *v, struct v86_frame *f)
{
    unsigned ax = (uint16_t)f->tf.eax;
    int r = service(v, f, ax);
    if (r < 0) return r;
    if (r == 2) {                       /* completion sets CF and marks DOS rows */
        if (ax > 0x0102) mark(v, ax, true);
        return 0;
    }
    dpmi_carry(f, r != 0);
    mark(v, ax, r == 0);
    return 0;
}

/* ---- V86 monitor hooks ---- */
static int termcheck(struct dosvm *v, struct v86_frame *f, uint8_t vector)
{
    struct v86_frame regs;
    memset(&regs, 0, sizeof(regs));
    regs.tf.eax = 0x6200;
    if (dpmi_hostcall(v, f, DPMI_OP_TERMCHECK, &regs)) return -ENOSYS;
    v->dpmi.rec[v->dpmi.depth - 1].vector = vector;
    return 0;
}
int dpmi_v86_int(struct dosvm *v, struct v86_frame *f, uint8_t vector)
{
    struct dpmi *d = &v->dpmi;
    uint16_t ax = (uint16_t)f->tf.eax;
    if (vector == 0x2F && ax == 0x1687 && (v->dpmi_enabled || DOSVM_DPMI_PUBLIC)) {
        /* 0.9 section 5.1; version 0.90 as DH=0, DL=90 decimal. */
        dpmi_low(&f->tf.eax, 0);
        dpmi_low(&f->tf.ebx, 1);
        f->tf.ecx = (f->tf.ecx & ~0xFFu) | arch_cpu_family();
        dpmi_low(&f->tf.edx, 0x005A);
        dpmi_low(&f->tf.esi, 0);
        f->es = 0xF000;
        dpmi_low(&f->tf.edi, DPMI_RM_ENTRY);
        return 0;
    }
    if (!d->clients) return -ENOSYS;
    if ((vector == 0x21 && ((ax >> 8) == 0x4C || (ax >> 8) == 0x00 || (ax >> 8) == 0x31)) ||
        vector == 0x20 || vector == 0x27)
        return termcheck(v, f, vector);
    if ((vector == 0x1C || vector == 0x23 || vector == 0x24) &&
        !(d->vec[vector].sel == DPMI_HOST_CS && d->vec[vector].off < 0x100))
        return dpmi_enter_pm_handler(v, f, vector, vector == 0x1C ? 0 : DPMI_RF_RESULTS);
    return -ENOSYS;
}
int dpmi_v86_stub(struct dosvm *v, struct v86_frame *f)
{
    uint32_t at = f->tf.eip - 2;
    if (f->tf.cs != 0xF000 || at < DPMI_RM_ENTRY || at >= DPMI_RM_END || (at & 3)) return -ENOSYS;
    if (at == DPMI_RM_ENTRY) return dpmi_entry(v, f);
    if (!v->dpmi.active) return -V86_EPERM;
    if (at == DPMI_RM_RETURN) return dpmi_v86_return(v, f);
    if (at == DPMI_RM_RAW) return dpmi_raw_to_pm(v, f);
    if (at == DPMI_RM_SAVE) return dpmi_far_return(v, f);
    if (at >= DPMI_RM_CALLBACK) return dpmi_callback_enter(v, f, (at - DPMI_RM_CALLBACK) / 4);
    return -V86_EPERM;
}

void dpmi_info_fill(struct dosvm *v, struct dpmi_info *out)
{
    struct dpmi *d = &v->dpmi;
    memset(out, 0, sizeof(*out));
    out->active = d->active;
    out->bits32 = d->bits32;
    out->clients = d->clients;
    out->depth = d->depth;
    out->stats = d->stats;
    out->hstack = d->hstack.used;
    out->pool_used = v->info.pool_used;
    for (unsigned i = 0; i < DPMI_LDT_ENTRIES; i++)
        out->selectors += d->kind[i] != DPMI_FREE && d->kind[i] != DPMI_HOST;
    for (unsigned i = 0; i < DPMI_BLOCKS; i++) out->blocks += d->block[i].b.used;
    for (unsigned i = 0; i < DPMI_DOS_BLOCKS; i++) out->dos_blocks += d->dos[i].count != 0;
    for (unsigned c = 0; c < DPMI_CLIENTS; c++)
        for (unsigned k = 0; k < DPMI_CALLBACKS; k++) out->callbacks += d->client[c].cb[k].used;
    for (unsigned i = 0; i < DPMI_WATCH; i++) out->watchpoints += d->watch[i].used;
}
