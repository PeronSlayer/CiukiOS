/* f3-06 kernel paths for the reference workloads, against the fake VM:
 *  - a DPMI client's #PF on the supervisor-only VGA window reaches the VGA
 *    model with its own 16/32-bit decoding and descriptor bases (DOOM writes
 *    its mode 13h/Y frames from 32-bit flat protected mode);
 *  - the direct plane window: single-plane write-mode-0 stores map the VM's
 *    window frames, preloaded with the plane, copied back by PTE dirty bit;
 *  - a VM booted from a further base image (dosvm_config.base).
 * Included by dosvm_test.c after the DPMI tests (uses their helpers).
 * SPDX-License-Identifier: GPL-2.0-only */
static uint32_t *window_pte(struct dosvm *v, uint32_t va)
{
    uint32_t *pd = table_va(v->as.pd_phys);
    CHECK(pd[va >> 22] & PTE_P);
    return &table_va(pd[va >> 22] & ~0xFFFu)[(va >> 12) & 1023];
}
/* Physical transport seam for native aperture cycles. Model planes live in
 * static host slots outside fake RAM; resolve their synthetic PTE frames
 * explicitly instead of assuming that video_direct names the PTE target. */
static uint8_t *window_memory(struct dosvm *v, unsigned offset)
{
    CHECK(offset < CVGA_PLANE_SIZE);
    uint32_t phys = as_lookup(&v->as, 0xA0000 + offset, 0);
    for (unsigned plane = 0; plane < 4; plane++)
        if ((phys & ~0xFFFu) == (plane_frame(v, plane, offset / PAGE_SIZE) & ~0xFFFu))
            return &v->vga.plane[plane][offset];
    CHECK(phys && phys < HOST_PAGES * PAGE_SIZE);
    return P2V(phys);
}
static void vga_out(struct dosvm *v, uint16_t port, uint8_t value) { port_out(v, port, 1, value); }
static void client_desc(struct dosvm *v, unsigned index, uint32_t base, uint32_t limit, uint8_t access, uint8_t ext)
{
    struct dpmi *d = &v->dpmi;
    CHECK(d->kind[index] == DPMI_FREE);
    dpmi_desc_set(d->ldt[index], base, limit, access, ext);
    d->kind[index] = DPMI_CLIENT; d->owner[index] = 1;
}

static void test_workload_pm_vga(void)
{
    struct dosvm *v = dpmi_vm(true);
    struct v86_frame mode = vf(0x0013, 0, 0, 0);
    CHECK(!dosvm_vga_bios(v, &mode));
    /* Force the original trapped path: changing only graphics scanout leaves
     * chain-4 read/write addressing and every write-mode-0 plane byte exact. */
    port_out(v, 0x3CE, 2, 0x0406);
    v86_start(); enter(v, false);
    CHECK(in_pm());
    /* 32-bit flat code and data, as DOS/4GW gives DOOM (base 0, 4 GiB, D=1). */
    client_desc(v, 900, 0, 0xFFFFFFFFu, 0xFA, 0x40);
    client_desc(v, 901, 0, 0xFFFFFFFFu, 0xF2, 0x40);
    /* A 16-bit client's segment over the window (base A0000h, 64 KiB). */
    client_desc(v, 902, 0x30000, 0xFFFF, 0xFA, 0);
    client_desc(v, 903, 0xA0000, 0xFFFF, 0xF2, 0);
    static const uint8_t code32[] = {
        0xC6, 0x05, 0x00, 0x00, 0x0A, 0x00, 0x5A,   /* 0: mov byte [0A0000h], 5Ah */
    };
    static const uint8_t stos32[] = {0xF3, 0xAB};   /* rep stosd */
    static const uint8_t code16[] = {0xC6, 0x06, 0x10, 0x00, 0x77};   /* mov byte [0010h], 77h */
    static const uint8_t load32[] = {0x8A, 0x05, 0x00, 0x00, 0x0A, 0x00}; /* mov al, [0A0000h] */
    CHECK(!dosvm_write(v, 0x30000, code32, sizeof(code32)) && !dosvm_write(v, 0x30010, stos32, sizeof(stos32)) &&
          !dosvm_write(v, 0x30020, code16, sizeof(code16)) && !dosvm_write(v, 0x30030, load32, sizeof(load32)));
    cur.tf.cs = DPMI_SEL(900); cur.tf.ds = cur.tf.es = DPMI_SEL(901); cur.tf.eip = 0x30000;
    fake_cr2 = 0xA0000;
    step(v, 14, 7);                                  /* P=1, W=1, U=1 */
    CHECK(!left && in_pm() && cur.tf.eip == 0x30007 && cvga_peek_vram(&v->vga, 0xA0000) == 0x5A);
    CHECK(v->video_pm_faults == 1 && v->video_faults == 1 && v->info.state < DOSVM_STOPPING);
    /* REP STOSD from 32-bit code: restartable after the string budget. */
    cur.tf.eip = 0x30010; cur.tf.edi = 0xA0000; cur.tf.ecx = 5000; cur.tf.eax = 0x11223344;
    step(v, 14, 7);
    CHECK(!left && cur.tf.eip == 0x30010 && cur.tf.ecx == 5000 - VIDEO_STRING_BUDGET);
    step(v, 14, 7);
    CHECK(!left && cur.tf.eip == 0x30012 && cur.tf.ecx == 0 && cur.tf.edi == 0xA0000 + 5000 * 4 &&
          cvga_peek_vram(&v->vga, 0xA0000 + 4999 * 4 + 3) == 0x11 && v->video_pm_faults == 3);
    /* 16-bit protected-mode code: 16-bit addressing, descriptor bases. */
    cur.tf.cs = DPMI_SEL(902); cur.tf.ds = DPMI_SEL(903); cur.tf.eip = 0x20;
    fake_cr2 = 0xA0010;
    step(v, 14, 7);
    CHECK(!left && cur.tf.eip == 0x25 && cvga_peek_vram(&v->vga, 0xA0010) == 0x77);
    /* A null data selector cannot have been loaded: the VM stops, not base 0. */
    cur.tf.cs = DPMI_SEL(900); cur.tf.ds = 0; cur.tf.eip = 0x30030;
    fake_cr2 = 0xA0000;
    step(v, 14, 5);
    CHECK(left && v->info.state == DOSVM_STOPPING && v->info.reason == DOSVM_FAULT && v->info.fault_vector == 14 &&
          v->video_pm_faults == 4);
    v->frame_pm = true;
    destroy(v, true); empty();
    printf("f3-06 DPMI VGA faults: 32-bit flat and 16-bit clients reach the VGA model, REP restart, null selector stops the VM\n");
}

