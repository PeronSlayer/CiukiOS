/* f3-04 kernel DPMI host against the fake VM: every trap goes through the
 * production dosvm_trap/v86_emulate/dpmi paths with synthetic CPU frames.
 * Guest instructions that trap are written as real bytes at CS:EIP; the
 * test plays the CPU for instructions that do not trap, and plays DOS at
 * a fake INT 21h entry (IRET at 0050:0000) for host-issued calls.
 * Included by dosvm_test.c. SPDX-License-Identifier: GPL-2.0-only */
#define DOS_SEG 0x0050u
#define IRET_SEG 0x0051u      /* generic real-mode IRET handler */
#define RETF_SEG 0x0052u      /* real-mode RETF procedure */
#define PSP_SEG 0x1000u
#define ENV_SEG 0x0F00u
#define CODE_SEG 0x1100u
#define STACK_SEG 0x1200u
#define DATA_SEG 0x1300u
static struct v86_frame cur;
static bool left;
static unsigned dos_calls, dos_exits;
static uint16_t dos_next, dos_blocks[8][2];
static bool dos_fail;

static void step(struct dosvm *v, unsigned vector, uint32_t err)
{
    struct v86_frame hw = cur;
    hw.tf.vector = vector; hw.tf.err = err;
    g_current = v->task; v->continuation = 0x12345678; cpu_flags = 0;
    left = false;
    if (!setjmp(guest_return)) { CHECK(dosvm_trap(&hw.tf)); cur = hw; }
    else { left = true; cur = v->frame; }
    g_current = &caller; sti();
    cur.tf.vector = cur.tf.err = 0;
}
static bool in_pm(void) { return !(cur.tf.eflags & V86_VM); }
static uint32_t seg_base(struct dosvm *v, uint16_t sel)
{ struct dpmi_seg s; CHECK(dpmi_seg(v, sel, &s)); return s.base; }
static uint32_t cs_linear(struct dosvm *v)
{
    if (!in_pm()) return ((cur.tf.cs & 0xFFFFu) << 4) + (uint16_t)cur.tf.eip;
    return seg_base(v, (uint16_t)cur.tf.cs) + cur.tf.eip;
}
static void exec(struct dosvm *v, const uint8_t *code, unsigned n, unsigned vector, uint32_t err)
{ CHECK(!dosvm_write(v, cs_linear(v), code, n)); step(v, vector, err); }
static void op1(struct dosvm *v, uint8_t op) { exec(v, &op, 1, 13, 0); }
static void pm_int(struct dosvm *v, uint8_t n) { uint8_t c[2] = {0xCD, n}; exec(v, c, 2, 13, (uint32_t)n << 3 | 2); }
static void v86_int(struct dosvm *v, uint8_t n) { uint8_t c[2] = {0xCD, n}; exec(v, c, 2, 13, 0); }
/* V86 INT n reflected to a synthetic ROM stub: INT FEh service, then IRET. */
static void rom_int(struct dosvm *v, uint8_t n)
{
    v86_int(v, n);
    CHECK(cur.tf.cs == 0xF000 && cur.tf.eip == (uint32_t)n * 16);
    step(v, 13, 0); step(v, 13, 0);
}
static void int31(struct dosvm *v, uint16_t ax) { cur.tf.eax = (cur.tf.eax & 0xFFFF0000u) | ax; pm_int(v, 0x31); }
static bool cf(void) { return cur.tf.eflags & V86_CF; }
static uint16_t r16(uint32_t r) { return (uint16_t)r; }
static void rm_push16(struct dosvm *v, uint16_t value)
{
    uint16_t sp = (uint16_t)(cur.tf.user_esp - 2);
    CHECK(!dosvm_write(v, ((cur.tf.user_ss & 0xFFFFu) << 4) + sp, &value, 2));
    cur.tf.user_esp = (cur.tf.user_esp & 0xFFFF0000u) | sp;
}
static uint16_t rm_pop16(struct dosvm *v)
{
    uint16_t value, sp = (uint16_t)cur.tf.user_esp;
    CHECK(!dosvm_read(v, ((cur.tf.user_ss & 0xFFFFu) << 4) + sp, &value, 2));
    cur.tf.user_esp = (cur.tf.user_esp & 0xFFFF0000u) | (uint16_t)(sp + 2);
    return value;
}
static uint32_t pm_pop(struct dosvm *v)
{ uint32_t x; CHECK(dpmi_pop(v, &cur, &x, dpmi_width(v))); return x; }
static void pm_push(struct dosvm *v, uint32_t x) { CHECK(dpmi_push(v, &cur, x, dpmi_width(v))); }
/* Client executes IRET (ring 3: IF not restored) or RETF. */
static void pm_iret(struct dosvm *v)
{
    uint32_t ip = pm_pop(v), cs = pm_pop(v), fl = pm_pop(v);
    cur.tf.eip = ip; cur.tf.cs = cs;
    cur.tf.eflags = (fl & 0x0DD5u) | V86_IF | 2u;
}
static void pm_retf(struct dosvm *v) { uint32_t ip = pm_pop(v), cs = pm_pop(v); cur.tf.eip = ip; cur.tf.cs = cs; }
/* Host stubs are HLT bytes: the CPU raises #GP(0) at them. */
static void at_stub(struct dosvm *v, uint32_t off)
{ CHECK(in_pm() && r16(cur.tf.cs) == DPMI_HOST_CS && cur.tf.eip == off); step(v, 13, 0); }

/* Fake DOS for host-issued and client real-mode INT 21h. */
static void dos_service(struct dosvm *v)
{
    CHECK(!in_pm() && cur.tf.cs == DOS_SEG && !cur.tf.eip);
    uint8_t ah = (uint8_t)(cur.tf.eax >> 8);
    bool carry = false;
    dos_calls++;
    switch (ah) {
    case 0x62: cur.tf.ebx = (cur.tf.ebx & ~0xFFFFu) | PSP_SEG; break;
    case 0x48: {
        uint16_t paras = (uint16_t)cur.tf.ebx;
        if (dos_fail || paras > 0x4000) { carry = true; cur.tf.eax = (cur.tf.eax & ~0xFFFFu) | 8; cur.tf.ebx = (cur.tf.ebx & ~0xFFFFu) | 0x4000; break; }
        unsigned i; for (i = 0; i < 8 && dos_blocks[i][1]; i++) {}
        CHECK(i < 8);
        dos_blocks[i][0] = dos_next; dos_blocks[i][1] = paras ? paras : 1;
        cur.tf.eax = (cur.tf.eax & ~0xFFFFu) | dos_next;
        dos_next += 0x2000;
        break;
    }
    case 0x49: case 0x4A: {
        unsigned i; for (i = 0; i < 8 && !(dos_blocks[i][1] && dos_blocks[i][0] == (uint16_t)cur.es); i++) {}
        if (i == 8) { carry = true; cur.tf.eax = (cur.tf.eax & ~0xFFFFu) | 9; break; }
        if (ah == 0x49) { dos_blocks[i][1] = 0; break; }
        if ((uint16_t)cur.tf.ebx > 0x2000) { carry = true; cur.tf.eax = (cur.tf.eax & ~0xFFFFu) | 8; cur.tf.ebx = (cur.tf.ebx & ~0xFFFFu) | 0x2000; break; }
        dos_blocks[i][1] = (uint16_t)cur.tf.ebx;
        break;
    }
    case 0x4C: case 0x00: case 0x31: dos_exits++; return;  /* never returns to the caller */
    default: carry = true; break;
    }
    /* DOS returns CF in the stacked FLAGS, then IRET (emulated by the monitor). */
    uint32_t flags_at = ((cur.tf.user_ss & 0xFFFFu) << 4) + (uint16_t)(cur.tf.user_esp + 4);
    uint16_t image; CHECK(!dosvm_read(v, flags_at, &image, 2));
    image = (uint16_t)((image & ~1u) | carry);
    CHECK(!dosvm_write(v, flags_at, &image, 2));
    step(v, 13, 0);                     /* IRET at 0050:0000 */
}
/* DOS call started by the host: run DOS, the IRET, then the return stub. */
static void dos_roundtrip(struct dosvm *v)
{
    dos_service(v);
    CHECK(!in_pm() && cur.tf.cs == 0xF000 && cur.tf.eip == DPMI_RM_RETURN);
    step(v, 13, 0);                     /* INT FEh at F000:1044 */
}
/* DOS returns from a call the client made through 0300h (fake DOS IRET). */
static void rm_iret_roundtrip_dos(struct dosvm *v)
{
    CHECK(!in_pm() && cur.tf.cs == DOS_SEG && !cur.tf.eip);
    step(v, 13, 0);
    CHECK(cur.tf.cs == 0xF000 && cur.tf.eip == DPMI_RM_RETURN);
    step(v, 13, 0);
}
/* Real-mode routine reached by a host transition returns through IRET. */
static void rm_iret_roundtrip(struct dosvm *v)
{
    CHECK(!in_pm() && cur.tf.cs == IRET_SEG && !cur.tf.eip);
    step(v, 13, 0);
    CHECK(cur.tf.cs == 0xF000 && cur.tf.eip == DPMI_RM_RETURN);
    step(v, 13, 0);
}

static struct dosvm *dpmi_vm(bool enabled)
{
    struct dosvm *v;
    struct dosvm_config c = {.commitment = DOSVM_LOW_CAP + 4u * 1024 * 1024, .dpmi = enabled};
    CHECK(dosvm_create(&c, &v) == 0 && v);
    v->info.dos_ready = true; v->info.state = DOSVM_RUNNABLE;
    v->monitor.state = V86_RUNNING; v->monitor.vif = true;
    uint16_t ivt[2] = {0, DOS_SEG};
    CHECK(!dosvm_write(v, 0x21 * 4, ivt, 4));
    for (unsigned n = 0x60; n < 0x70; n++) { ivt[1] = IRET_SEG; CHECK(!dosvm_write(v, n * 4, ivt, 4)); }
    uint8_t iret = 0xCF, retf = 0xCB;
    CHECK(!dosvm_write(v, DOS_SEG << 4, &iret, 1) && !dosvm_write(v, IRET_SEG << 4, &iret, 1) &&
          !dosvm_write(v, RETF_SEG << 4, &retf, 1));
    uint16_t env = ENV_SEG;
    CHECK(!dosvm_write(v, (PSP_SEG << 4) + 0x2C, &env, 2));
    uint8_t mcb[5] = {'M', 0x00, 0x10, 0x08, 0x00};      /* owner 1000h, 8 paragraphs */
    CHECK(!dosvm_write(v, (ENV_SEG - 1) << 4, mcb, 5));
    memset(dos_blocks, 0, sizeof(dos_blocks)); dos_next = 0x6000; dos_fail = false;
    return v;
}
static void v86_start(void)
{
    memset(&cur, 0, sizeof(cur));
    cur.tf.cs = CODE_SEG; cur.tf.eip = 0x100;
    cur.tf.user_ss = STACK_SEG; cur.tf.user_esp = 0x0800;
    cur.ds = DATA_SEG; cur.es = PSP_SEG;
    cur.tf.eflags = V86_VM | V86_IF | 2;
}
/* 1687h then the far call to the entry; returns with CF of the entry. */
static void enter(struct dosvm *v, bool bits32)
{
    cur.tf.eax = 0x1687; v86_int(v, 0x2F);
    CHECK(r16(cur.tf.eax) == 0 && (cur.tf.ebx & 1) && r16(cur.tf.edx) == 0x005A && r16(cur.tf.esi) == 0);
    CHECK((uint8_t)cur.tf.ecx == 6 && cur.es == 0xF000 && r16(cur.tf.edi) == DPMI_RM_ENTRY);
    uint16_t ret_ip = (uint16_t)(cur.tf.eip + 3);
    rm_push16(v, (uint16_t)cur.tf.cs); rm_push16(v, ret_ip);
    cur.tf.cs = 0xF000; cur.tf.eip = DPMI_RM_ENTRY; cur.tf.eax = bits32;
    unsigned before = dos_calls;
    step(v, 13, 0);
    if (!in_pm() && cur.tf.cs == DOS_SEG) { dos_roundtrip(v); CHECK(dos_calls == before + 1); }
    if (!in_pm()) CHECK(cur.tf.eip == ret_ip && cf());
}

