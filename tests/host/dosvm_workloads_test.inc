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
    CHECK(!v->video_direct);                         /* chain-4: every store traps */
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
    CHECK(!v->video_direct && v->vga.plane[0][0x41] == 0x55);
    vga_out(v, 0x3C4, 4); vga_out(v, 0x3C5, 0x06); vga_out(v, 0x3CE, 5); vga_out(v, 0x3CF, 0x40);
    vga_out(v, 0x3C4, 2); vga_out(v, 0x3C5, 0x08);
    CHECK(v->video_direct == 4);
    destroy(v, true); empty();                       /* as_destroy frees only VM frames */
    printf("f3-06 direct plane window: single-plane stores mapped to the model, dirty pages marked, closed on latch/mask/mode changes\n");
}

static void test_workload_base_image(void)
{
    struct dosvm *v = 0;
    char path[24] = "x";
    workload_disk = false;
    CHECK(dosvm_workload_base(path) == -ENOENT && !path[0] && !heap_blocks && !tables);
    workload_disk = true;
    CHECK(!dosvm_workload_base(path) && !strcmp(path, "D:\\CIUKIGAM.IMG") && !heap_blocks && !tables);
    struct dosvm_config c = {.commitment = DOSVM_LOW_CAP, .base = path};
    CHECK(dosvm_create(&c, &v) == 0 && v && v->base_open && !strcmp(last_base, "D:\\CIUKIGAM.IMG"));
    destroy(v, true); empty();
    c.base = "";
    CHECK(dosvm_create(&c, &v) == -EINVAL);
    empty();
    /* A window VM boots the attached workload disk with the staged DPMI host;
     * without the disk it keeps the canonical payload and no DPMI host. */
    struct process *saved_current = fake_current;
    fake_current = &fake_proc[0];
    struct ciuki_dosvm_command w;
    unsigned scans = workload_scans;
    /* Two D: stats: the lookup, then the base image open of the new VM. */
    CHECK(!create_window(0, "Games", &w) && workload_scans == scans + 2);
    v = dosvm_lookup((struct dosvm_key){w.id, w.generation});
    CHECK(v && v->dpmi_enabled && !strcmp(last_base, "D:\\CIUKIGAM.IMG"));
    struct ciuki_dosvm_command shut = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE, &shut, sizeof(shut)));
    empty();
    workload_disk = false;
    CHECK(!create_window(0, "Plain", &w));
    v = dosvm_lookup((struct dosvm_key){w.id, w.generation});
    CHECK(v && !v->dpmi_enabled && !strcmp(last_base, "C:\\SYSTEM\\DOS\\DOSBOOT.IMG"));
    shut = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE, &shut, sizeof(shut)));
    empty();
    fake_current = saved_current;
    printf("f3-06 base image: further read-only image, workload disk lookup, window VMs boot it with the staged DPMI host\n");
}

static void test_workloads(void)
{
    test_workload_pm_vga();
    test_workload_direct_window();
    test_workload_base_image();
}