static void test_workload_direct_window(void)
{
    struct dosvm *v = create();
    CHECK(!((uintptr_t)v->vga.plane & (PAGE_SIZE - 1)));   /* planes are whole pages */
    int10(v, 0x0013, 0, 0, 0);
    CHECK(v->video_direct == VIDEO_LINEAR);         /* f3-07: chain-4 uses its own counter */
    vga_out(v, 0x3C4, 4); vga_out(v, 0x3C5, 0x06);   /* SR04: chain-4 off, odd/even disabled (Mode Y) */
    vga_out(v, 0x3CE, 5); vga_out(v, 0x3CF, 0x40);   /* GC05: write mode 0, read mode 0 */
    vga_out(v, 0x3CE, 6); vga_out(v, 0x3CF, 0x05);   /* GC06: A0000h, 64 KiB, graphics */
    vga_out(v, 0x3D4, 0x14); vga_out(v, 0x3D5, 0x00); /* CRTC: dword mode off, byte mode on */
    vga_out(v, 0x3D4, 0x17); vga_out(v, 0x3D5, 0xE3); /* (DOOM's I_InitGraphics) */
    CHECK(!v->video_direct);                         /* map mask still 0Fh */
    vga_out(v, 0x3C4, 2); vga_out(v, 0x3C5, 0x02);   /* map mask: plane 1 only */
    CHECK(v->video_direct == 2 && v->video_maps == 1);
    for (unsigned i = 0; i < 16; i++) {
        uint32_t pte = *window_pte(v, 0xA0000 + i * PAGE_SIZE);
        CHECK((pte & (PTE_U | PTE_W)) == (PTE_U | PTE_W) && (pte & ~0xFFFu) == (plane_frame(v, 1, i) & ~0xFFFu));
    }
    CHECK(!(*window_pte(v, 0xB0000) & PTE_U));        /* B0000h-BFFFFh still trapped */
    /* A guest store: the CPU writes the plane page and sets its PTE dirty bit. */
    v->vga.plane[1][0x3010] = 0xAB;
    *window_pte(v, 0xA3000) |= DOSVM_PTE_DIRTY;
    memset(v->vga.dirty, 0, sizeof(v->vga.dirty));
    dosvm_vga_sync(v, false);
    CHECK((v->vga.dirty[1][0x3010 >> 6] & (1u << ((0x3010 >> 3) & 7))) && v->vga.dirty[1][0x3000 >> 6] == 0xFF &&
          !v->vga.dirty[1][0x2000 >> 6] && !v->vga.dirty[0][0x3000 >> 6]);
    CHECK(v->video_flushes == 1 && !(*window_pte(v, 0xA3000) & DOSVM_PTE_DIRTY) && v->video_direct == 2);
    dosvm_vga_sync(v, false);
    CHECK(v->video_flushes == 1);                    /* clean pages are not marked */
    /* Another plane: only the PTEs move; the old plane's stored pages are marked. */
    *window_pte(v, 0xA5000) |= DOSVM_PTE_DIRTY;
    vga_out(v, 0x3C5, 0x04);
    CHECK(v->video_direct == 3 && v->video_maps == 2 && v->video_flushes == 2 &&
          (*window_pte(v, 0xA5000) & ~0xFFFu) == (plane_frame(v, 2, 5) & ~0xFFFu) && v->vga.dirty[1][0x5000 >> 6] == 0xFF);
    /* Two planes, a latch copy (write mode 1) or a bit mask: the window closes. */
    vga_out(v, 0x3C5, 0x03);
    CHECK(!v->video_direct && !(*window_pte(v, 0xA0000) & PTE_U) && (*window_pte(v, 0xA0000) & ~0xFFFu) == v->frames[0xA0]);
    vga_out(v, 0x3C5, 0x01); CHECK(v->video_direct == 1);
    vga_out(v, 0x3CE, 5); vga_out(v, 0x3CF, 0x41); CHECK(!v->video_direct);
    vga_out(v, 0x3CF, 0x40); CHECK(v->video_direct == 1);
    vga_out(v, 0x3CE, 8); vga_out(v, 0x3CF, 0x0F); CHECK(!v->video_direct);
    vga_out(v, 0x3CF, 0xFF); CHECK(v->video_direct == 1);
    /* Kernel reads and writes of the window see the same plane memory. */
    v->vga.plane[0][0x40] = 0x99;
    uint8_t x = 0x55, y = 0;
    CHECK(!dosvm_write(v, 0xA0041, &x, 1) && v->vga.plane[0][0x41] == 0x55 && v->video_direct == 1);
    CHECK(!dosvm_read(v, 0xA0040, &y, 1) && y == 0x99);
    /* The presenter takes the stored pages with its dirty granules. */
    v->vga.plane[0][0x1000] = 0x3C;
    *window_pte(v, 0xA1000) |= DOSVM_PTE_DIRTY;
    unsigned before = v->video_flushes;
    struct dosvm_frame frame;
    CHECK(!dosvm_frame_digest(v, 0, &frame) && frame.width == 320 && frame.height == 200 && v->video_flushes == before + 1);
    /* Page flips are counted; geometry and counters are visible to probes. */
    vga_out(v, 0x3D4, 0x0C); vga_out(v, 0x3D5, 0x40);
    port_out(v, 0x3D4, 2, 0x400C);
    struct dosvm_video_info info;
    CHECK(dosvm_video_info(dosvm_key(v), &info) && info.starts == 2 && info.unchained && info.width == 320 &&
          info.height == 200 && info.maps == v->video_maps && info.flushes == v->video_flushes && info.mode == 0x13);
    /* A mode set closes the window; destruction restores the VM's own frames. */
    int10(v, 0x0093, 0, 0, 0);
    CHECK(v->video_direct == VIDEO_LINEAR && v->vga.plane[0][0x41] == 0x55);
    vga_out(v, 0x3C4, 4); vga_out(v, 0x3C5, 0x06); vga_out(v, 0x3CE, 5); vga_out(v, 0x3CF, 0x40);
    vga_out(v, 0x3C4, 2); vga_out(v, 0x3C5, 0x08);
    CHECK(v->video_direct == 4);
    destroy(v, true); empty();                       /* as_destroy frees only VM frames */
    printf("f3-06 direct plane window: single-plane stores mapped to the model, dirty pages marked, closed on latch/mask/mode changes\n");
}

static unsigned workload_waits;
static void release_workload_lookup(void)
{
    CHECK(cpu_flags & V86_IF);
    if (++workload_waits == 3) { workload_busy = false; sleep_hook = 0; }
}
/* Play only native memory cycles; every #PF and port cycle still uses the
 * production monitor. The PMM deliberately separates successive frames. */