static void test_dpmi_discovery_entry(void)
{
    /* Unadvertised VM: CiukiDOS's own answer chain (here the ROM stub). */
    struct dosvm *v = dpmi_vm(false);
    v86_start(); cur.tf.eax = 0x1687; rom_int(v, 0x2F);
    CHECK(r16(cur.tf.eax) == 0xFFFF && !v->dpmi.active && cur.tf.eip == 0x102);
    /* A guessed entry address fails cleanly: CF, back at the caller. */
    rm_push16(v, CODE_SEG); rm_push16(v, 0x200); cur.tf.cs = 0xF000; cur.tf.eip = DPMI_RM_ENTRY;
    step(v, 13, 0);
    CHECK(!in_pm() && cf() && cur.tf.cs == CODE_SEG && cur.tf.eip == 0x200 && !v->dpmi.active);
    cur.tf.eax = 0x1686; rom_int(v, 0x2F); CHECK(r16(cur.tf.eax) != 0);
    destroy(v, true); empty();

    v = dpmi_vm(true);
    v86_start();
    CHECK(!dosvm_set_a20(v, false));    /* V86 A20 off: PM must still see it on */
    enter(v, false);
    struct dpmi *d = &v->dpmi;
    CHECK(in_pm() && !cf() && d->active && d->clients == 1 && !d->bits32 && !d->depth);
    CHECK(ldt_loaded == d->ldt && ldt_entries == DPMI_LDT_ENTRIES && v->a20 && !d->rm_a20);
    struct dpmi_seg s;
    CHECK(dpmi_seg(v, r16(cur.tf.cs), &s) && s.code && s.base == CODE_SEG << 4 && s.limit == 0xFFFF && !s.big && s.dpl == 3);
    CHECK(cur.tf.eip == 0x105);
    CHECK(dpmi_seg(v, r16(cur.tf.ds), &s) && !s.code && s.base == DATA_SEG << 4 && s.limit == 0xFFFF);
    CHECK(dpmi_seg(v, r16(cur.tf.user_ss), &s) && s.writable && s.base == STACK_SEG << 4 && cur.tf.user_esp == 0x800);
    CHECK(dpmi_seg(v, r16(cur.tf.es), &s) && s.base == PSP_SEG << 4 && s.limit == 0xFF);
    CHECK(!cur.tf.fs && !cur.tf.gs && (cur.tf.eflags & V86_IF) && !(cur.tf.eflags & 0x3000));
    uint16_t env_sel; CHECK(!dosvm_read(v, (PSP_SEG << 4) + 0x2C, &env_sel, 2));
    CHECK(dpmi_seg(v, env_sel, &s) && s.base == ENV_SEG << 4 && s.limit == 0x7F && d->kind[env_sel >> 3] == DPMI_FIXED);
    CHECK(d->client[0].psp == PSP_SEG && d->client[0].env == ENV_SEG);
    /* 1686h in protected mode answers 0 (default PM handler). */
    cur.tf.eax = 0x1686; pm_int(v, 0x2F); CHECK(in_pm() && r16(cur.tf.eax) == 0);
    /* Host selectors: the client cannot free, modify or 000Dh them. */
    cur.tf.ebx = DPMI_HOST_CS; int31(v, 0x0001); CHECK(cf() && r16(cur.tf.eax) == 0x0001);
    cur.tf.ebx = DPMI_HOST_SS; cur.tf.ecx = 0; cur.tf.edx = 0; int31(v, 0x0007); CHECK(cf());
    cur.tf.ebx = env_sel; int31(v, 0x0001); CHECK(cf());   /* PSP/environment descriptors are fixed */
    /* Opposite-width nested entry fails without a partial transition. */
    struct dpmi before = *d;
    struct v86_frame pm = cur;
    v86_start(); cur.tf.cs = 0x2100; cur.tf.eip = 0x40; cur.tf.user_esp = 0x700;
    enter(v, true);
    CHECK(!in_pm() && cf() && d->clients == 1 && d->stats.entry_failures == before.stats.entry_failures + 1);
    CHECK(!memcmp(d->ldt, before.ldt, sizeof(d->ldt)) && !memcmp(d->kind, before.kind, sizeof(d->kind)));
    cur = pm; v->frame_pm = true;
    destroy(v, true); empty();
    CHECK(!d->active);
    printf("DPMI discovery/entry: 1687h gated per VM, 16-bit entry selectors, A20 on, width conflict refused\n");
}

static void test_dpmi_ldt(void)
{
    struct dosvm *v = dpmi_vm(true);
    struct dpmi *d = &v->dpmi;
    v86_start(); enter(v, false);
    CHECK(in_pm());
    uint32_t buf = (DATA_SEG << 4) + 0x100;
    cur.tf.ecx = 3; int31(v, 0x0000); CHECK(!cf());
    uint16_t sel = r16(cur.tf.eax);
    CHECK((sel & 7) == 7 && (sel >> 3) >= DPMI_SPECIFIC && d->kind[sel >> 3] == DPMI_CLIENT && d->kind[(sel >> 3) + 2] == DPMI_CLIENT);
    struct dpmi_seg s;
    CHECK(dpmi_seg(v, sel, &s) && s.present && !s.code && !s.base && !s.limit && s.dpl == 3);
    cur.tf.ecx = 0; int31(v, 0x0000); CHECK(cf() && r16(cur.tf.eax) == 0);
    cur.tf.ecx = 2000; int31(v, 0x0000); CHECK(cf());
    int31(v, 0x0003); CHECK(!cf() && r16(cur.tf.eax) == 8);
    int31(v, 0x0004); CHECK(cf() && r16(cur.tf.eax) == 4);
    int31(v, 0x0005); CHECK(cf() && r16(cur.tf.eax) == 5);
    /* base/limit/rights */
    cur.tf.ebx = sel; cur.tf.ecx = 0x0012; cur.tf.edx = 0x3456; int31(v, 0x0007); CHECK(!cf());
    cur.tf.ebx = sel; int31(v, 0x0006); CHECK(!cf() && r16(cur.tf.ecx) == 0x0012 && r16(cur.tf.edx) == 0x3456);
    cur.tf.ebx = sel; cur.tf.ecx = 0x0010; cur.tf.edx = 0x0000; int31(v, 0x0008); CHECK(cf());   /* > 1 MiB, not page aligned */
    cur.tf.ebx = sel; cur.tf.ecx = 0x00FF; cur.tf.edx = 0xFFFF; int31(v, 0x0008); CHECK(!cf());
    CHECK(dpmi_seg(v, sel, &s) && s.limit == 0xFFFFFF && (d->ldt[sel >> 3][1] & (1u << 23)));
    cur.tf.ebx = sel; cur.tf.ecx = 0x0000; cur.tf.edx = 0xFFFF; int31(v, 0x0008); CHECK(!cf() && dpmi_seg(v, sel, &s) && s.limit == 0xFFFF);
    cur.tf.ebx = sel; cur.tf.ecx = 0x40FA; int31(v, 0x0009); CHECK(!cf() && dpmi_seg(v, sel, &s) && s.code && s.readable && s.big);
    cur.tf.ebx = sel; cur.tf.ecx = 0x009A; int31(v, 0x0009); CHECK(cf());   /* DPL 0 */
    cur.tf.ebx = sel; cur.tf.ecx = 0x00FE; int31(v, 0x0009); CHECK(cf());   /* conforming code */
    cur.tf.ebx = sel; cur.tf.ecx = 0x00F8; int31(v, 0x0009); CHECK(cf());   /* execute-only code */
    cur.tf.ebx = sel; cur.tf.ecx = 0x00E2; int31(v, 0x0009); CHECK(cf());   /* system descriptor */
    cur.tf.ebx = sel; cur.tf.ecx = 0x20F2; int31(v, 0x0009); CHECK(cf());   /* reserved bit 5 of CH */
    cur.tf.ebx = sel; cur.tf.ecx = 0x0070; int31(v, 0x0009); CHECK(!cf() && dpmi_seg(v, sel, &s) && !s.present); /* absent: anything */
    cur.tf.ebx = sel; cur.tf.ecx = 0x00FA; int31(v, 0x0009); CHECK(!cf());
    /* 000Ah data alias of a code descriptor; 000Ah of data fails. */
    cur.tf.ebx = sel; int31(v, 0x000A); CHECK(!cf());
    uint16_t alias = r16(cur.tf.eax);
    CHECK(dpmi_seg(v, alias, &s) && !s.code && s.writable && s.base == 0x00123456 && s.limit == 0xFFFF);
    cur.tf.ebx = alias; int31(v, 0x000A); CHECK(cf());
    /* 000Bh/000Ch round trip; invalid buffer and invalid descriptors fail. */
    cur.tf.ebx = alias; cur.tf.es = cur.tf.ds; cur.tf.edi = 0x100; int31(v, 0x000B); CHECK(!cf());
    uint32_t e[2]; CHECK(!dosvm_read(v, buf, e, 8) && e[0] == d->ldt[alias >> 3][0] && e[1] == d->ldt[alias >> 3][1]);
    e[1] = (e[1] & ~0xFF00u) | 0xF200u; e[0] = 0x00007777u;
    CHECK(!dosvm_write(v, buf, e, 8));
    cur.tf.ebx = sel + 8; int31(v, 0x000C); CHECK(!cf() && dpmi_seg(v, sel + 8, &s) && s.limit == 0x7777);
    e[1] = (e[1] & ~0xFF00u) | 0x9200u; CHECK(!dosvm_write(v, buf, e, 8));
    cur.tf.ebx = sel + 8; int31(v, 0x000C); CHECK(cf() && dpmi_seg(v, sel + 8, &s) && s.limit == 0x7777);
    cur.tf.ebx = sel; cur.tf.edi = 0xFFFC; int31(v, 0x000B); CHECK(cf());    /* crosses the DS limit */
    cur.tf.ebx = 0x0FFF; cur.tf.edi = 0x100; int31(v, 0x000B); CHECK(cf());  /* unallocated */
    /* 0002h: the same segment returns the same VM-owned selector; never freeable. */
    cur.tf.ebx = 0xB800; int31(v, 0x0002); CHECK(!cf());
    uint16_t text = r16(cur.tf.eax);
    cur.tf.ebx = 0xB800; int31(v, 0x0002); CHECK(!cf() && r16(cur.tf.eax) == text && d->kind[text >> 3] == DPMI_ALIAS && !d->owner[text >> 3]);
    CHECK(dpmi_seg(v, text, &s) && s.base == 0xB8000 && s.limit == 0xFFFF);
    cur.tf.ebx = text; int31(v, 0x0001); CHECK(cf());
    cur.tf.ebx = text; cur.tf.ecx = 0; cur.tf.edx = 0; int31(v, 0x0007); CHECK(cf());
    /* 000Dh: only free entries of the first sixteen. */
    cur.tf.ebx = 0x0024 | 7; int31(v, 0x000D); CHECK(!cf() && d->kind[4] == DPMI_CLIENT);
    cur.tf.ebx = 0x0024 | 7; int31(v, 0x000D); CHECK(cf());
    cur.tf.ebx = DPMI_SEL(DPMI_SPECIFIC); int31(v, 0x000D); CHECK(cf());
    cur.tf.ebx = 0x0028; int31(v, 0x000D); CHECK(cf());                         /* GDT selector */
    /* Freeing a selector held in a data segment register zeroes it. */
    cur.tf.es = sel + 16; cur.tf.ebx = sel + 16; int31(v, 0x0001); CHECK(!cf() && !cur.tf.es && d->kind[(sel >> 3) + 2] == DPMI_FREE);
    cur.tf.ebx = sel + 16; int31(v, 0x0001); CHECK(cf());
    cur.tf.ebx = sel; int31(v, 0x0006); CHECK(!cf());
    int31(v, 0x000E); CHECK(cf() && r16(cur.tf.eax) == 0x000E);
    int31(v, 0x000F); CHECK(cf());
    /* A data register made unusable by 0009h is zeroed on return. */
    cur.tf.fs = alias; cur.tf.ebx = alias; cur.tf.ecx = 0x0072; int31(v, 0x0009);
    CHECK(!cf() && !cur.tf.fs);
    destroy(v, true); empty();
    printf("DPMI LDT services 0000h-000Dh: allocation, rights validation, aliases, reserved/1.0 refusals\n");
}


