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
     * without the disk it keeps the canonical payload and no DPMI host. The
     * production create path (syscall 72, the f3-06 lookup under the 2040-byte
     * kernel heap limit, f4-05 mixer attach with a ready backend), as
     * /bin/dosbox runs it on QEMU (vm-window, 2026-10-12: ENOMEM before). */
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
            v->vga.plane[v->video_direct - 1][linear - 0xA0000] = (uint8_t)f->tf.eax;
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

static void test_workloads(void)
{
    test_workload_wolf_frame();
    test_workload_verdict_screen();
    test_workload_pm_vga();
    test_workload_direct_window();
    test_workload_base_image();
}