static void linear_store(struct dosvm *v, unsigned offset, uint8_t byte)
{
    uint32_t address = 0xA0000 + offset, pte = *window_pte(v, address);
    CHECK(v->video_direct == VIDEO_LINEAR && (pte & (PTE_U | PTE_W)) == (PTE_U | PTE_W));
    uint32_t phys = as_lookup(&v->as, address, 0);
    CHECK(phys == v->frames[address / PAGE_SIZE] + (offset & (PAGE_SIZE - 1)));
    *(uint8_t *)P2V(phys) = byte;
    *window_pte(v, address) |= DOSVM_PTE_DIRTY;
}

static void test_workload_linear_closures(void)
{
    static const struct { uint16_t port, value; } changes[] = {
        {0x3C4, 0x0604}, {0x3C4, 0x0302},   /* chain-4, map mask */
        {0x3CE, 0x0101},                     /* enable set/reset */
        {0x3CE, 0x0103}, {0x3CE, 0x0803},   /* rotate, latch logical op */
        {0x3CE, 0x4105}, {0x3CE, 0x4805}, {0x3CE, 0x5005}, /* write/read mode, odd/even */
        {0x3CE, 0x0F08}, {0x3CE, 0x0006}, {0x3CE, 0x0906}, /* bit mask, aperture */
        {0x3D4, 0x0014}, {0x3B4, 0xE317},   /* CRTC address mode, both port pairs */
    };
    struct dosvm *v = create();
    for (unsigned c = 0; c < sizeof(changes) / sizeof(changes[0]) + 2; c++) {
        int10(v, 0x0013, 0, 0, 0);
        if (c < sizeof(changes) / sizeof(changes[0]) && changes[c].port == 0x3B4)
            port_out(v, 0x3C2, 1, v->vga.misc & ~1u); /* select the monochrome CRTC pair */
        CHECK(v->video_direct == VIDEO_LINEAR && cvga_direct_linear(&v->vga));
        unsigned harvests = v->video_linear_harvests;
        linear_store(v, 0x1237, (uint8_t)(0x80 + c));
        CHECK(!v->vga.plane[3][0x1234]);
        if (c < sizeof(changes) / sizeof(changes[0]))
            port_out(v, changes[c].port, 2, changes[c].value);
        else port_out(v, c == sizeof(changes) / sizeof(changes[0]) ? 0x3C2 : 0x3C3, 1, 0);
        CHECK(!v->video_direct && !cvga_direct_linear(&v->vga) && !(*window_pte(v, 0xA0000) & PTE_U));
        CHECK(v->vga.plane[3][0x1234] == (uint8_t)(0x80 + c) && v->video_linear_harvests == harvests + 1);
    }
    int10(v, 0x0013, 0, 0, 0);
    linear_store(v, 0x24, 0xAB);
    unsigned maps = v->video_linear_maps, harvests = v->video_linear_harvests;
    int10(v, 0x0093, 0, 0, 0);                     /* preserve VRAM: harvest before BIOS */
    CHECK(v->vga.plane[0][0x24] == 0xAB && v->video_linear_maps == maps + 1 &&
          v->video_linear_harvests == harvests + 1 && v->video_direct == VIDEO_LINEAR);
    linear_store(v, 0x25, 0xCD);
    int10(v, 0x0013, 0, 0, 0);                     /* clear VRAM: old shadow cannot resurrect it */
    CHECK(!v->vga.plane[0][0x24] && !v->vga.plane[1][0x24] &&
          !*(uint8_t *)P2V(as_lookup(&v->as, 0xA0025, 0)));
    linear_store(v, 0x26, 0xEF);
    int10(v, 0x4F02, 0x4103, 0, 0);
    CHECK(!v->video_direct && v->vga.plane[2][0x24] == 0xEF);
    dosvm_vga_direct(v);
    /* Kernel/model consumers close first, with peeks leaving latches alone. */
    linear_store(v, 0x27, 0xAA);
    uint8_t byte = 0, latches[4];
    memcpy(latches, v->vga.latch, sizeof(latches));
    CHECK(!dosvm_read(v, 0xA0027, &byte, 1) && byte == 0xAA && !v->video_direct &&
          !memcmp(latches, v->vga.latch, sizeof(latches)));
    dosvm_vga_direct(v); linear_store(v, 0x28, 0xBB); byte = 0xCC;
    CHECK(!dosvm_write(v, 0xA0029, &byte, 1) && !v->video_direct &&
          v->vga.plane[0][0x28] == 0xBB && v->vga.plane[1][0x28] == 0xCC);
    dosvm_vga_direct(v);
    /* Select write mode 1, THEN read to load all four latches for a copy. */
    port_out(v, 0x3CE, 2, 0x4105);
    CHECK(!v->video_direct);
    CHECK(video_read(v, 0xA0028, &byte) == 0 && byte == 0xBB);
    CHECK(video_write(v, 0xA0030, 0) == 0 && v->vga.plane[0][0x30] == 0xBB);
    port_out(v, 0x3CE, 2, 0x4005);
    CHECK(v->video_direct == VIDEO_LINEAR);
    linear_store(v, 0x2B, 0xDD);
    CHECK(!dosvm_stop(dosvm_key(v), DOSVM_TERMINATE) && !v->video_direct &&
          v->vga.plane[3][0x28] == 0xDD && !(*window_pte(v, 0xA0000) & PTE_U));
    dosvm_vga_direct(v); CHECK(!v->video_direct);     /* stopped VM cannot reopen */
    destroy(v, true); empty();
    printf("f3-07 linear window: port/BIOS/VBE/latch/consumer/stop closures harvest the old state, VM backing restored\n");
}

static void test_workload_linear_golden(void)
{
    struct dosvm *v = create();
    video_stage(v, 3);                            /* the exact golden mode/DAC configuration */
    static uint8_t zero[65536];
    CHECK(!dosvm_write(v, 0xA0000, zero, sizeof(zero)));
    dosvm_vga_direct(v);
    memset(v->vga.dirty, 0, sizeof(v->vga.dirty));
    unsigned harvests = v->video_linear_harvests;
    linear_store(v, 0x1237, 0xAB);
    dosvm_vga_sync(v, false);
    CHECK(v->video_direct == VIDEO_LINEAR && v->video_linear_harvests == harvests + 1 &&
          !(*window_pte(v, 0xA1000) & DOSVM_PTE_DIRTY));
    for (unsigned p = 0; p < 4; p++) for (unsigned i = 0; i < CVGA_DIRTY_BYTES; i++) {
        uint8_t expected = p == 3 && i == (0x1234 >> 6) ? (uint8_t)(1u << ((0x1234 >> 3) & 7)) : 0;
        CHECK(v->vga.dirty[p][i] == expected);      /* unchanged bytes/clean pages stay clean */
    }
    dosvm_vga_sync(v, false); CHECK(v->video_linear_harvests == harvests + 1);
    for (unsigned y = 0; y < 200; y++) for (unsigned x = 0; x < 320; x++)
        linear_store(v, y * 320 + x, (uint8_t)(x + y));
    struct dosvm_frame frame;
    char hex[65];
    CHECK(!dosvm_frame_digest(v, 0, &frame) && v->video_direct == VIDEO_LINEAR);
    sha256_hex(frame.digest, hex); CHECK(!strcmp(hex, video_golden[2]));
    video_stage(v, 4);
    CHECK(!dosvm_frame_digest(v, 0, &frame));
    sha256_hex(frame.digest, hex); CHECK(!strcmp(hex, video_golden[3]));
    destroy(v, true); empty();
    printf("f3-07 VMVIDEO native chained pattern: dirty granules exact, clean harvest idle, stage 3/4 goldens unchanged PASS\n");
}