/* Struct for 0300h-0303h at DS:0200h. */
static uint32_t rmcs(void) { return (DATA_SEG << 4) + 0x200; }
static void rmcs_set(struct dosvm *v, uint32_t eax, uint32_t ebx, uint16_t flags, uint16_t es, uint16_t cs, uint16_t ip, uint16_t ss, uint16_t sp)
{
    uint8_t s[0x32] = {0};
    vm_w32(s + 0x10, ebx); vm_w32(s + 0x1C, eax); vm_w32(s, 0x11112222u); vm_w32(s + 8, 0x33334444u);
    vm_w16(s + 0x20, flags); vm_w16(s + 0x22, es); vm_w16(s + 0x24, 0x4444); vm_w16(s + 0x2A, ip); vm_w16(s + 0x2C, cs);
    vm_w16(s + 0x2E, sp); vm_w16(s + 0x30, ss);
    CHECK(!dosvm_write(v, rmcs(), s, sizeof(s)));
}
static uint32_t rmcs32(struct dosvm *v, unsigned off) { uint32_t x = 0; CHECK(!dosvm_read(v, rmcs() + off, &x, 4)); return x; }
static uint16_t rmcs16(struct dosvm *v, unsigned off) { uint16_t x = 0; CHECK(!dosvm_read(v, rmcs() + off, &x, 2)); return x; }

static void test_dpmi_interrupts_translation(void)
{
    struct dosvm *v = dpmi_vm(true);
    struct dpmi *d = &v->dpmi;
    v86_start(); enter(v, false);
    uint16_t code = r16(cur.tf.cs), data = r16(cur.tf.ds);
    /* 0200h/0201h real vectors. */
    cur.tf.ebx = 0x61; int31(v, 0x0200); CHECK(!cf() && r16(cur.tf.ecx) == IRET_SEG && r16(cur.tf.edx) == 0);
    cur.tf.ebx = 0x62; cur.tf.ecx = RETF_SEG; cur.tf.edx = 0; int31(v, 0x0201); CHECK(!cf());
    uint16_t ivt[2]; CHECK(!dosvm_read(v, 0x62 * 4, ivt, 4) && ivt[1] == RETF_SEG);
    /* 0202h/0203h exception vectors: 00h-1Fh only, code selectors only. */
    cur.tf.ebx = 0x0D; int31(v, 0x0202); CHECK(!cf() && r16(cur.tf.ecx) == DPMI_HOST_CS && r16(cur.tf.edx) == DPMI_PM_EXC + 0x0D);
    cur.tf.ebx = 0x20; int31(v, 0x0202); CHECK(cf() && r16(cur.tf.eax) == 0x0202);
    cur.tf.ebx = 0x0D; cur.tf.ecx = data; cur.tf.edx = 0x10; int31(v, 0x0203); CHECK(cf());       /* data selector */
    cur.tf.ebx = 0x0D; cur.tf.ecx = code; cur.tf.edx = 0x400; int31(v, 0x0203); CHECK(!cf() && d->exc[0x0D].off == 0x400);
    cur.tf.ebx = 0x0D; cur.tf.ecx = DPMI_HOST_CS; cur.tf.edx = DPMI_PM_EXC + 0x0D; int31(v, 0x0203); CHECK(!cf());
    cur.tf.ebx = 0x0D; cur.tf.ecx = DPMI_HOST_CS; cur.tf.edx = DPMI_PM_IRQRET; int31(v, 0x0203); CHECK(cf());
    /* 0204h/0205h protected vectors, then a hooked software interrupt. */
    cur.tf.ebx = 0x21; int31(v, 0x0204); CHECK(!cf() && r16(cur.tf.ecx) == DPMI_HOST_CS && r16(cur.tf.edx) == 0x21);
    cur.tf.ebx = 0x66; cur.tf.ecx = code; cur.tf.edx = 0x500; int31(v, 0x0205); CHECK(!cf());
    cur.tf.ebx = 0x66; cur.tf.ecx = code; cur.tf.edx = 0x10000; int31(v, 0x0205); CHECK(!cf() && d->vec[0x66].off == 0); /* 16-bit: DX */
    cur.tf.ebx = 0x66; cur.tf.ecx = code; cur.tf.edx = 0x500; int31(v, 0x0205); CHECK(!cf());
    int31(v, 0x0210); CHECK(cf()); int31(v, 0x0213); CHECK(cf());
    struct v86_frame before = cur;
    pm_int(v, 0x66);
    CHECK(in_pm() && r16(cur.tf.cs) == code && cur.tf.eip == 0x500 && cur.tf.user_esp == before.tf.user_esp - 6 && v->monitor.vif);
    pm_iret(v);
    CHECK(cur.tf.eip == before.tf.eip + 2 && cur.tf.user_esp == before.tf.user_esp);
    /* INT 0-7 run with virtual IF clear. */
    cur.tf.ebx = 0x05; cur.tf.ecx = code; cur.tf.edx = 0x520; int31(v, 0x0205);
    pm_int(v, 0x05); CHECK(cur.tf.eip == 0x520 && !v->monitor.vif);
    pm_iret(v); op1(v, 0xFB);
    /* STI shadow without TF (TF would leak into a PUSHF image): an IRQ taken
     * before the next instruction keeps it, any later trap ends it. */
    CHECK(v->monitor.vif && d->shadow && !(cur.tf.eflags & V86_TF) && d->shadow_eip == cur.tf.eip);
    step(v, 0x20, 0); CHECK(d->shadow);
    cur.tf.eip += 1; step(v, 0x20, 0); CHECK(!d->shadow);
    /* Default reflection: registers cross, segment registers do not. */
    cur.tf.eax = 0x1234; cur.tf.ebx = 0x5678; cur.tf.ecx = 0x9ABC; before = cur;
    pm_int(v, 0x61);
    CHECK(!in_pm() && left && cur.tf.cs == IRET_SEG && r16(cur.tf.eax) == 0x1234 && r16(cur.tf.ebx) == 0x5678 && !cur.ds && !cur.es);
    CHECK(cur.tf.user_ss == DPMI_RSTACK_SEG && r16(cur.tf.user_esp) == DPMI_RSTACK_BYTES - 6 && !v->monitor.vif && d->depth == 1);
    cur.tf.eax = 0xAAAA; cur.tf.eflags |= V86_ZF;
    rm_iret_roundtrip(v);
    CHECK(in_pm() && left && r16(cur.tf.eax) == 0xAAAA && cur.tf.eip == before.tf.eip + 2 && r16(cur.tf.ds) == data && !d->depth);
    CHECK(d->rstack_top == DPMI_RSTACK_BYTES && v->monitor.vif);
    /* INT 80h from a client is the guest's vector 80h, never a syscall. */
    cur.tf.ebx = 0x80; cur.tf.ecx = code; cur.tf.edx = 0x540; int31(v, 0x0205);
    before = cur; cur.tf.eip += 2; step(v, 0x80, 0);
    CHECK(in_pm() && cur.tf.eip == 0x540 && cur.tf.user_esp == before.tf.user_esp - 6);
    pm_iret(v);
    /* 0300h: IVT[BL] with the structure's registers and CX stack words. */
    pm_push(v, 0xBEEF); pm_push(v, 0xCAFE);
    rmcs_set(v, 0x00004100, 0x0000ABCD, 0x0202, 0x5555, 0, 0, 0, 0);
    cur.tf.ebx = 0x61; cur.tf.ecx = 2; cur.tf.es = data; cur.tf.edi = 0x200; cur.tf.eax = 0x0300; before = cur;
    pm_int(v, 0x31);
    CHECK(!in_pm() && left && cur.tf.cs == IRET_SEG && r16(cur.tf.eax) == 0x4100 && r16(cur.tf.ebx) == 0xABCD && cur.es == 0x5555 && cur.ds == 0x4444);
    CHECK(cur.tf.edi == 0x11112222u && cur.tf.ebp == 0x33334444u && !v->monitor.vif && d->stats.realcalls == 1);
    uint16_t words[5];
    CHECK(!dosvm_read(v, (DPMI_RSTACK_SEG << 4) + r16(cur.tf.user_esp), words, sizeof(words)));
    CHECK(words[0] == DPMI_RM_RETURN && words[1] == 0xF000 && (words[2] & 0x0FD5) == 0x0200 && words[3] == 0xCAFE && words[4] == 0xBEEF);
    cur.tf.eax = 0x0042; cur.tf.ecx = 0x77; cur.es = 0x6666; cur.tf.eflags |= V86_CF;
    step(v, 13, 0);                     /* IRET: the handler's stacked flags come back */
    CHECK(cur.tf.cs == 0xF000 && cur.tf.eip == DPMI_RM_RETURN); step(v, 13, 0);
    CHECK(in_pm() && left && !cf() && cur.tf.eip == before.tf.eip + 2 && r16(cur.tf.ecx) == 2 && cur.tf.user_esp == before.tf.user_esp);
    CHECK(rmcs32(v, 0x1C) == 0x0042 && rmcs32(v, 0x18) == 0x77 && rmcs16(v, 0x22) == 0x6666 && rmcs16(v, 0x2C) == 0 && rmcs16(v, 0x2E) == 0);
    /* 0301h: far call with a client real-mode stack; 0302h: IRET frame. */
    rmcs_set(v, 0x1111, 0, 0x0002, 0, RETF_SEG, 0, STACK_SEG, 0x0400);
    cur.tf.ecx = 0; before = cur; int31(v, 0x0301);
    CHECK(!in_pm() && cur.tf.cs == RETF_SEG && cur.tf.user_ss == STACK_SEG && r16(cur.tf.user_esp) == 0x03FC);
    cur.tf.eip = rm_pop16(v); cur.tf.cs = rm_pop16(v);   /* RETF does not trap in V86: play it */
    CHECK(cur.tf.cs == 0xF000 && cur.tf.eip == DPMI_RM_RETURN); step(v, 13, 0);
    CHECK(in_pm() && !cf() && rmcs32(v, 0x1C) == 0x1111);
    rmcs_set(v, 0x2222, 0, 0x0202, 0, IRET_SEG, 0, 0, 0);
    int31(v, 0x0302);
    CHECK(!in_pm() && cur.tf.cs == IRET_SEG && !v->monitor.vif);
    rm_iret_roundtrip(v); CHECK(in_pm() && !cf() && rmcs32(v, 0x1C) == 0x2222);
    /* Invalid inputs: structure outside DS, too many stack words, bad CS:IP. */
    unsigned calls = d->stats.realcalls;
    cur.tf.edi = 0xFFF0; int31(v, 0x0300); CHECK(in_pm() && cf());
    cur.tf.edi = 0x200; cur.tf.ecx = 0x8000; int31(v, 0x0301); CHECK(in_pm() && cf());
    rmcs_set(v, 0, 0, 0, 0, RETF_SEG, 0, STACK_SEG, 0x0002); cur.tf.ecx = 0;  /* no room for the frame */
    int31(v, 0x0301); CHECK(in_pm() && cf());
    CHECK(d->stats.realcalls == calls && !d->depth);
    destroy(v, true); empty();
    printf("DPMI vectors, software interrupts and translation 0200h-0205h/0300h-0302h: frames, reflection, structures\n");
}