static void test_workload_linear_replay(bool pm, bool window)
{
    struct dosvm *v = dpmi_vm(pm);
    int10(v, 0x0013, 0, 0, 0);
    v86_start();
    if (pm) {
        enter(v, false); CHECK(in_pm());
        client_desc(v, 900, 0, 0xFFFFFFFFu, 0xFA, 0x40);
        client_desc(v, 901, 0, 0xFFFFFFFFu, 0xF2, 0x40);
        cur.tf.cs = DPMI_SEL(900); cur.tf.ds = cur.tf.es = DPMI_SEL(901);
    } else { cur.tf.cs = 0x3000; cur.es = 0xA000; cur.ds = 0x3000; }
    /* MOV [ES:DI],AX (V86) or MOV [EDI],EAX (flat PM), followed by an
     * unsupported decoder opcode so the baseline V86 burst stops there. */
    static const uint8_t code16[] = {0x26, 0x89, 0x05, 0x90};
    static const uint8_t code32[] = {0x89, 0x07, 0x90};
    CHECK(!dosvm_write(v, 0x30000, pm ? code32 : code16, pm ? sizeof(code32) : sizeof(code16)));
    if (!window) port_out(v, 0x3CE, 2, 0x0406); /* only scanout changed; memory cycles identical */
    static cvga_state reference;
    reference = v->vga;
    /* Sentinels in non-chain-addressed bytes prove harvest does not compact
     * the model or overwrite holes, including the off-screen final page. */
    dosvm_vga_model(v);
    for (unsigned plane = 0; plane < 4; plane++) for (unsigned at = 1; at < CVGA_PLANE_SIZE; at += 4) {
        v->vga.plane[plane][at] = (uint8_t)(plane + at);
        reference.plane[plane][at] = (uint8_t)(plane + at);
    }
    if (window) dosvm_vga_direct(v);
    unsigned start_faults = v->video_faults, start_pm = v->video_pm_faults;
    unsigned size = pm ? 4 : 2, stores = 0;
    for (unsigned offset = 0; offset < CVGA_PLANE_SIZE; offset += size) {
        cur.tf.eip = pm ? 0x30000 : 0;
        cur.tf.edi = pm ? 0xA0000 + offset : offset;
        cur.tf.eax = 0xA5823100u ^ (offset * 0x01030507u);
        for (unsigned b = 0; b < size; b++) cvga_write_vram(&reference, 0xA0000 + offset + b, (uint8_t)(cur.tf.eax >> (b * 8)));
        if (!(*window_pte(v, 0xA0000 + offset) & PTE_U)) {
            fake_cr2 = 0xA0000 + offset;
            uint32_t eip = cur.tf.eip, eax = cur.tf.eax, edi = cur.tf.edi;
            step(v, 14, 7);
            CHECK(!left && v->info.state < DOSVM_STOPPING && in_pm() == pm);
            if (window) CHECK(cur.tf.eip == eip && cur.tf.eax == eax && cur.tf.edi == edi);
            else CHECK(cur.tf.eip == eip + (pm ? 2u : 3u));
        }
        if (window) for (unsigned b = 0; b < size; b++) linear_store(v, offset + b, (uint8_t)(cur.tf.eax >> (b * 8)));
        stores++;
        if (window && offset == 0x7FFC) dosvm_vga_sync(v, true); /* mid-frame close, next fault retries */
    }
    /* DAC and byte/word CRTC flips still trap, and do not remap. */
    unsigned maps = v->video_linear_maps;
    unsigned harvests = v->video_linear_harvests;
    port_out(v, 0x3D4, 2, 0x000C); port_out(v, 0x3D4, 2, 0x000D);
    if (window) CHECK(v->video_linear_harvests > harvests);
    port_out(v, 0x3C8, 1, 0x40); port_out(v, 0x3C9, 1, 1); port_out(v, 0x3C9, 1, 2); port_out(v, 0x3C9, 1, 3);
    CHECK(v->video_starts == 1 && v->vga.dac[0x40][2] == 3 && v->video_linear_maps == maps);
    struct dosvm_frame frame;
    if (!window) port_out(v, 0x3CE, 2, 0x0506);  /* restore standard scanout for the same frame */
    harvests = v->video_linear_harvests;
    CHECK(!dosvm_frame_digest(v, 0, &frame) && frame.width == 320 && frame.height == 200 && v->video_direct == VIDEO_LINEAR);
    CHECK(!memcmp(v->vga.plane, reference.plane, sizeof(reference.plane)));
    CHECK(v->video_linear_harvests == harvests); /* flip already published D pages */
    dosvm_vga_direct(v);
    uint8_t old_latches[4]; memcpy(old_latches, v->vga.latch, sizeof(old_latches));
    /* Read map select is ignored by chain-4; direct values match every
     * trapped read, including page edges and the unused tail of the frame. */
    for (unsigned map = 0; map < 4; map++) {
        port_out(v, 0x3CE, 2, (map << 8) | 4u);
        for (unsigned offset = 0; offset < CVGA_PLANE_SIZE; offset++) {
            uint8_t native = *(const uint8_t *)P2V(as_lookup(&v->as, 0xA0000 + offset, 0));
            CHECK(native == cvga_read_vram(&reference, 0xA0000 + offset));
        }
        CHECK(!memcmp(old_latches, v->vga.latch, sizeof(old_latches)));
    }
    struct dosvm_video_info info;
    CHECK(dosvm_video_info(dosvm_key(v), &info) && info.linear_maps == v->video_linear_maps &&
          info.linear_harvests == v->video_linear_harvests && !info.maps && !info.flushes);
    unsigned faults = v->video_faults - start_faults;
    CHECK(faults == (window ? 1u : stores) && v->video_pm_faults - start_pm == (pm ? faults : 0));
    /* Out-of-window cycles cannot retry into an unrelated mapping. */
    dosvm_vga_sync(v, true);
    CHECK(!dosvm_vga_retry(v, 0xB0000, 0x30000, pm) && !dosvm_vga_retry(v, 0xA0000, 0xA0000, pm));
    v->frame_pm = pm;
    destroy(v, true); empty(); fake_cr2 = 0xDEAD000;
    printf("f3-07 %s chained replay %s window: %u stores, %u faults, planes/read bytes exact, presenter harvest and retry PASS\n",
           pm ? "flat PM" : "V86", window ? "with" : "without", stores, faults);
}

static void test_workload_base_image(void)
{
    struct dosvm *v = 0;
    char path[24] = "x";
    workload_disk = false;
    CHECK(dosvm_workload_base(path) == -ENOENT && !path[0] && !heap_blocks && !tables);
    workload_disk = true;
    CHECK(!dosvm_workload_base(path) && !strcmp(path, "D:\\CIUKIGAM.IMG") && !heap_blocks && !tables);
    workload_busy = true; workload_waits = 0; sleep_hook = release_workload_lookup;
    CHECK(!dosvm_workload_base(path) && workload_waits == 3 && !workload_busy && !tables && !handles);
    io_fail_at = io_calls + 1;
    CHECK(dosvm_workload_base(path) == -V86_EIO && !path[0] && !workload_busy && !tables && !handles);
    io_fail_at = 0;
    for (unsigned i = 0; i < 64; i++) CHECK(!dosvm_workload_base(path) && !workload_busy && !tables && !handles && !heap_blocks);
    puts("f3-08 workload lookup: sleeping gate, init-error release, 64 table/handle/heap baselines PASS");
    struct dosvm_config c = {.commitment = DOSVM_LOW_CAP, .base = path};
    CHECK(dosvm_create(&c, &v) == 0 && v && v->base_open && !strcmp(last_base, "D:\\CIUKIGAM.IMG"));
    destroy(v, true); empty();
    c.base = "";
    CHECK(dosvm_create(&c, &v) == -EINVAL);
    empty();
    /* A window VM boots the attached workload disk with the staged DPMI host;
     * without the disk it keeps the canonical payload and no DPMI host. The
     * production create path (syscall 72, the f3-06 lookup under the 2040-byte
     * kernel heap limit, f4-05 mixer attach with a ready backend), as
     * /Programs/dosbox runs it on QEMU (vm-window, 2026-10-12: ENOMEM before). */
    struct process *saved_current = fake_current;
    fake_current = &fake_proc[0];
    struct ciuki_dosvm_command w;
    unsigned scans = workload_scans;
    vq_begin();
    /* Two D: stats: the lookup, then the base image open of the new VM. */
    CHECK(!create_window(0, "Games", &w) && workload_scans == scans + 2 && !workload_busy);
    v = dosvm_lookup((struct dosvm_key){w.id, w.generation});
    CHECK(v && v->dpmi_enabled && !strcmp(last_base, "D:\\CIUKIGAM.IMG"));
    struct audio_vm_stats st;
    struct dosvm_key wkey = {w.id, w.generation};
    CHECK(audio_vm_stats(wkey, &st) && st.state == AUDIO_VM_PENDING && ainfo(v).consumer);
    struct ciuki_dosvm_command shut = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE, &shut, sizeof(shut)));
    CHECK(audio_vm_stats(wkey, &st) && st.state == AUDIO_VM_FREE);
    empty();
    workload_disk = false;
    CHECK(!create_window(0, "Plain", &w) && !workload_busy);
    v = dosvm_lookup((struct dosvm_key){w.id, w.generation});
    CHECK(v && !v->dpmi_enabled && !strcmp(last_base, "C:\\SYSTEM\\DOS\\DOSBOOT.IMG"));
    wkey = (struct dosvm_key){w.id, w.generation};
    CHECK(audio_vm_stats(wkey, &st) && st.state == AUDIO_VM_PENDING && ainfo(v).consumer);
    shut = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE, &shut, sizeof(shut)));
    vq_end();
    empty();
    fake_current = saved_current;
    printf("f3-06 base image: further read-only image, workload disk lookup within the kernel heap limit, window VMs (syscall 72, mixer attached) boot it with the staged DPMI host\n");
}

/* Owner decision 2026-10-12: the bundled shareware games disk
 * (C:\GAMES\CIUKIGAM.IMG, scripts/build_games_disk.py) is the window VMs'
 * base when no local fixture disk is attached; the workload probes keep the
 * fixture only (their pins are the owner's data); a base cut after its last
 * used sector reads as zeros past its end. */
static void test_bundled_games_base(void)
{
    char path[24] = "x";
    struct dosvm *v = 0;
    workload_disk = false; bundled_disk = true; bundled_bytes = 4096u * 512u;
    unsigned scans = bundled_scans;
    CHECK(dosvm_workload_base(path) == -ENOENT && !path[0] && bundled_scans == scans && !tables);
    CHECK(!dosvm_window_base(path) && !strcmp(path, "C:\\GAMES\\CIUKIGAM.IMG") && bundled_scans == scans + 1);
    CHECK(!tables && !workload_busy && !heap_blocks);
    workload_disk = true;                                   /* the owner's local disk takes precedence */
    CHECK(!dosvm_window_base(path) && !strcmp(path, "D:\\CIUKIGAM.IMG") && bundled_scans == scans + 1);
    workload_disk = false;
    /* A cut base: no VFS read past its end, zeros instead; writes there go to the overlay. */
    struct dosvm_config c = {.commitment = DOSVM_LOW_CAP, .base = "C:\\GAMES\\CIUKIGAM.IMG"};
    CHECK(!dosvm_create(&c, &v) && v && v->base_sectors == 4096u);
    uint8_t got[512], bytes[512];
    unsigned reads = disk_reads;
    CHECK(!dosvm_disk_read(v, 4095, got) && got[0] == (uint8_t)(4095 % 251) && disk_reads == reads + 1);
    memset(got, 0x5A, sizeof(got));
    CHECK(!dosvm_disk_read(v, 4096, got) && disk_reads == reads + 1);
    for (unsigned i = 0; i < sizeof(got); i++) CHECK(!got[i]);
    memset(got, 0x5A, sizeof(got));
    CHECK(!dosvm_disk_read(v, DOSVM_SECTORS - 1, got) && !got[0] && !got[511] && disk_reads == reads + 1);
    memset(bytes, 0x3C, sizeof(bytes));
    CHECK(!dosvm_disk_write(v, 200000, bytes) && !dosvm_disk_read(v, 200000, got) && !memcmp(bytes, got, 512));
    CHECK(dosvm_disk_read(v, DOSVM_SECTORS, got) == -EINVAL);
    destroy(v, true); empty();
    /* Sizes outside the geometry: refused by the VM, absent for the lookup. */
    static const uint32_t bad[] = {DOSVM_SECTORS * 512u + 512u, (DOSVM_BASE_MIN_SECTORS - 1u) * 512u, 4096u * 512u + 1u, 0};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        bundled_bytes = bad[i];
        v = (void *)1;
        CHECK(dosvm_create(&c, &v) == -EINVAL && !v); empty();
        CHECK(dosvm_window_base(path) == -ENOENT && !path[0]);
    }
    bundled_bytes = DOSVM_BASE_MIN_SECTORS * 512u;
    CHECK(!dosvm_window_base(path));
    bundled_bytes = DOSVM_SECTORS * 512u;                   /* an uncut image is valid too */
    CHECK(!dosvm_window_base(path));
    bundled_directory = true;
    CHECK(dosvm_window_base(path) == -ENOENT && !path[0]);
    bundled_directory = false;
    /* Window VM (syscall 72): the bundled disk with the staged DPMI host. */
    bundled_bytes = 4096u * 512u;
    struct process *saved_current = fake_current;
    fake_current = &fake_proc[0];
    struct ciuki_dosvm_command w;
    vq_begin();
    CHECK(!create_window(0, "Games", &w) && !workload_busy);
    v = dosvm_lookup((struct dosvm_key){w.id, w.generation});
    CHECK(v && v->dpmi_enabled && v->base_sectors == 4096u && !strcmp(last_base, "C:\\GAMES\\CIUKIGAM.IMG"));
    struct ciuki_dosvm_command shut = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE, &shut, sizeof(shut)));
    vq_end();
    empty();
    fake_current = saved_current;
    bundled_disk = false; bundled_bytes = DOSVM_SECTORS * 512u;
    printf("bundled games disk: window VMs use C:\\GAMES\\CIUKIGAM.IMG after the local fixture disk (probes: fixture only), a cut base reads zeros past its end, invalid sizes refused\n");
}