static void test_dpmi_termination(void)
{
    struct dosvm *v = dpmi_vm(true);
    struct dpmi *d = &v->dpmi;
    v86_start(); enter(v, false);
    uint16_t code = r16(cur.tf.cs);
    uint32_t pool = v->info.pool_used;
    CHECK(pool == DPMI_HSTACK_BYTES);
    cur.tf.ebx = 0; cur.tf.ecx = 0x3000; int31(v, 0x0501); CHECK(!cf());
    cur.tf.ecx = 4; int31(v, 0x0000); CHECK(!cf());
    cur.tf.ebx = 0x66; cur.tf.ecx = code; cur.tf.edx = 0x500; int31(v, 0x0205);
    cur.tf.ds = code; cur.tf.esi = 0x600; cur.tf.es = cur.tf.user_ss; cur.tf.edi = 0x10; int31(v, 0x0303); CHECK(!cf());
    uint16_t cb_off = r16(cur.tf.edx);
    CHECK(v->info.pool_used == pool + 0x3000 && d->client[0].cb[0].used);
    /* INT 21h/4Ch in protected mode reaches the single cleanup path, then DOS. */
    cur.tf.eax = 0x4C07; pm_int(v, 0x21);
    CHECK(!in_pm() && left && cur.tf.cs == DOS_SEG && r16(cur.tf.eax) == 0x4C07);
    CHECK(!d->active && !d->clients && v->info.pool_used == 0 && ldt_loaded == 0 && d->stats.terminations == 1);
    uint16_t env; CHECK(!dosvm_read(v, (PSP_SEG << 4) + 0x2C, &env, 2) && env == ENV_SEG);
    dos_service(v); CHECK(dos_exits == 1);
    /* The freed callback is stale: invoking it terminates the VM. */
    v86_start(); rm_push16(v, CODE_SEG); rm_push16(v, 0x300); cur.tf.cs = 0xF000; cur.tf.eip = cb_off;
    step(v, 13, 0);
    CHECK(v->info.state == DOSVM_STOPPING && v->info.reason == DOSVM_FAULT && left);
    destroy(v, true); empty();

    /* V86 4Ch with a client alive: the PSP decides. A child program (other
     * PSP) leaves the client; the client's own PSP terminates it. */
    v = dpmi_vm(true); d = &v->dpmi;
    v86_start(); enter(v, false);
    struct v86_frame pm = cur;
    cur.tf.eflags |= 0; rmcs_set(v, 0x4B00, 0, 0x0202, 0, 0, 0, 0, 0);
    cur.tf.ebx = 0x21; cur.tf.ecx = 0; cur.tf.es = pm.tf.ds; cur.tf.edi = 0x200;
    int31(v, 0x0300);                   /* EXEC through 0300h: client suspended */
    CHECK(!in_pm() && cur.tf.cs == DOS_SEG && d->depth == 1);
    struct v86_frame child; memset(&child, 0, sizeof(child));
    child.tf.cs = 0x7000; child.tf.eip = 0x100; child.tf.user_ss = 0x7000; child.tf.user_esp = 0xFFFE;
    child.tf.eflags = V86_VM | V86_IF | 2;
    struct v86_frame dos_frame = cur;
    cur = child; cur.tf.eax = 0x4C00;
    v86_int(v, 0x21);                   /* child's 4Ch: host asks DOS for the PSP first */
    CHECK(!in_pm() && cur.tf.cs == DOS_SEG && d->depth == 2 && r16(cur.tf.eax) == 0x6200);
    /* The fake DOS answers the client's PSP only for the client: the
     * current PSP is now the child's. */
    cur.tf.ebx = 0x7000 - 0x10;
    {
        uint32_t flags_at = ((cur.tf.user_ss & 0xFFFFu) << 4) + (uint16_t)(cur.tf.user_esp + 4);
        uint16_t image; CHECK(!dosvm_read(v, flags_at, &image, 2)); image &= ~1u; CHECK(!dosvm_write(v, flags_at, &image, 2));
        step(v, 13, 0); step(v, 13, 0);
    }
    CHECK(!in_pm() && cur.tf.cs == DOS_SEG && r16(cur.tf.eax) == 0x4C00 && d->clients == 1 && d->depth == 1);
    cur = dos_frame;                    /* DOS returns from EXEC to the client */
    rm_iret_roundtrip_dos(v);
    CHECK(in_pm() && !cf() && d->clients == 1 && !d->depth);
    /* Now the client calls 4Ch in real mode via a raw switch; its PSP matches. */
    int31(v, 0x0306); CHECK(!cf());
    uint16_t rawcs = r16(cur.tf.esi); uint32_t rawip = cur.tf.edi;
    cur.tf.eax = DATA_SEG; cur.tf.ecx = PSP_SEG; cur.tf.edx = STACK_SEG; cur.tf.ebx = 0x700; cur.tf.esi = CODE_SEG; cur.tf.edi = 0x180;
    cur.tf.cs = rawcs; cur.tf.eip = rawip; cur.tf.ebp = 0x5150;
    step(v, 13, 0);
    CHECK(!in_pm() && left && cur.tf.cs == CODE_SEG && cur.tf.eip == 0x180 && cur.ds == DATA_SEG && cur.es == PSP_SEG && cur.tf.ebp == 0x5150 && !cur.fs);
    cur.tf.eax = 0x4C01; v86_int(v, 0x21);
    CHECK(cur.tf.cs == DOS_SEG && r16(cur.tf.eax) == 0x6200);
    dos_service(v);                     /* 62h -> PSP_SEG, IRET to the return stub */
    step(v, 13, 0);
    CHECK(!in_pm() && cur.tf.cs == DOS_SEG && r16(cur.tf.eax) == 0x4C01 && !d->active && !d->clients);
    destroy(v, true); empty();
    printf("DPMI termination: PM 4Ch cleanup, environment restored, stale callback fault, PSP-checked V86 4Ch, raw switch\n");
}


static void test_dpmi_callbacks_raw(void)
{
    struct dosvm *v = dpmi_vm(true);
    struct dpmi *d = &v->dpmi;
    v86_start(); enter(v, false);
    uint16_t code = r16(cur.tf.cs), data = r16(cur.tf.ds);
    /* 0303h: at least 16 slots per client; invalid procedure/structure fail. */
    cur.tf.ds = data; cur.tf.esi = 0x10; cur.tf.es = data; cur.tf.edi = 0x200; int31(v, 0x0303); CHECK(cf());
    cur.tf.ds = code; cur.tf.esi = 0x10; cur.tf.es = data; cur.tf.edi = 0xFFF0; int31(v, 0x0303); CHECK(cf());
    uint16_t cb[DPMI_CALLBACKS];
    for (unsigned k = 0; k < DPMI_CALLBACKS; k++) {
        cur.tf.ds = code; cur.tf.esi = 0x600 + k; cur.tf.es = data; cur.tf.edi = 0x200;
        int31(v, 0x0303); CHECK(!cf() && r16(cur.tf.ecx) == 0xF000);
        cb[k] = r16(cur.tf.edx);
    }
    cur.tf.ds = code; int31(v, 0x0303); CHECK(cf());                     /* seventeenth */
    cur.tf.ecx = 0xF000; cur.tf.edx = cb[15]; int31(v, 0x0304); CHECK(!cf());
    cur.tf.ecx = 0xF000; cur.tf.edx = cb[15]; int31(v, 0x0304); CHECK(cf());
    cur.tf.ecx = 0xF000; cur.tf.edx = cb[0] + 1; int31(v, 0x0304); CHECK(cf());
    cur.tf.ds = data;
    /* Real mode calls callback 0 with a far call; the PM procedure runs on
     * the locked stack with DS:SI = real SS:SP and ES:DI = the structure. */
    struct v86_frame pm = cur;
    int31(v, 0x0306); uint16_t rawcs = r16(cur.tf.esi); uint32_t rawip = cur.tf.edi;
    uint16_t rm_back_ip = r16(cur.tf.ecx), rm_back_cs = r16(cur.tf.ebx);
    cur.tf.eax = DATA_SEG; cur.tf.ecx = DATA_SEG; cur.tf.edx = STACK_SEG; cur.tf.ebx = 0x600; cur.tf.esi = CODE_SEG; cur.tf.edi = 0x200;
    cur.tf.cs = rawcs; cur.tf.eip = rawip; step(v, 13, 0);
    CHECK(!in_pm() && cur.tf.cs == CODE_SEG && cur.tf.eip == 0x200 && r16(cur.tf.user_esp) == 0x600);
    rm_push16(v, CODE_SEG); rm_push16(v, 0x205);                         /* far call return */
    cur.tf.eax = 0x7777; cur.tf.cs = 0xF000; cur.tf.eip = cb[0];
    step(v, 13, 0);
    CHECK(in_pm() && left && r16(cur.tf.cs) == code && cur.tf.eip == 0x600 && r16(cur.tf.user_ss) == DPMI_HOST_SS);
    CHECK(cur.tf.user_esp == DPMI_HSTACK_BYTES - 6 && !v->monitor.vif && r16(cur.tf.es) == data && cur.tf.edi == 0x200);
    CHECK(seg_base(v, r16(cur.tf.ds)) == STACK_SEG << 4 && r16(cur.tf.esi) == 0x5FC && d->depth == 1);
    CHECK(rmcs32(v, 0x1C) == 0x7777 && rmcs16(v, 0x2C) == 0xF000 && rmcs16(v, 0x2A) == cb[0] && rmcs16(v, 0x2E) == 0x5FC && rmcs16(v, 0x30) == STACK_SEG);
    /* The procedure simulates RETF: pops the return address off the real
     * stack into the structure, sets AX, then IRETs to the host. */
    uint16_t ret[2]; CHECK(!dosvm_read(v, (STACK_SEG << 4) + 0x5FC, ret, 4));
    uint8_t st[0x32]; CHECK(!dosvm_read(v, rmcs(), st, sizeof(st)));
    vm_w16(st + 0x2A, ret[0]); vm_w16(st + 0x2C, ret[1]); vm_w16(st + 0x2E, 0x600); vm_w32(st + 0x1C, 0x4321);
    CHECK(!dosvm_write(v, rmcs(), st, sizeof(st)));
    pm_iret(v); at_stub(v, DPMI_PM_CBRET);
    CHECK(!in_pm() && left && cur.tf.cs == CODE_SEG && cur.tf.eip == 0x205 && r16(cur.tf.user_esp) == 0x600 && r16(cur.tf.eax) == 0x4321 && !d->depth);
    CHECK(d->kind[DPMI_IDX_TEMP] == DPMI_FREE && d->lock_top == DPMI_HSTACK_BYTES);
    /* A forged CBRET (no record) terminates the VM. Raw back to PM first. */
    cur.tf.eax = data; cur.tf.ecx = data; cur.tf.edx = r16(pm.tf.user_ss); cur.tf.ebx = 0x700; cur.tf.esi = code; cur.tf.edi = 0x300;
    cur.tf.cs = rm_back_cs; cur.tf.eip = rm_back_ip; step(v, 13, 0);
    CHECK(in_pm() && left && r16(cur.tf.cs) == code && cur.tf.eip == 0x300 && !cur.tf.fs && !cur.tf.gs && r16(cur.tf.ds) == data);
    /* 0305h: zero-size state, both routines just return. */
    int31(v, 0x0305); CHECK(!cf() && r16(cur.tf.eax) == 0 && r16(cur.tf.ebx) == 0xF000 && r16(cur.tf.esi) == DPMI_HOST_CS);
    uint16_t save_ip = r16(cur.tf.edi);
    pm_push(v, code); pm_push(v, 0x333); cur.tf.cs = DPMI_HOST_CS; cur.tf.eip = save_ip; cur.tf.eax = 0;
    step(v, 13, 0); CHECK(in_pm() && r16(cur.tf.cs) == code && cur.tf.eip == 0x333 && r16(cur.tf.eax) == 0);
    /* Raw switch into PM with an invalid CS terminates only the VM. */
    int31(v, 0x0306);
    cur.tf.eax = DATA_SEG; cur.tf.ecx = DATA_SEG; cur.tf.edx = STACK_SEG; cur.tf.ebx = 0x600; cur.tf.esi = CODE_SEG; cur.tf.edi = 0x200;
    cur.tf.cs = DPMI_HOST_CS; cur.tf.eip = DPMI_PM_RAW; step(v, 13, 0);
    CHECK(!in_pm());
    cur.tf.eax = data; cur.tf.ecx = 0; cur.tf.edx = data; cur.tf.ebx = 0x700; cur.tf.esi = data; cur.tf.edi = 0;   /* CS = data selector */
    cur.tf.cs = rm_back_cs; cur.tf.eip = rm_back_ip; step(v, 13, 0);
    CHECK(v->info.state == DOSVM_STOPPING && v->info.reason == DOSVM_FAULT);
    destroy(v, true); empty();
    printf("DPMI callbacks/raw/state 0303h-0306h: 16 slots, locked-stack procedure, structure return, raw switches validated\n");
}