/* The verdict line latches the text screen: a child's output that the shell
 * later overwrites (its row-0 title bar) stays readable (QEMU zz-doom). */
static void text_row(struct dosvm *v, unsigned row, const char *text)
{
    uint16_t cells[80];
    bool end = false;
    for (unsigned i = 0; i < 80; i++) { end = end || !text[i]; cells[i] = 0x0700 | (end ? ' ' : (uint8_t)text[i]); }
    CHECK(!dosvm_write(v, 0xB8000 + row * 160, cells, sizeof(cells)));
}
static void test_workload_verdict_screen(void)
{
    struct dosvm *v = create();
    dosvm_expect(v, "RUNREC");
    CHECK(!dosvm_verdict_text(v));
    text_row(v, 0, "timed 1710 gametics in 15190 realtics");
    text_row(v, 1, "RUNREC FAIL R01 T00");
    line(v, "RUNREC FAIL R01 T00");
    const uint16_t *latched = dosvm_verdict_text(v);
    CHECK(latched && v->info.verdict_done && !v->info.verdict_pass && !strcmp(v->info.verdict_detail, "R01_T00"));
    CHECK((uint8_t)latched[0] == 't' && (uint8_t)latched[80] == 'R');
    text_row(v, 0, "                             CiukiOS pre-Alpha v0.8.3");
    CHECK((uint8_t)latched[0] == 't' && (uint8_t)dosvm_text(v, 0)[29] == 'C');
    line(v, "RUNREC PASS");                              /* a verdict latches once */
    CHECK(dosvm_verdict_text(v) == latched && (uint8_t)latched[0] == 't');
    dosvm_expect(v, "RUNREC");
    CHECK(!dosvm_verdict_text(v));
    line(v, "OTHER PASS");
    CHECK(!dosvm_verdict_text(v) && !v->info.verdict_done);
    line(v, "RUNREC PASS");
    CHECK(dosvm_verdict_text(v) && v->info.verdict_pass && (uint8_t)dosvm_verdict_text(v)[29] == 'C');
    destroy(v, true); empty();
    printf("f3-06 verdict screen: latched at the payload's verdict line, kept over the shell redraw, reset by dosvm_expect\n");
}

/* f3-06 QEMU finding (wolf3d-qemu-t23, 2026-10-12): the play stage saw no
 * page flip in 3 s and 1.14 million emulated aperture stores. Replay of one
 * Wolf3D 3D-view frame in Mode Y, as WL_DRAW.C ScalePost and WL_SCALE.C
 * BuildCompScale produce it: per wall strip an OUT 3C5h with the strip's
 * map mask (mapmasks1-3), then a FAR CALL into a compiled scaler, a straight
 * run of "mov al,[si+d8]" / "mov es:[di+d16],al" ending in RETF; then
 * ThreeDRefresh's flip (OUT 3D4h,0Ch; OUT 3D5h,high). The harness is the
 * CPU for what does not trap: the scaler's loads, stores into a writable
 * mapped window page (setting its dirty bit), the call and RETF. What traps
 * on hardware enters the production monitor through dosvm_trap: each OUT
 * (#GP) and each aperture store without a writable mapping (#PF). */