static void test_dpmi_memory_misc(void)
{
    struct dosvm *v = dpmi_vm(true);
    struct dpmi *d = &v->dpmi;
    v86_start(); enter(v, false);
    uint16_t data = r16(cur.tf.ds);
    uint32_t info_lin = (DATA_SEG << 4) + 0x300, info[12];
    cur.tf.es = data; cur.tf.edi = 0x300; int31(v, 0x0500); CHECK(!cf());
    CHECK(!dosvm_read(v, info_lin, info, sizeof(info)) && info[0] == 4u * 1024 * 1024 - DPMI_HSTACK_BYTES && info[1] == info[0] / 4096);
    CHECK(info[3] == 1024 && info[4] == 0xFFFFFFFFu && info[5] == info[1] && info[8] == 0xFFFFFFFFu && info[11] == 0xFFFFFFFFu);
    cur.tf.edi = 0xFFE0; int31(v, 0x0500); CHECK(cf());
    /* 0501h-0503h on the pool shared with XMS. */
    cur.tf.ebx = 0; cur.tf.ecx = 0; int31(v, 0x0501); CHECK(cf());
    cur.tf.ebx = 0x0040; cur.tf.ecx = 0x0001; int31(v, 0x0501); CHECK(cf());   /* > quota */
    cur.tf.ebx = 0; cur.tf.ecx = 0x1001; int31(v, 0x0501); CHECK(!cf());
    uint32_t a = ((uint32_t)r16(cur.tf.ebx) << 16) | r16(cur.tf.ecx), ha = ((uint32_t)r16(cur.tf.esi) << 16) | r16(cur.tf.edi);
    CHECK(a == VM_POOL_BASE + DPMI_HSTACK_BYTES && v->info.pool_used == DPMI_HSTACK_BYTES + 8192 && ha);
    cur.tf.ebx = 0; cur.tf.ecx = 0x2000; int31(v, 0x0501); CHECK(!cf());
    uint32_t hb = ((uint32_t)r16(cur.tf.esi) << 16) | r16(cur.tf.edi);
    uint32_t pattern = 0x5A5AA5A5; CHECK(!dosvm_write(v, a + 16, &pattern, 4));
    /* XMS sees the same pool: its free total shrank by the DPMI blocks. */
    struct v86_frame x = {.tf = {.eax = 0x0800}}; CHECK(!dosvm_xms(v, &x) && r16(x.tf.edx) == (4096 - 16 - 8 - 8));
    /* Lock counts: lock twice, unlock twice, a third unlock fails. */
    cur.tf.ebx = a >> 16; cur.tf.ecx = a + 0x10; cur.tf.esi = 0; cur.tf.edi = 0x1000; int31(v, 0x0600); CHECK(!cf());
    CHECK(d->lock[a >> 12] == 1 && d->lock[(a >> 12) + 1] == 1);
    cur.tf.ebx = a >> 16; cur.tf.ecx = a + 0x10; cur.tf.esi = 0; cur.tf.edi = 0x1000; int31(v, 0x0600); CHECK(!cf() && d->lock[a >> 12] == 2);
    for (unsigned i = 0; i < 2; i++) { cur.tf.ebx = a >> 16; cur.tf.ecx = a + 0x10; cur.tf.esi = 0; cur.tf.edi = 0x1000; int31(v, 0x0601); CHECK(!cf()); }
    cur.tf.ebx = a >> 16; cur.tf.ecx = a + 0x10; cur.tf.esi = 0; cur.tf.edi = 0x1000; int31(v, 0x0601); CHECK(cf());
    uint32_t free_pool = VM_POOL_BASE + 0x200000;                       /* unallocated pool page */
    cur.tf.ebx = free_pool >> 16; cur.tf.ecx = free_pool; cur.tf.esi = 0; cur.tf.edi = 16; int31(v, 0x0600); CHECK(cf());
    cur.tf.ebx = 0xC000; cur.tf.ecx = 0; cur.tf.esi = 0; cur.tf.edi = 16; int31(v, 0x0600); CHECK(cf()); /* kernel address */
    cur.tf.ebx = 0x0001; cur.tf.ecx = 0x0000; cur.tf.esi = 0; cur.tf.edi = 0x1000; int31(v, 0x0600); CHECK(!cf()); /* real-mode RAM */
    cur.tf.ebx = 0x0001; cur.tf.ecx = 0x0000; cur.tf.esi = 0; cur.tf.edi = 0x1000; int31(v, 0x0601); CHECK(!cf());
    cur.tf.ebx = 0x0001; cur.tf.ecx = 0x0800; cur.tf.esi = 0; cur.tf.edi = 0x2000; int31(v, 0x0602); CHECK(!cf());
    CHECK(d->pageable[0x11 / 8] & (1u << (0x11 % 8))); CHECK(!(d->pageable[0x10 / 8] & (1u << (0x10 % 8))));
    cur.tf.ebx = 0x0001; cur.tf.ecx = 0x0800; cur.tf.esi = 0; cur.tf.edi = 0x2000; int31(v, 0x0603); CHECK(!cf() && !(d->pageable[0x11 / 8] & (1u << (0x11 % 8))));
    cur.tf.ebx = 0x0010; cur.tf.ecx = 0x0000; cur.tf.esi = 0; cur.tf.edi = 0x10; int31(v, 0x0602); CHECK(cf()); /* not below 1 MiB */
    int31(v, 0x0604); CHECK(!cf() && r16(cur.tf.ebx) == 0 && r16(cur.tf.ecx) == 4096);
    int31(v, 0x0700); CHECK(cf() && r16(cur.tf.eax) == 0x0700); int31(v, 0x0701); CHECK(cf());
    cur.tf.ebx = a >> 16; cur.tf.ecx = a; cur.tf.esi = 0; cur.tf.edi = 0x2000; int31(v, 0x0702); CHECK(!cf());
    /* 0703h discards complete pages only: partial edges keep their bytes. */
    uint32_t edge = 0x1234; CHECK(!dosvm_write(v, a + 0x1FFC, &edge, 4));
    cur.tf.ebx = a >> 16; cur.tf.ecx = a + 0x0800; cur.tf.esi = 0; cur.tf.edi = 0x1800; int31(v, 0x0703); CHECK(!cf());
    uint32_t got; CHECK(!dosvm_read(v, a + 16, &got, 4) && got == pattern);
    CHECK(!dosvm_read(v, a + 0x1FFC, &got, 4) && got == 0);
    cur.tf.ebx = 0x0001; cur.tf.ecx = 0; cur.tf.esi = 0; cur.tf.edi = 0x2000; int31(v, 0x0703); CHECK(cf()); /* not a DPMI block */
    /* 0503h grows in place, then past a neighbour (relocates, keeps bytes and locks). */
    cur.tf.ebx = a >> 16; cur.tf.ecx = a; cur.tf.esi = 0; cur.tf.edi = 16; int31(v, 0x0600); CHECK(!cf());
    cur.tf.ebx = 0; cur.tf.ecx = 0x3000; cur.tf.esi = ha >> 16; cur.tf.edi = ha; int31(v, 0x0503); CHECK(!cf());
    uint32_t a2 = ((uint32_t)r16(cur.tf.ebx) << 16) | r16(cur.tf.ecx), ha2 = ((uint32_t)r16(cur.tf.esi) << 16) | r16(cur.tf.edi);
    CHECK(a2 != a && ha2 != ha && !dosvm_read(v, a2 + 16, &got, 4) && got == pattern && d->lock[a2 >> 12] == 1 && !d->lock[a >> 12]);
    cur.tf.ebx = 0; cur.tf.ecx = 0x3000; cur.tf.esi = ha >> 16; cur.tf.edi = ha; int31(v, 0x0503); CHECK(cf());   /* old handle */
    cur.tf.ebx = 0; cur.tf.ecx = 0; cur.tf.esi = ha2 >> 16; cur.tf.edi = ha2; int31(v, 0x0503); CHECK(cf());      /* size 0 */
    cur.tf.ebx = 0x0100; cur.tf.ecx = 0; cur.tf.esi = ha2 >> 16; cur.tf.edi = ha2; int31(v, 0x0503); CHECK(cf() && d->block[0].b.used); /* failed growth keeps it */
    cur.tf.esi = hb >> 16; cur.tf.edi = hb; int31(v, 0x0502); CHECK(!cf());
    cur.tf.esi = hb >> 16; cur.tf.edi = hb; int31(v, 0x0502); CHECK(cf());
    cur.tf.esi = ha2 >> 16; cur.tf.edi = ha2; int31(v, 0x0502); CHECK(!cf() && v->info.pool_used == DPMI_HSTACK_BYTES && !d->lock[a2 >> 12]);
    int31(v, 0x0504); CHECK(cf()); int31(v, 0x050B); CHECK(cf());
    /* 0400h version and PIC bases; 0401h is DPMI 1.0. */
    int31(v, 0x0400); CHECK(!cf() && r16(cur.tf.eax) == 0x005A && r16(cur.tf.ebx) == 1 && (uint8_t)cur.tf.ecx == 6 && r16(cur.tf.edx) == 0x0870);
    int31(v, 0x0401); CHECK(cf() && r16(cur.tf.eax) == 0x0401);
    /* 0800h refused, AX unchanged; 0801h/1.0 rows refused. */
    cur.tf.ebx = 0xFEC0; cur.tf.ecx = 0; cur.tf.esi = 0; cur.tf.edi = 0x1000; int31(v, 0x0800); CHECK(cf() && r16(cur.tf.eax) == 0x0800 && r16(cur.tf.ebx) == 0xFEC0);
    int31(v, 0x0801); CHECK(cf());
    /* 0900h-0902h share the monitor's virtual IF. */
    v->monitor.vif = true; int31(v, 0x0900); CHECK(!cf() && r16(cur.tf.eax) == 0x0901 && !v->monitor.vif);
    int31(v, 0x0902); CHECK(r16(cur.tf.eax) == 0x0900);
    int31(v, 0x0901); CHECK(r16(cur.tf.eax) == 0x0900 && v->monitor.vif);
    op1(v, 0xFA); CHECK(!v->monitor.vif); int31(v, 0x0902); CHECK(r16(cur.tf.eax) == 0x0900);
    op1(v, 0xFB); int31(v, 0x0902); CHECK(r16(cur.tf.eax) == 0x0901 && !d->shadow);
    /* 0A00h: no vendor extension. */
    cur.tf.ds = data; cur.tf.esi = 0x10; int31(v, 0x0A00); CHECK(cf() && r16(cur.tf.eax) == 0x0A00);
    /* Unknown and 1.0-only functions change nothing. */
    static const uint16_t refused[] = {0x0C00, 0x0C01, 0x0D00, 0x0D01, 0x0D02, 0x0D03, 0x0E00, 0x0E01, 0x0F00, 0x0103, 0x0306 + 1, 0xFFFF, 0x0B04};
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        struct dpmi snap = *d; struct v86_frame regs = cur;
        int31(v, refused[i]);
        CHECK(cf() && r16(cur.tf.eax) == refused[i] && cur.tf.ebx == regs.tf.ebx && cur.tf.ecx == regs.tf.ecx && cur.tf.edx == regs.tf.edx);
        snap.stats = d->stats;
        /* Trap-boundary cache advances independently of INT 31h state. */
        snap.decode_valid = d->decode_valid; snap.decode_cs = d->decode_cs; snap.decode_eip = d->decode_eip;
        CHECK(!memcmp(&snap, d, sizeof(snap)));
    }
    destroy(v, true); empty();
    printf("DPMI memory/paging/misc 0400h-0A00h: quota report, blocks, lock counts, discard edges, refusals change nothing\n");
}

static void test_dpmi_dos_memory(void)
{
    struct dosvm *v = dpmi_vm(true);
    struct dpmi *d = &v->dpmi;
    v86_start(); enter(v, false);
    cur.tf.ebx = 0x1800; int31(v, 0x0100);                               /* 96 KiB: two selectors */
    CHECK(!in_pm() && cur.tf.cs == DOS_SEG && r16(cur.tf.eax) == 0x4800 && r16(cur.tf.ebx) == 0x1800);
    dos_roundtrip(v);
    CHECK(in_pm() && !cf() && r16(cur.tf.eax) == 0x6000);
    uint16_t sel = r16(cur.tf.edx);
    struct dpmi_seg s;
    CHECK(dpmi_seg(v, sel, &s) && s.base == 0x60000 && s.limit == 0x17FFF && d->kind[sel >> 3] == DPMI_DOS);
    CHECK(dpmi_seg(v, sel + 8, &s) && s.base == 0x70000 && s.limit == 0x7FFF);
    cur.tf.ebx = sel; int31(v, 0x0001); CHECK(cf());                     /* never freed by 0001h */
    /* Shrink to one selector, then fail a growth DOS refuses (AX=8, BX=max). */
    cur.tf.ebx = 0x0800; cur.tf.edx = sel; int31(v, 0x0102); dos_roundtrip(v);
    CHECK(!cf() && dpmi_seg(v, sel, &s) && s.limit == 0x7FFF && d->kind[(sel >> 3) + 1] == DPMI_FREE);
    cur.tf.ebx = 0x3000; cur.tf.edx = sel; int31(v, 0x0102); dos_roundtrip(v);
    CHECK(cf() && r16(cur.tf.eax) == 8 && r16(cur.tf.ebx) == 0x2000 && d->kind[(sel >> 3) + 1] == DPMI_FREE && dpmi_seg(v, sel, &s) && s.limit == 0x7FFF);
    /* DOS allocation failure: AX/BX from DOS, no selector left behind. */
    dos_fail = true; unsigned used = 0;
    for (unsigned i = 0; i < DPMI_LDT_ENTRIES; i++) used += d->kind[i] != DPMI_FREE;
    cur.tf.ebx = 0x100; int31(v, 0x0100); dos_roundtrip(v);
    CHECK(cf() && r16(cur.tf.eax) == 8 && r16(cur.tf.ebx) == 0x4000);
    unsigned now = 0; for (unsigned i = 0; i < DPMI_LDT_ENTRIES; i++) now += d->kind[i] != DPMI_FREE;
    CHECK(now == used); dos_fail = false;
    cur.tf.edx = sel + 8; int31(v, 0x0101); CHECK(cf() && r16(cur.tf.eax) == 9);   /* not a block selector */
    cur.tf.edx = sel; int31(v, 0x0101); dos_roundtrip(v);
    CHECK(!cf() && d->kind[sel >> 3] == DPMI_FREE && !d->dos[0].count && !dos_blocks[0][1]);
    destroy(v, true); empty();
    printf("DPMI DOS memory 0100h-0102h through the personality: selectors, shrink/grow, DOS error codes\n");
}