static const uint8_t wolf_masks1[4][8] = {{1,3,7,15,15,15,15,15},{2,6,14,14,14,14,14,14},{4,12,12,12,12,12,12,12},{8,8,8,8,8,8,8,8}};
static const uint8_t wolf_masks2[4][8] = {{0,0,0,0,1,3,7,15},{0,0,0,1,3,7,15,15},{0,0,1,3,7,15,15,15},{0,1,3,7,15,15,15,15}};
static const uint8_t wolf_masks3[4][8] = {{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,1},{0,0,0,0,0,0,1,3},{0,0,0,0,0,1,3,7}};
#define WOLF_VIEW_HEIGHT 152
#define WOLF_CODE 0x20000u
#define WOLF_TEXTURE 0x30000u
static unsigned wolf_traps, wolf_pf;
/* WL_SCALE.C BuildCompScale for one wall height, into guest RAM. */
static void wolf_build_scaler(struct dosvm *v, int height)
{
    uint8_t code[4096]; unsigned n = 0;
    long step = ((long)height << 16) / 64, fix = 0;
    int toppix = (WOLF_VIEW_HEIGHT - height) / 2;
    for (int src = 0; src <= 64; src++) {
        int startpix = (int)(fix >> 16); fix += step; int endpix = (int)(fix >> 16);
        startpix += toppix; endpix += toppix;
        if (startpix == endpix || endpix < 0 || startpix >= WOLF_VIEW_HEIGHT || src == 64) continue;
        code[n++] = 0x8A; code[n++] = 0x44; code[n++] = (uint8_t)src;
        for (; startpix < endpix; startpix++) {
            if (startpix >= WOLF_VIEW_HEIGHT) break;
            if (startpix < 0) continue;
            unsigned ofs = (unsigned)startpix * 80;
            code[n++] = 0x26; code[n++] = 0x88; code[n++] = 0x85; code[n++] = (uint8_t)ofs; code[n++] = (uint8_t)(ofs >> 8);
        }
    }
    code[n++] = 0xCB;
    CHECK(n <= sizeof(code) && !dosvm_write(v, WOLF_CODE, code, n));
}
static void wolf_out(struct dosvm *v, struct v86_frame *f, uint16_t port, uint8_t value)
{
    static const uint8_t out_dx_al = 0xEE;
    f->tf.cs = 0x1000; f->tf.eip = 0; f->tf.edx = port;
    CHECK(device_insn(v, f, value, &out_dx_al, 1) && f->tf.eip == 1);
    wolf_traps++;
}
/* CALL DWORD PTR [bp] into the scaler with ES:DI at the strip byte. */
static void wolf_call_scaler(struct dosvm *v, struct v86_frame *f, uint16_t di)
{
    f->tf.cs = WOLF_CODE >> 4; f->tf.eip = 0; f->tf.edi = di; f->tf.esi = 0;
    f->ds = WOLF_TEXTURE >> 4; f->es = 0xA000;
    for (unsigned steps = 0; steps < 100000; steps++) {
        uint8_t op[5] = {0};
        CHECK(!dosvm_read(v, (f->tf.cs << 4) + f->tf.eip, op, sizeof(op)));
        if (op[0] == 0xCB) return;                                   /* RETF */
        if (op[0] == 0x8A && op[1] == 0x44) {                        /* mov al,[si+d8] */
            uint8_t al = 0;
            CHECK(!dosvm_read(v, (f->ds << 4) + (uint16_t)(f->tf.esi + op[2]), &al, 1));
            f->tf.eax = (f->tf.eax & ~0xFFu) | al; f->tf.eip += 3; continue;
        }
        CHECK(op[0] == 0x26 && op[1] == 0x88 && op[2] == 0x85);     /* mov es:[di+d16],al */
        uint32_t linear = (f->es << 4) + (uint16_t)(f->tf.edi + (op[3] | (unsigned)op[4] << 8));
        uint32_t *pte = window_pte(v, linear);
        if ((*pte & (PTE_U | PTE_W)) == (PTE_U | PTE_W)) {           /* the CPU stores into the mapped plane */
            CHECK(v->video_direct && linear < 0xB0000);
            *window_memory(v, linear - 0xA0000) = (uint8_t)f->tf.eax;
            *pte |= DOSVM_PTE_DIRTY; f->tf.eip += 5; continue;
        }
        fake_cr2 = linear; CHECK(device_trap(v, f, 14)); wolf_traps++; wolf_pf++;   /* #PF */
    }
    CHECK(false);
}
/* One 304-pixel view of strips of `width` pixels from a scaler of `height`. */
static void wolf_frame(struct dosvm *v, struct v86_frame *f, int height, unsigned width, uint16_t bufferofs, uint8_t ref[4][16384])
{
    wolf_build_scaler(v, height);
    wolf_out(v, f, 0x3C4, 2);                                        /* SC_INDEX: map mask */
    for (unsigned postx = 0; postx < 304; postx += width) {
        unsigned w = postx + width <= 304 ? width : 304 - postx, plane = postx & 3;
        const uint8_t masks[3] = {wolf_masks1[plane][w - 1], wolf_masks2[plane][w - 1], wolf_masks3[plane][w - 1]};
        uint16_t di = (uint16_t)(postx / 4 + bufferofs);
        for (unsigned k = 0; k < 3 && masks[k]; k++, di++) {
            wolf_out(v, f, 0x3C5, masks[k]);
            wolf_call_scaler(v, f, di);
            /* Reference: the stored texel in every plane of the mask. */
            long step = ((long)height << 16) / 64, fix = 0;
            int toppix = (WOLF_VIEW_HEIGHT - height) / 2;
            for (int src = 0; src < 64; src++) {
                int a = (int)(fix >> 16) + toppix; fix += step; int b = (int)(fix >> 16) + toppix;
                for (int y = a < 0 ? 0 : a; y < b && y < WOLF_VIEW_HEIGHT; y++)
                    for (unsigned p = 0; p < 4; p++) if (masks[k] & (1u << p))
                        ref[p][(uint16_t)(di + y * 80) & 16383] = (uint8_t)(src * 3 + 7);
            }
        }
    }
    wolf_out(v, f, 0x3D4, 0x0C); wolf_out(v, f, 0x3D5, (uint8_t)(bufferofs >> 8));  /* ThreeDRefresh flip */
}
static void test_workload_wolf_frame(void)
{
    static uint8_t ref[4][16384];
    struct dosvm *v = create();
    CHECK(!dosvm_boot(v)); v->info.dos_ready = true;
    int10(v, 0x0013, 0, 0, 0);
    vga_out(v, 0x3C4, 4); vga_out(v, 0x3C5, 0x06);                     /* VL_DePlaneVGA */
    vga_out(v, 0x3D4, 0x14); vga_out(v, 0x3D5, 0x00); vga_out(v, 0x3D4, 0x17); vga_out(v, 0x3D5, 0xE3);
    uint8_t texture[64];
    for (unsigned i = 0; i < 64; i++) texture[i] = (uint8_t)(i * 3 + 7);
    CHECK(!dosvm_write(v, WOLF_TEXTURE, texture, sizeof(texture)));
    struct v86_frame f = {0};
    f.tf.user_ss = 0x9000; f.tf.user_esp = 0xFFF0; f.tf.eflags = V86_VM | V86_IF | 2;
    v->monitor.vif = false;                                          /* no IRQ in the walk */
    static const struct { int height; unsigned width; const char *name; } views[] = {
        {300, 4, "near wall, 4-pixel strips"}, {100, 2, "mid wall, 2-pixel strips"}, {50, 1, "far wall, 1-pixel strips"}};
    unsigned total_traps = 0, total_faults = 0;
    for (unsigned i = 0; i < 3; i++) {
        memset(ref, 0, sizeof(ref));
        for (unsigned p = 0; p < 4; p++) memset(v->vga.plane[p], 0, 16384);
        dosvm_vga_sync(v, false);
        unsigned traps = wolf_traps, pf = wolf_pf, faults = v->video_faults, maps = v->video_maps,
                 burst = v->video_burst, starts = v->video_starts;
        wolf_frame(v, &f, views[i].height, views[i].width, 0, ref);
        dosvm_vga_sync(v, false);
        printf("Wolf3D frame replay, %s: %u traps (%u #PF), %u emulated aperture faults, %u burst instructions, %u window maps, %u flips\n",
               views[i].name, wolf_traps - traps, wolf_pf - pf, v->video_faults - faults,
               v->video_burst - burst, v->video_maps - maps, v->video_starts - starts);
        CHECK(v->video_starts == starts + 1);                        /* the flip is counted */
        for (unsigned p = 0; p < 4; p++) CHECK(!memcmp(v->vga.plane[p], ref[p], 16384));
        total_traps += wolf_traps - traps; total_faults += v->video_faults - faults;
    }
    /* At most one #PF per strip byte (each scaler call), not one per pixel. */
    CHECK(wolf_pf <= 3 * 304 && total_faults <= 3 * 304);
    destroy(v, true); empty();
    printf("f3-06 Wolf3D frame replay: ScalePost strips and compiled scalers through dosvm_trap, planes exact, flips counted, %u traps for three views\n", total_traps);
}
/* f3-06 POPF compatibility (DOOM's DMX on QEMU): a CLI right after a flags
 * save steps the client; PUSHF/POPF are emulated on the virtual IF. */