static struct dosvm *hlt_vm; static unsigned hlt_wait;
static void hlt_raise(void) { if (++hlt_wait < 5) return; hlt_wait = 0; sleep_hook = 0; CHECK(dosvm_irq_raise(dosvm_key(hlt_vm), 0)); }
static void test_dpmi_irq_exceptions(void)
{
    struct dosvm *v = dpmi_vm(true);
    struct dpmi *d = &v->dpmi;
    struct dosvm_key key = dosvm_key(v);
    v86_start(); enter(v, false);
    uint16_t code = r16(cur.tf.cs);
    /* Unhooked IRQ0 while in protected mode: default reflection to the real
     * vector (IRET handler), then the interrupted frame comes back intact. */
    uint16_t ivt[2] = {0, IRET_SEG}; CHECK(!dosvm_write(v, 8 * 4, ivt, 4));
    cur.tf.eax = 0x11112222u; cur.tf.esi = 0x3333; struct v86_frame before = cur;
    CHECK(dosvm_irq_raise(key, 0));
    uint8_t in21[2] = {0xE4, 0x21}; exec(v, in21, 2, 13, 0);                /* any trap is a delivery point */
    CHECK(!in_pm() && left && cur.tf.cs == IRET_SEG && v->monitor.pic[0].isr == 1 && !v->monitor.vif && d->stats.irqs_reflected == 1);
    cur.tf.eax = 0xDEAD; cur.tf.esi = 0;
    rm_iret_roundtrip(v);
    CHECK(in_pm() && (cur.tf.eax & ~0xFFu) == (before.tf.eax & ~0xFFu) && cur.tf.esi == before.tf.esi && cur.tf.eip == before.tf.eip + 2 && v->monitor.vif);
    CHECK((uint8_t)cur.tf.eax == v->monitor.pic[0].imr);   /* the IN completed before the delivery */
    v->monitor.pic[0].isr = 0;                                             /* the real handler would EOI */
    /* Hook IRQ0 in protected mode: handler on the locked stack, IF clear. */
    cur.tf.ebx = 0x08; cur.tf.ecx = code; cur.tf.edx = 0x700; int31(v, 0x0205);
    before = cur;
    CHECK(dosvm_irq_raise(key, 0));
    op1(v, 0xFA); CHECK(!v->monitor.vif && in_pm() && cur.tf.eip == before.tf.eip + 1);  /* CLI: no delivery */
    CHECK(v->monitor.pic[0].irr & 1);
    before = cur;
    op1(v, 0xFB);                       /* STI: shadow, delivery waits one instruction */
    CHECK(v->monitor.vif && d->shadow && cur.tf.eip == before.tf.eip + 1 && !(cur.tf.eflags & V86_TF));
    step(v, 0x20, 0);                   /* physical IRQ before the shadowed instruction */
    CHECK(in_pm() && cur.tf.eip == before.tf.eip + 1 && !d->depth && (v->monitor.pic[0].irr & 1));
    cur.tf.eip += 1;                    /* the shadowed instruction retired */
    before = cur;
    step(v, 0x20, 0);
    CHECK(in_pm() && cur.tf.eip == 0x700 && r16(cur.tf.user_ss) == DPMI_HOST_SS && cur.tf.user_esp == DPMI_HSTACK_BYTES - 6);
    CHECK(!v->monitor.vif && !(cur.tf.eflags & V86_TF) && d->depth == 1 && d->stats.irqs_pm == 1 && v->monitor.pic[0].isr == 1);
    uint16_t fr[3]; CHECK(!dosvm_read(v, d->hstack.base + DPMI_HSTACK_BYTES - 6, fr, 6));
    CHECK(fr[0] == DPMI_PM_IRQRET && fr[1] == DPMI_HOST_CS && (fr[2] & V86_IF));
    uint8_t eoi[2] = {0xB0, 0x20}; (void)eoi;
    cur.tf.eax = 0x20; uint8_t out20[2] = {0xE6, 0x20}; exec(v, out20, 2, 13, 0);
    CHECK(!v->monitor.pic[0].isr);
    cur.tf.eax = 0x9999;               /* a handler that clobbers registers */
    pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
    CHECK(in_pm() && cur.tf.eip == before.tf.eip && cur.tf.eax == before.tf.eax && v->monitor.vif && !d->depth && d->lock_top == DPMI_HSTACK_BYTES);
    /* HLT with IF set: the worker blocks until the (fake) timer raises IRQ0,
     * then the hooked handler runs with the return address after HLT. */
    before = cur;
    hlt_vm = v; sleep_hook = hlt_raise; unsigned progress = native_progress;
    op1(v, 0xF4);
    CHECK(!sleep_hook && native_progress > progress + 2 && cur.tf.eip == 0x700 && d->depth == 1 && v->monitor.state == V86_RUNNING);
    uint16_t hfr[3]; CHECK(!dosvm_read(v, d->hstack.base + DPMI_HSTACK_BYTES - 6, hfr, 6));
    CHECK(hfr[0] == DPMI_PM_IRQRET);
    CHECK(d->rec[0].saved.tf.eip == before.tf.eip + 1);
    /* The handler may IRET from another stack (the interrupted context is
     * the kernel record's), but a return through the wrong host stub (the
     * software-interrupt return for a hardware interrupt) terminates the VM. */
    cur.tf.user_esp -= 2; cur.tf.cs = DPMI_HOST_CS; cur.tf.eip = DPMI_PM_SOFTRET; step(v, 13, 0);
    CHECK(v->info.state == DOSVM_STOPPING && v->info.reason == DOSVM_FAULT);
    destroy(v, true); empty();

    /* IRQ raised while the client is in real mode (inside a 0300h call):
     * hooked vector -> protected-mode handler, then back to V86. */
    v = dpmi_vm(true); d = &v->dpmi; key = dosvm_key(v);
    v86_start(); enter(v, false);
    code = r16(cur.tf.cs);
    cur.tf.ebx = 0x09; cur.tf.ecx = code; cur.tf.edx = 0x720; int31(v, 0x0205);
    v->monitor.pic[0].imr = 0xF8;
    rmcs_set(v, 0, 0, 0x0202, 0, IRET_SEG, 0, 0, 0); cur.tf.ecx = 0; cur.tf.es = cur.tf.ds; cur.tf.edi = 0x200;
    int31(v, 0x0302);
    CHECK(!in_pm() && cur.tf.cs == IRET_SEG && d->depth == 1);
    /* The real-mode routine runs elsewhere (3000h) with IF set and traps
     * at an absent port; IRQ1 is pending. */
    v->monitor.vif = true; CHECK(dosvm_irq_raise(key, 1));
    cur.tf.cs = 0x3000; cur.tf.eip = 0;
    uint8_t nop_out[2] = {0xE6, 0xE8};
    exec(v, nop_out, 2, 13, 0);
    CHECK(in_pm() && left && cur.tf.eip == 0x720 && r16(cur.tf.user_ss) == DPMI_HOST_SS && !cur.tf.ds && d->depth == 2);
    pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
    CHECK(!in_pm() && left && cur.tf.cs == 0x3000 && cur.tf.eip == 2 && d->depth == 1 && v->monitor.vif);
    cur.tf.cs = IRET_SEG; cur.tf.eip = 0;                                  /* the routine's final IRET */
    rm_iret_roundtrip(v); CHECK(in_pm() && !cf() && !d->depth);
    /* Exceptions. Default 0Dh is fatal; default 00h becomes INT 00h. */
    cur.tf.ebx = 0x00; cur.tf.ecx = code; cur.tf.edx = 0x740; int31(v, 0x0205);
    before = cur; step(v, 0, 0);                                           /* #DE */
    CHECK(in_pm() && cur.tf.eip == 0x740 && !v->monitor.vif && cur.tf.user_esp == before.tf.user_esp - 6);
    pm_iret(v); op1(v, 0xFB);
    /* A client 0Dh handler: locked-stack frame, far return, edited EIP. */
    cur.tf.ebx = 0x0D; cur.tf.ecx = code; cur.tf.edx = 0x760; int31(v, 0x0203);
    before = cur;
    uint8_t lgdt[3] = {0x0F, 0x01, 0x10}; exec(v, lgdt, 3, 13, 0);         /* privileged: genuine #GP(0) */
    CHECK(in_pm() && cur.tf.eip == 0x760 && r16(cur.tf.user_ss) == DPMI_HOST_SS && cur.tf.user_esp == DPMI_HSTACK_BYTES - 16 && !v->monitor.vif);
    uint16_t ex[8]; CHECK(!dosvm_read(v, d->hstack.base + DPMI_HSTACK_BYTES - 16, ex, 16));
    CHECK(ex[0] == DPMI_PM_EXCRET && ex[1] == DPMI_HOST_CS && ex[2] == 0 && ex[3] == before.tf.eip && ex[4] == code);
    CHECK((ex[5] & V86_IF) && ex[6] == r16(before.tf.user_esp) && ex[7] == r16(before.tf.user_ss));
    ex[3] = (uint16_t)(before.tf.eip + 3);                                 /* skip the instruction */
    CHECK(!dosvm_write(v, d->hstack.base + DPMI_HSTACK_BYTES - 16, ex, 16));
    pm_retf(v); at_stub(v, DPMI_PM_EXCRET);
    CHECK(in_pm() && cur.tf.eip == before.tf.eip + 3 && cur.tf.user_esp == before.tf.user_esp && v->monitor.vif && !d->depth);
    /* A handler that moves the frame lower on the locked stack before its
     * far return (DOS/4GW's error path): the host reads it at SS:ESP. */
    exec(v, lgdt, 3, 13, 0);
    CHECK(!dosvm_read(v, d->hstack.base + DPMI_HSTACK_BYTES - 16, ex, 16));
    ex[3] = (uint16_t)(before.tf.eip + 3);
    CHECK(!dosvm_write(v, d->hstack.base + DPMI_HSTACK_BYTES - 32, ex, 16));
    cur.tf.user_esp = DPMI_HSTACK_BYTES - 32;
    pm_retf(v); at_stub(v, DPMI_PM_EXCRET);
    CHECK(in_pm() && cur.tf.eip == before.tf.eip + 3 && cur.tf.user_esp == before.tf.user_esp && !d->depth && d->lock_top == DPMI_HSTACK_BYTES);
    /* Guest-edited frame with an invalid CS terminates the VM. */
    exec(v, lgdt, 3, 13, 0);
    CHECK(!dosvm_read(v, d->hstack.base + DPMI_HSTACK_BYTES - 16, ex, 16));
    ex[4] = r16(cur.tf.ds) == 0 ? 0x1234 : 0xFFF7;
    CHECK(!dosvm_write(v, d->hstack.base + DPMI_HSTACK_BYTES - 16, ex, 16));
    pm_retf(v); at_stub(v, DPMI_PM_EXCRET);
    CHECK(v->info.state == DOSVM_STOPPING && v->info.reason == DOSVM_FAULT);
    destroy(v, true); empty();

    /* Default fatal exception (#UD) terminates only this VM; a witness VM lives. */
    struct dosvm *w = create();
    v = dpmi_vm(true); d = &v->dpmi;
    v86_start(); enter(v, false);
    step(v, 6, 0);
    CHECK(v->info.state == DOSVM_STOPPING && v->info.reason == DOSVM_FAULT && v->info.fault_vector == 6 && w->info.state == DOSVM_CREATING);
    destroy(v, true); destroy(w, true); empty();
    printf("DPMI dispatch: IRQ reflection/PM handler from both modes, STI shadow, EOI, exceptions, edited/forged frames\n");
}

static void test_dpmi_io_watch(void)
{
    struct dosvm *v = dpmi_vm(true);
    struct dpmi *d = &v->dpmi;
    v86_start(); enter(v, false);
    /* Port I/O from ring 3 reaches only the VM's virtual devices. */
    unsigned lines = v->info.guest_records;
    const char *text = "PM HI";
    for (unsigned i = 0; text[i]; i++) { cur.tf.eax = (uint8_t)text[i]; uint8_t o[2] = {0xE6, 0xE9}; exec(v, o, 2, 13, 0); }
    cur.tf.eax = 10; uint8_t o[2] = {0xE6, 0xE9}; exec(v, o, 2, 13, 0);
    CHECK(v->info.guest_records == lines + 1 && in_pm());
    cur.tf.eax = 0xFF00; uint8_t in21[2] = {0xE4, 0x21}; exec(v, in21, 2, 13, 0);
    CHECK((uint8_t)cur.tf.eax == v->monitor.pic[0].imr && (cur.tf.eax & 0xFF00) == 0xFF00);
    cur.tf.edx = 0x3F8; cur.tf.eax = 0; op1(v, 0xEC); CHECK((uint8_t)cur.tf.eax == 0xFF);   /* absent UART */
    cur.tf.edx = 0x3F8; cur.tf.eax = 0; uint8_t inw[2] = {0x66, 0xED}; exec(v, inw, 2, 13, 0); CHECK(cur.tf.eax == 0xFFFFFFFFu);
    /* rep outsb: one element per trap, restartable. */
    uint32_t src = (DATA_SEG << 4) + 0x40; CHECK(!dosvm_write(v, src, "ok\n", 3));
    cur.tf.esi = 0x40; cur.tf.ecx = 3; cur.tf.edx = 0xE9; uint8_t rep[2] = {0xF3, 0x6E};
    uint32_t at = cur.tf.eip;
    exec(v, rep, 2, 13, 0); CHECK(cur.tf.eip == at && r16(cur.tf.ecx) == 2 && r16(cur.tf.esi) == 0x41);
    step(v, 13, 0); step(v, 13, 0); CHECK(cur.tf.eip == at + 2 && !r16(cur.tf.ecx) && v->info.guest_records == lines + 2);
    /* Watchpoints: VM memory only, DR7 switched with the VM. */
    uint32_t wlin = (DATA_SEG << 4) + 0x80;
    cur.tf.ebx = wlin >> 16; cur.tf.ecx = wlin; cur.tf.edx = 0x0104; int31(v, 0x0B00); CHECK(!cf());
    unsigned h = r16(cur.tf.ebx);
    CHECK(h == 0 && debug_dr7 == ((1u << 0) | (1u << 16) | (3u << 18) | (1u << 8)) && debug_addr[0] == wlin);
    cur.tf.ebx = 0xC010; cur.tf.ecx = 0; cur.tf.edx = 0x0104; int31(v, 0x0B00); CHECK(cf());   /* kernel */
    cur.tf.ebx = wlin >> 16; cur.tf.ecx = wlin + 1; cur.tf.edx = 0x0102; int31(v, 0x0B00); CHECK(cf()); /* unaligned */
    cur.tf.ebx = wlin >> 16; cur.tf.ecx = wlin; cur.tf.edx = 0x0002; int31(v, 0x0B00); CHECK(cf());  /* execute, size 2 */
    cur.tf.ebx = wlin >> 16; cur.tf.ecx = wlin; cur.tf.edx = 0x0301; int31(v, 0x0B00); CHECK(cf());  /* type 3 */
    cur.tf.ebx = h; int31(v, 0x0B02); CHECK(!cf() && r16(cur.tf.eax) == 0);
    debug_dr6 = 0x0001; step(v, 1, 0);                                     /* the guest wrote it */
    CHECK(in_pm() && v->info.state != DOSVM_STOPPING);
    cur.tf.ebx = h; int31(v, 0x0B02); CHECK(!cf() && r16(cur.tf.eax) == 1);
    cur.tf.ebx = h; int31(v, 0x0B03); CHECK(!cf());
    cur.tf.ebx = h; int31(v, 0x0B02); CHECK(r16(cur.tf.eax) == 0);
    dosvm_task_switch(&caller); CHECK(!debug_dr7 && !ldt_loaded);         /* another task: none */
    dosvm_task_switch(v->task); CHECK(debug_dr7 && ldt_loaded == d->ldt);
    cur.tf.ebx = h; int31(v, 0x0B01); CHECK(!cf() && !debug_dr7);
    cur.tf.ebx = h; int31(v, 0x0B01); CHECK(cf());
    /* An instruction breakpoint resumes with RF. */
    uint32_t xlin = cs_linear(v) + 0x10;
    cur.tf.ebx = xlin >> 16; cur.tf.ecx = xlin; cur.tf.edx = 0x0001; int31(v, 0x0B00); CHECK(!cf());
    debug_dr6 = 0x0001; step(v, 1, 0); CHECK(cur.tf.eflags & (1u << 16));
    /* The forbidden port faults the VM, as in V86. */
    cur.tf.edx = 0xFFFF; cur.tf.eax = 0; op1(v, 0xEE);
    CHECK(v->info.state == DOSVM_STOPPING && v->info.reason == DOSVM_FAULT);
    destroy(v, true); empty();
    dosvm_task_switch(&caller); CHECK(!debug_dr7 && !ldt_loaded);
    printf("DPMI I/O and watchpoints: virtual ports only, restartable string I/O, VM-local DR7, RF on breakpoints\n");
}


static void test_dpmi_nesting_chaining(void)
{
    struct dosvm *v = dpmi_vm(true);
    struct dpmi *d = &v->dpmi;
    v86_start(); enter(v, false);
    uint16_t code = r16(cur.tf.cs), data = r16(cur.tf.ds);
    /* Eight nested transitions: 0302h -> real mode -> callback -> PM -> 0302h ... */
    cur.tf.ds = code; cur.tf.esi = 0x600; cur.tf.es = data; cur.tf.edi = 0x200; int31(v, 0x0303); CHECK(!cf());
    uint16_t cb = r16(cur.tf.edx);
    cur.tf.ds = data;
    /* Sixteen levels (32 records, every host real-mode stack level): the
     * contract's eight are covered, then the next call meets exhaustion. */
    unsigned levels = 0;
    uint16_t lvl_ss[16], lvl_sp[16];
    for (; levels < 16; levels++) {
        rmcs_set(v, 0, 0, 0x0002, 0, 0xF000, cb, 0, 0); cur.tf.ecx = 0; cur.tf.es = data; cur.tf.edi = 0x200;
        int31(v, 0x0302);                                                  /* real mode enters at the callback */
        CHECK(!in_pm() && cur.tf.cs == 0xF000 && cur.tf.eip == cb);
        step(v, 13, 0);                                                    /* INT FEh at the callback */
        CHECK(in_pm() && cur.tf.eip == 0x600 && d->depth == 2 * levels + 2);
        lvl_ss[levels] = rmcs16(v, 0x30); lvl_sp[levels] = rmcs16(v, 0x2E);
        CHECK(lvl_ss[levels] == DPMI_RSTACK_SEG && lvl_sp[levels] == DPMI_RSTACK_BYTES - DPMI_RSTACK_LEVEL * levels - 6);
    }
    CHECK(d->stats.max_depth == 32 && d->depth == DPMI_DEPTH && d->rstack_top == 0);
    /* Exhaustion: one more 0300h fails with CF instead of nesting. */
    rmcs_set(v, 0, 0, 0x0002, 0, 0, 0, 0, 0); cur.tf.ebx = 0x61; cur.tf.es = data; cur.tf.edi = 0x200;
    int31(v, 0x0300); CHECK(in_pm() && cf() && d->depth == DPMI_DEPTH);
    /* Unwind every level in order through the callback return path. */
    for (unsigned i = levels; i > 0; i--) {
        uint8_t st[0x32]; CHECK(!dosvm_read(v, rmcs(), st, sizeof(st)));
        uint16_t sp = lvl_sp[i - 1], ss = lvl_ss[i - 1], ret[3];
        vm_w16(st + 0x30, ss);
        CHECK(!dosvm_read(v, ((uint32_t)ss << 4) + sp, ret, 6));       /* IRET frame of 0302h */
        vm_w16(st + 0x2A, ret[0]); vm_w16(st + 0x2C, ret[1]); vm_w16(st + 0x20, ret[2]); vm_w16(st + 0x2E, sp + 6);
        CHECK(!dosvm_write(v, rmcs(), st, sizeof(st)));
        cur.tf.es = data; cur.tf.edi = 0x200;
        pm_iret(v); at_stub(v, DPMI_PM_CBRET);
        CHECK(!in_pm() && cur.tf.cs == 0xF000 && cur.tf.eip == DPMI_RM_RETURN);
        step(v, 13, 0);
        CHECK(in_pm() && !cf() && d->depth == 2 * (i - 1));
    }
    CHECK(d->lock_top == DPMI_HSTACK_BYTES && d->rstack_top == DPMI_RSTACK_BYTES);
    /* Chaining: a hooked INT 31h jumps to the address 0204h returned. */
    cur.tf.ebx = 0x31; int31(v, 0x0204); uint16_t old_sel = r16(cur.tf.ecx), old_off = r16(cur.tf.edx);
    cur.tf.ebx = 0x31; cur.tf.ecx = code; cur.tf.edx = 0x780; int31(v, 0x0205); CHECK(!cf());
    struct v86_frame before = cur;
    cur.tf.eax = 0x0400; pm_int(v, 0x31);
    CHECK(in_pm() && cur.tf.eip == 0x780);                                 /* the client's hook */
    cur.tf.cs = old_sel; cur.tf.eip = old_off; step(v, 13, 0);              /* jmp far [old] */
    CHECK(in_pm() && r16(cur.tf.cs) == code && cur.tf.eip == before.tf.eip + 2 && r16(cur.tf.eax) == 0x005A && !cf());
    CHECK(cur.tf.user_esp == before.tf.user_esp);
    /* Chained 0300h: the real call runs, then returns to the original caller. */
    rmcs_set(v, 0x5000, 0, 0x0002, 0, 0, 0, 0, 0); cur.tf.ebx = 0x61; cur.tf.ecx = 0; cur.tf.es = data; cur.tf.edi = 0x200;
    before = cur; cur.tf.eax = 0x0300; pm_int(v, 0x31);
    cur.tf.cs = old_sel; cur.tf.eip = old_off; step(v, 13, 0);
    CHECK(!in_pm() && cur.tf.cs == IRET_SEG && r16(cur.tf.eax) == 0x5000);
    rm_iret_roundtrip(v);
    CHECK(in_pm() && r16(cur.tf.cs) == code && cur.tf.eip == before.tf.eip + 2 && !cf() && cur.tf.user_esp == before.tf.user_esp);
    cur.tf.ebx = 0x31; cur.tf.ecx = old_sel; cur.tf.edx = old_off; int31(v, 0x0205);
    CHECK(cur.tf.eip == 0x780); cur.tf.cs = old_sel; cur.tf.eip = old_off; step(v, 13, 0);   /* the hook chains */
    CHECK(!cf() && d->vec[0x31].sel == DPMI_HOST_CS && d->vec[0x31].off == 0x31);
    /* Exception chaining to the default handler: 00h becomes INT 00h. */
    cur.tf.ebx = 0x00; cur.tf.ecx = code; cur.tf.edx = 0x7A0; int31(v, 0x0203);
    cur.tf.ebx = 0x00; cur.tf.ecx = code; cur.tf.edx = 0x7C0; int31(v, 0x0205);
    before = cur; step(v, 0, 0);
    CHECK(in_pm() && cur.tf.eip == 0x7A0 && d->depth == 1);
    cur.tf.cs = DPMI_HOST_CS; cur.tf.eip = DPMI_PM_EXC + 0; step(v, 13, 0);
    CHECK(in_pm() && cur.tf.eip == 0x7C0 && !d->depth && cur.tf.user_esp == before.tf.user_esp - 6);
    uint32_t ip = pm_pop(v); CHECK(ip == before.tf.eip);
    destroy(v, true); empty();
    printf("DPMI nesting/chaining: 16 nested callback/0302h levels, exhaustion CF, INT 31h and exception chains\n");
}