static void test_workload_popf_compat(void)
{
    struct dosvm *v = dpmi_vm(true);
    v86_start(); enter(v, false);
    CHECK(in_pm());
    struct dpmi *d = &v->dpmi;
    /* 200 pushf; 201 pop ax; 202 cli; 203 push ax; 204 popf; 205 nop;
     * 206 pushf; 207 cli; 208 popf; 209 cli; 20A pushf; 20B cli */
    static const uint8_t code[] = {0x9C, 0x58, 0xFA, 0x50, 0x9D, 0x90, 0x9C, 0xFA, 0x9D, 0xFA, 0x9C, 0xFA};
    CHECK(!dosvm_write(v, seg_base(v, (uint16_t)cur.tf.cs) + 0x200, code, sizeof(code)));
    cur.tf.eflags = (cur.tf.eflags & ~0x0DD5u) | V86_ZF | V86_IF | 2u;
    uint16_t image = (uint16_t)((cur.tf.eflags & 0x0CD5u) | V86_IF | 2u);   /* PUSHF of the real flags */
    /* pushf; pop ax ran natively; the CLI traps: the image in AX gets TF
     * (the virtual IF was set), nothing is stepped. */
    v->monitor.vif = true;
    cur.tf.eax = 0xABCD0000u | image;
    cur.tf.eip = 0x200; step(v, 0x20, 0); /* establish a native instruction boundary */
    cur.tf.eip = 0x202;
    step(v, 13, 0);
    CHECK(!left && in_pm() && !v->monitor.vif && cur.tf.eip == 0x203 && !(cur.tf.eflags & V86_TF));
    CHECK(cur.tf.eax == (0xABCD0000u | image | V86_TF) && d->if_armed && d->stats.if_armed == 1);
    /* push ax; popf ran natively and loaded TF; the single-step trap after
     * the NOP restores the virtual IF and clears TF, no client exception. */
    unsigned exceptions = d->stats.exceptions;
    cur.tf.eflags |= V86_TF; cur.tf.eip = 0x206;
    debug_dr6 = 0x4000; step(v, 1, 0);
    CHECK(!left && v->monitor.vif && !(cur.tf.eflags & V86_TF) && cur.tf.eip == 0x206);
    CHECK(d->stats.if_restored == 1 && d->stats.exceptions == exceptions);
    /* pushf; cli: the image on the stack top gets TF. */
    pm_push(v, image); cur.tf.eip = 0x207;
    step(v, 13, 0);
    CHECK(!left && !v->monitor.vif && cur.tf.eip == 0x208 && d->stats.if_armed == 2);
    uint32_t top = 0;
    struct dpmi_seg ss; CHECK(dpmi_seg(v, (uint16_t)cur.tf.user_ss, &ss));
    CHECK(!dosvm_read(v, ss.base + (uint16_t)cur.tf.user_esp, &top, 2) && top == (uint32_t)(image | V86_TF));
    /* popf ran natively (TF loaded); the next instruction is a CLI that
     * traps first: the restore is applied before the CLI, which then
     * disables again; the popf before it does not arm anything. */
    (void)pm_pop(v); cur.tf.eflags |= V86_TF; cur.tf.eip = 0x209;
    step(v, 13, 0);
    CHECK(!left && !v->monitor.vif && !(cur.tf.eflags & V86_TF) && cur.tf.eip == 0x20A);
    CHECK(d->stats.if_restored == 2 && d->stats.if_armed == 2);
    /* Saved while virtually disabled (a nested section): left alone, its
     * restore keeps the client disabled. */
    pm_push(v, image); cur.tf.eip = 0x20B;
    step(v, 13, 0);
    CHECK(!left && !v->monitor.vif && d->stats.if_armed == 2);
    CHECK(!dosvm_read(v, ss.base + (uint16_t)cur.tf.user_esp, &top, 2) && top == image);
    /* A stack top that is not PUSHF's image of the current flags is never
     * touched; neither is a CLI without a flags save before it. */
    CHECK(!dosvm_write(v, ss.base + (uint16_t)cur.tf.user_esp, &(uint16_t){0x1234}, 2));
    v->monitor.vif = true; cur.tf.eip = 0x20B;
    step(v, 13, 0);
    CHECK(!left && !v->monitor.vif && d->stats.if_armed == 2);
    CHECK(!dosvm_read(v, ss.base + (uint16_t)cur.tf.user_esp, &top, 2) && top == 0x1234);
    (void)pm_pop(v);
    v->monitor.vif = true; cur.tf.eip = 0x209;
    step(v, 13, 0);
    CHECK(!left && !v->monitor.vif && d->stats.if_armed == 2 && !(cur.tf.eflags & V86_TF));
    v->frame_pm = true;
    destroy(v, true); empty();
    printf("f3-06 POPF compatibility: flags image armed at CLI (stack and POP r), POPF trap restores the virtual IF, nested and foreign images untouched\n");
}

#include "dosvm_review_test.inc"
static void test_workloads(void)
{
    test_review_vm();
    if (getenv("F3_REVIEW")) return;
    test_workload_linear_closures();
    test_workload_linear_golden();
    test_workload_linear_replay(false, false);
    test_workload_linear_replay(false, true);
    test_workload_linear_replay(true, false);
    test_workload_linear_replay(true, true);
    test_workload_wolf_frame();
    test_workload_popf_compat();
    test_workload_verdict_screen();
    test_workload_pm_vga();
    test_workload_direct_window();
    test_workload_base_image();
    test_bundled_games_base();
}