static void test_dpmi_stale_frames(void)
{
    struct dosvm *v = dpmi_vm(true);
    struct dpmi *d = &v->dpmi;
    v86_start(); enter(v, false);
    uint16_t code = r16(cur.tf.cs), data = r16(cur.tf.ds);
    /* A handler in a freed memory block terminates the VM rather than running. */
    cur.tf.ebx = 0; cur.tf.ecx = 0x1000; int31(v, 0x0501); CHECK(!cf());
    uint32_t base = ((uint32_t)r16(cur.tf.ebx) << 16) | r16(cur.tf.ecx), handle = ((uint32_t)r16(cur.tf.esi) << 16) | r16(cur.tf.edi);
    cur.tf.ecx = 1; int31(v, 0x0000); uint16_t hsel = r16(cur.tf.eax);
    cur.tf.ebx = hsel; cur.tf.ecx = base >> 16; cur.tf.edx = base; int31(v, 0x0007);
    cur.tf.ebx = hsel; cur.tf.ecx = 0; cur.tf.edx = 0xFFF; int31(v, 0x0008);
    cur.tf.ebx = hsel; cur.tf.ecx = 0x00FA; int31(v, 0x0009); CHECK(!cf());
    cur.tf.ebx = 0x67; cur.tf.ecx = hsel; cur.tf.edx = 0x10; int31(v, 0x0205); CHECK(!cf());
    pm_int(v, 0x67); CHECK(in_pm() && cur.tf.eip == 0x10 && r16(cur.tf.cs) == hsel);   /* live: runs */
    pm_iret(v);
    cur.tf.esi = handle >> 16; cur.tf.edi = handle; int31(v, 0x0502); CHECK(!cf());
    unsigned stale = d->stats.stale;
    pm_int(v, 0x67);
    CHECK(v->info.state == DOSVM_STOPPING && v->info.reason == DOSVM_FAULT && d->stats.stale == stale + 1);
    destroy(v, true); empty();
    /* A client that frees its own CS faults the VM at the return. */
    v = dpmi_vm(true); d = &v->dpmi;
    v86_start(); enter(v, false);
    cur.tf.ebx = cur.tf.cs; int31(v, 0x0001);
    CHECK(v->info.state == DOSVM_STOPPING && v->info.reason == DOSVM_FAULT && v->info.fault_vector == 13);
    destroy(v, true); empty();
    /* Freeing DS (also SS here) is legal; SS invalid at return faults too. */
    v = dpmi_vm(true); d = &v->dpmi;
    v86_start(); cur.ds = STACK_SEG; enter(v, false);
    CHECK(r16(cur.tf.ds) == r16(cur.tf.user_ss));
    cur.tf.ebx = cur.tf.ds; int31(v, 0x0001);
    CHECK(v->info.state == DOSVM_STOPPING);
    destroy(v, true); empty();
    (void)code; (void)data;
    printf("DPMI stale handlers and frames: freed storage and freed CS/SS terminate only the VM\n");
}

static void test_dpmi_32bit_nested(void)
{
    struct dosvm *v = dpmi_vm(true);
    struct dpmi *d = &v->dpmi;
    v86_start(); cur.tf.user_esp = 0xABCD0800u; enter(v, true);
    CHECK(in_pm() && d->bits32 && cur.tf.user_esp == 0x0800);
    struct dpmi_seg s;
    CHECK(dpmi_seg(v, r16(cur.tf.ds), &s) && s.big && dpmi_seg(v, r16(cur.tf.user_ss), &s) && s.big);
    CHECK(dpmi_seg(v, r16(cur.tf.cs), &s) && !s.big);                     /* starts in a 16-bit code segment */
    uint16_t code = r16(cur.tf.cs);
    /* A flat 32-bit code selector, 32-bit offsets everywhere. */
    cur.tf.ecx = 2; int31(v, 0x0000); uint16_t flat = r16(cur.tf.eax), fdata = flat + 8;
    cur.tf.ebx = flat; cur.tf.ecx = 0x40FA; int31(v, 0x0009); CHECK(!cf());
    cur.tf.ebx = flat; cur.tf.ecx = 0xFFFF; cur.tf.edx = 0xFFFF; int31(v, 0x0008); CHECK(!cf());
    cur.tf.ebx = fdata; cur.tf.ecx = 0xFFFF; cur.tf.edx = 0xFFFF; int31(v, 0x0008); CHECK(!cf() && dpmi_seg(v, fdata, &s) && s.limit == 0xFFFFFFFFu);
    cur.tf.ebx = 0x68; cur.tf.ecx = flat; cur.tf.edx = 0x00011000u; int31(v, 0x0205); CHECK(!cf() && d->vec[0x68].off == 0x11000);
    cur.tf.ebx = 0x68; int31(v, 0x0204); CHECK(cur.tf.edx == 0x11000);
    struct v86_frame before = cur;
    pm_int(v, 0x68);
    CHECK(in_pm() && cur.tf.eip == 0x11000 && r16(cur.tf.cs) == flat && cur.tf.user_esp == before.tf.user_esp - 12);
    pm_iret(v); CHECK(cur.tf.eip == before.tf.eip + 2);
    /* 32-bit exception frame: 32 bytes, dword fields, far return 8 bytes. */
    cur.tf.ebx = 0x0D; cur.tf.ecx = flat; cur.tf.edx = 0x12000; int31(v, 0x0203);
    before = cur;
    uint8_t lgdt[3] = {0x0F, 0x01, 0x10}; exec(v, lgdt, 3, 13, 0);
    CHECK(cur.tf.eip == 0x12000 && cur.tf.user_esp == DPMI_HSTACK_BYTES - 32);
    uint32_t ex[8]; CHECK(!dosvm_read(v, d->hstack.base + DPMI_HSTACK_BYTES - 32, ex, 32));
    CHECK(ex[0] == DPMI_PM_EXCRET && ex[1] == DPMI_HOST_CS && ex[3] == before.tf.eip && ex[4] == code && ex[6] == before.tf.user_esp);
    ex[3] += 3; CHECK(!dosvm_write(v, d->hstack.base + DPMI_HSTACK_BYTES - 32, ex, 32));
    pm_retf(v); at_stub(v, DPMI_PM_EXCRET);
    CHECK(cur.tf.eip == before.tf.eip + 3 && !d->depth);
    /* A 32-bit nested client: allocates, hooks, terminates; parent survives. */
    d->if_armed = true; /* parent has an unloaded flags image */
    struct v86_frame parent = cur;
    cur.tf.ebx = 0; cur.tf.ecx = 0x2000; int31(v, 0x0501); uint32_t pblock = ((uint32_t)r16(cur.tf.ebx) << 16) | r16(cur.tf.ecx);
    struct dpmi_vec pvec = d->vec[0x68];
    rmcs_set(v, 0x4B00, 0, 0x0202, 0, 0, 0, 0, 0); cur.tf.ebx = 0x21; cur.tf.ecx = 0; cur.tf.es = cur.tf.ds; cur.tf.edi = 0x200;
    int31(v, 0x0300); CHECK(!in_pm() && cur.tf.cs == DOS_SEG && d->depth == 1);
    struct v86_frame exec_call = cur;
    /* the child program starts and enters DPMI (same width) */
    v86_start(); cur.tf.cs = 0x2100; cur.tf.user_ss = 0x2200; cur.ds = 0x2200;
    uint16_t psp = PSP_SEG;   /* the fake DOS reports the same PSP; the child owns its own selectors */
    (void)psp;
    d->decode_stack_valid = d->decode_ds_valid = true; /* parent's snapshot is not the child's */
    enter(v, true);
    CHECK(in_pm() && d->clients == 2 && d->client[1].depth_base == 1);
    CHECK(!d->if_armed && d->client[0].suspended_if_armed);
    CHECK(!d->decode_stack_valid && !d->decode_ds_valid);
    cur.tf.ebx = 0; cur.tf.ecx = 0x3000; int31(v, 0x0501); CHECK(!cf());
    cur.tf.ebx = 0x68; cur.tf.ecx = cur.tf.cs; cur.tf.edx = 0x50; int31(v, 0x0205); CHECK(!cf() && d->vec[0x68].sel != pvec.sel);
    cur.tf.ds = cur.tf.cs; cur.tf.esi = 0x60; cur.tf.es = cur.tf.user_ss; cur.tf.edi = 0x100; int31(v, 0x0303); CHECK(!cf());
    unsigned child_sels = 0; for (unsigned i = 0; i < DPMI_LDT_ENTRIES; i++) child_sels += d->owner[i] == 2;
    CHECK(child_sels >= 4);
    /* Opposite width inside the child fails (no third client). */
    cur.tf.eax = 0x4C00; pm_int(v, 0x21);                                 /* child exits */
    CHECK(!in_pm() && cur.tf.cs == DOS_SEG && d->clients == 1 && d->depth == 1 && d->active);
    CHECK(d->if_armed); /* child cleanup preserves only the parent's armed image */
    CHECK(d->vec[0x68].sel == pvec.sel && d->vec[0x68].off == pvec.off && !d->client[1].used);
    for (unsigned i = 0; i < DPMI_LDT_ENTRIES; i++) CHECK(d->owner[i] != 2);
    CHECK(v->info.pool_used == DPMI_HSTACK_BYTES + 0x2000 && d->block[0].b.base == pblock);
    dos_service(v);                     /* the child's 4Ch */
    cur = exec_call; rm_iret_roundtrip_dos(v);                             /* EXEC returns to the parent */
    CHECK(in_pm() && !cf() && r16(cur.tf.cs) == r16(parent.tf.cs) && d->clients == 1 && !d->depth);
    /* Destroying the VM with suspended transitions frees everything. */
    rmcs_set(v, 0, 0, 0x0002, 0, IRET_SEG, 0, 0, 0); cur.tf.es = cur.tf.ds; cur.tf.edi = 0x200; int31(v, 0x0302);
    CHECK(!in_pm() && d->depth == 1);
    destroy(v, true); empty();
    CHECK(!d->active && !d->depth && !v->info.pool_used);
    printf("DPMI 32-bit client and nesting: 32-bit frames/offsets, nested client cleanup restores the parent, destroy unwinds\n");
}

static void test_dpmi_rm_soft(void)
{
    struct dosvm *v = dpmi_vm(true);
    struct dpmi *d = &v->dpmi;
    v86_start(); enter(v, false);
    uint16_t code = r16(cur.tf.cs);
    cur.tf.ebx = 0x24; cur.tf.ecx = code; cur.tf.edx = 0x7E0; int31(v, 0x0205);
    /* Real-mode INT 24h (DOS critical error) with a PM hook: the handler's
     * AL comes back to real mode. */
    rmcs_set(v, 0, 0, 0x0202, 0, 0x3000, 0, 0, 0); cur.tf.ecx = 0; cur.tf.es = cur.tf.ds; cur.tf.edi = 0x200;
    int31(v, 0x0301); CHECK(!in_pm() && cur.tf.cs == 0x3000);
    cur.tf.eax = 0x0000; v86_int(v, 0x24);
    CHECK(in_pm() && cur.tf.eip == 0x7E0 && r16(cur.tf.user_ss) == DPMI_HOST_SS && d->depth == 2);
    cur.tf.eax = 0x0003;                /* fail the call */
    pm_iret(v); at_stub(v, DPMI_PM_SOFTRET);
    CHECK(!in_pm() && cur.tf.cs == 0x3000 && cur.tf.eip == 2 && (uint8_t)cur.tf.eax == 3 && d->depth == 1);
    destroy(v, true); empty();
    printf("DPMI real-mode INT 1Ch/23h/24h reflection to protected-mode hooks: locked stack, results copied back\n");
}

static void test_dpmi(void)
{
    test_dpmi_discovery_entry();
    test_dpmi_ldt();
    test_dpmi_interrupts_translation();
    test_dpmi_termination();
    test_dpmi_callbacks_raw();
    test_dpmi_memory_misc();
    test_dpmi_dos_memory();
    test_dpmi_irq_exceptions();
    test_dpmi_io_watch();
    test_dpmi_nesting_chaining();
    test_dpmi_stale_frames();
    test_dpmi_32bit_nested();
    test_dpmi_rm_soft();
    (void)debug_dr7; (void)debug_addr; (void)op1; (void)rm_iret_roundtrip; (void)pm_iret; (void)pm_retf;
    (void)at_stub; (void)pm_push; (void)rm_pop16;
}
