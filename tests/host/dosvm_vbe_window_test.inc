/* war2vbe (2026-10-25): VBE modes on a display without a native mode
 * provider (display_fullscreen_supported() false, as on QEMU std VGA,
 * VirtualBox and VMware). Before this change 4F00h/4F01h/4F02h failed there
 * (014Fh) and the Warcraft II demo stopped with "could not find a VESA video
 * driver". Checks the VBE BIOS answers (VESA BIOS Extension Core Functions
 * 2.0/3.0: 4F00h info block, 4F01h mode information with window A and
 * WinFuncPtr, 4F02h windowed and linear models, 4F03h, 4F05h window
 * control, 4F0Ah protected-mode table; the window function and the table's
 * palette code on the CPU model), the bank window aliasing the VM's VBE
 * buffer, its dirty pages
 * reaching the window presenter with the VM's DAC palette, kernel stores
 * through the window, the VGA BIOS mode return, teardown with the window
 * open, the native full-screen flush of a windowed mode, and (101-dosvid2
 * merge) the direct LFB lease taken by linear modes only, its copy-back
 * visible through the bank window after a windowed mode set. Included by
 * dosvm_test.c after dosvm_fullscreen_test.inc. SPDX-License-Identifier: GPL-2.0-only */
static void vbe_window_store(struct dosvm *v, unsigned offset, uint8_t byte)
{
    /* A native guest store through the open window: the CPU writes the
     * mapped frame and sets the window PTE's D bit (Intel SDM Vol. 3A 4.8). */
    uint32_t address = 0xA0000 + offset, pte = *window_pte(v, address);
    CHECK(v->video_direct == VIDEO_BANK && (pte & (PTE_U | PTE_W)) == (PTE_U | PTE_W));
    *(uint8_t *)P2V(as_lookup(&v->as, address, 0)) = byte;
    *window_pte(v, address) |= DOSVM_PTE_DIRTY;
}
static uint32_t vbe_window_pixel(unsigned x, unsigned y)
{ uint32_t p; memcpy(&p, frame_buffer + (y * DOSVM_VIDEO_WIDTH + x) * 4, 4); return p & 0xFFFFFFu; }
static void vbe_window_maps(struct dosvm *v, unsigned bank)
{
    CHECK(v->video_direct == VIDEO_BANK && v->vbe_bank_mapped == bank);
    for (unsigned page = 0; page < 16; page++) {
        uint32_t pte = *window_pte(v, 0xA0000 + page * PAGE_SIZE);
        CHECK((pte & ~0xFFFu) == dosvm_vbe_frame(v, bank * 16 + page) && (pte & (PTE_U | PTE_W)) == (PTE_U | PTE_W));
    }
}
#ifdef CIUKI_DOSVM_GUEST_CPU
/* The 4F01h WinFuncPtr on the CPU model with the production V86 monitor: a
 * VBE 1.2-style caller loads only BH/BL/DX (AX is not 4F05h), far calls
 * F000:0104 (MOV AX,4F05h; PUSHF; CALL FAR F000:0100; RETF, whose INT FEh
 * reaches the VBE BIOS), then stores through A000h into the new bank. */
static void vbe_window_function_cpu(void)
{
    const struct gcpu_ops ops = {if_cpu_read, if_cpu_write, if_cpu_descriptor, if_cpu_unexpected, 0};
    gcpu_init(&ops);
    struct dosvm *v = dpmi_vm(false); v86_start(); if_cpu_vm = v;
    int10(v, 0x4F02, 0x0101, 0, 0);
    CHECK(v->vbe_mode == 0x101 && !v->vbe_linear && v->video_direct == VIDEO_BANK && !v->vbe_bank);
    const uint8_t code[] = {0xB8, 0x34, 0x12,                    /* mov ax,1234h */
                            0x31, 0xDB,                          /* xor bx,bx: set window A */
                            0xBA, 0x03, 0x00,                    /* mov dx,3 */
                            0x9A, 0x04, 0x01, 0x00, 0xF0,        /* call far F000:0104 */
                            0x89, 0xC6,                          /* mov si,ax */
                            0xB8, 0x00, 0xA0, 0x8E, 0xC0,        /* mov ax,A000h; mov es,ax */
                            0x26, 0xC6, 0x06, 0x05, 0x00, 0x77,  /* mov byte [es:5],77h */
                            0xF4};                               /* hlt */
    CHECK(!dosvm_write(v, cs_linear(v), code, sizeof(code)));
    unsigned traps = 0;
    for (;;) {
        struct gcpu_state st = {.eax=cur.tf.eax, .ebx=cur.tf.ebx, .ecx=cur.tf.ecx, .edx=cur.tf.edx,
            .esi=cur.tf.esi, .edi=cur.tf.edi, .ebp=cur.tf.ebp, .esp=cur.tf.user_esp, .eip=cur.tf.eip,
            .eflags=cur.tf.eflags, .cs=(uint16_t)cur.tf.cs, .ss=(uint16_t)cur.tf.user_ss,
            .ds=(uint16_t)cur.ds, .es=(uint16_t)cur.es, .fs=(uint16_t)cur.fs, .gs=(uint16_t)cur.gs, .pm=false};
        struct gcpu_event ev;
        int rc = gcpu_run(&st, 64, &ev);
        cur.tf.eax=st.eax; cur.tf.ebx=st.ebx; cur.tf.ecx=st.ecx; cur.tf.edx=st.edx;
        cur.tf.esi=st.esi; cur.tf.edi=st.edi; cur.tf.ebp=st.ebp; cur.tf.user_esp=st.esp;
        cur.tf.eip=st.eip; cur.tf.cs=st.cs; cur.tf.user_ss=st.ss; cur.tf.eflags=st.eflags|V86_VM;
        cur.ds=st.ds; cur.es=st.es; cur.fs=st.fs; cur.gs=st.gs;
        CHECK(rc == GCPU_TRAP && ev.vector == 13 && ++traps < 16);
        uint8_t op = 0; CHECK(!dosvm_read(v, cs_linear(v), &op, 1));
        if (op == 0xF4 && cur.tf.cs == CODE_SEG) break;
        step(v, ev.vector, ev.err);
    }
    uint8_t byte = 0;
    CHECK(cur.tf.eip == 0x100 + sizeof(code) - 1 && (uint16_t)cur.tf.esi == 0x004F && v->vbe_bank == 3);
    CHECK(!dosvm_read(v, v->vbe_buffer.base + 3 * 65536 + 5, &byte, 1) && byte == 0x77);
    vbe_window_maps(v, 3);
    destroy(v, true); empty();
    /* The 4F0Ah SetPrimaryPalette code run in place from the ROM table by a
     * 32-bit DPMI client: CX entries (ECX high half ignored) from DX, ES:EDI
     * blue/green/red/0, through the trapped DAC ports. */
    v = dpmi_vm(true); v86_start(); enter(v, true); if_cpu_vm = v;
    CHECK(in_pm());
    client_desc(v, 900, 0, 0xFFFFF, 0xFA, 0xC0);                  /* flat 32-bit code, 4 GiB */
    client_desc(v, 901, 0, 0xFFFFF, 0xF2, 0xC0);                  /* flat data for ES:EDI */
    uint8_t table[8]; CHECK(!dosvm_read(v, 0xF0000 + DOSVM_VBE_PM_TABLE, table, sizeof(table)));
    uint8_t hlt = 0xF4, entries[8] = {1, 2, 3, 0, 4, 5, 6, 0};
    CHECK(!dosvm_write(v, 0x30300, &hlt, 1) && !dosvm_write(v, 0x30200, entries, sizeof(entries)));
    pm_push(v, 0x30300);                                         /* near return to the HLT */
    cur.tf.cs = DPMI_SEL(900); cur.tf.eip = 0xF0000 + DOSVM_VBE_PM_TABLE + vm_u16(table + 4);
    cur.tf.es = DPMI_SEL(901); cur.tf.edi = 0x30200; cur.tf.ecx = 0xABCD0002u; cur.tf.edx = 5; cur.tf.ebx = 0;
    cur.tf.eax = 0x11112222u; cur.tf.eflags = V86_IF | 2;
    for (traps = 0;;) {
        struct gcpu_event ev;
        int rc = if_cpu_run(v, &ev);
        if (rc != GCPU_TRAP || ev.vector != 13) printf("war2vbe palette: rc=%d vector=%u err=%x at %04x:%08x\n", rc,
                                                       ev.vector, ev.err, (unsigned)cur.tf.cs & 0xFFFF, cur.tf.eip);
        CHECK(rc == GCPU_TRAP && ev.vector == 13 && ++traps < 16);
        if (cur.tf.eip == 0x30300) break;
        step(v, ev.vector, ev.err);
    }
    CHECK(v->vga.dac[5][0] == 3 && v->vga.dac[5][1] == 2 && v->vga.dac[5][2] == 1);
    CHECK(v->vga.dac[6][0] == 6 && v->vga.dac[6][1] == 5 && v->vga.dac[6][2] == 4 && v->vga.dac[7][0] != 6);
    CHECK(cur.tf.eax == 0x11112222u && cur.tf.ecx == 0xABCD0002u && cur.tf.edx == 5 && cur.tf.edi == 0x30200);
    destroy(v, true); empty(); gcpu_done(); if_cpu_vm = 0;
    puts("PASS war2vbe WinFuncPtr on the CPU model: BH/BL/DX far call moves window A, A000h stores reach bank 3; "
         "4F0Ah SetPrimaryPalette writes the DAC from a 32-bit client");
}
#endif
/* T23 c42f556e (docs/validation/t23-war2-fullscreen-2026-10-25.md): the
 * Warcraft II demo's full-screen sequence on the SuperSavage provider,
 * replayed on the production VM, full-screen state machine and monitor with
 * the fake adapter: Alt+Enter on the mode 13h logo, 4F02h 0101h while full
 * screen (mode-change return), banked title screen in the window, Alt+Enter
 * into the VBE (linear transport) full screen, a palette load per retrace
 * (DOOM-style OUT 3C9h loop, fx-27 replay), Alt+Enter back to the window.
 * Each check below failed on main 50a843517:
 * - a palette load in the VBE full screen took 769 ordinary exits (the fx-27
 *   lean exits and DAC burst were off for any full screen; T23: 21.8 us per
 *   3C9h exit against 4.6 us in the window);
 * - the first guest frame's durable checkpoint held the display worker, the
 *   only scanout transport, for the 2000 ms BOOT.LOG sync timeout (T23:
 *   "fullscreen_log error=-11 step=first-guest-frame");
 * - a screen that changes only its palette was returned to the window by
 *   the 5 s first-frame watchdog (T23: first pixel store at 4120 ms). */
static unsigned war2_palette(struct dosvm *v, uint8_t seed, bool scalar)
{
    const uint8_t code[] = {0xBA, 0xC8, 0x03, 0x30, 0xC0, 0xEE,  /* mov dx,3C8h; xor al,al; out dx,al */
        0x42, 0xBE, 0x00, 0x01, 0xB9, 0x00, 0x03,                  /* inc dx; mov si,100h; mov cx,300h */
        0xAC, 0xC0, 0xE8, 0x02, 0xEE, 0xE2, 0xF9,                  /* lodsb; shr al,2; out dx,al; loop */
        0x90};
    CHECK(!dosvm_write(v, 0x30000, code, sizeof(code)));
    uint8_t source[768];
    for (unsigned i = 0; i < 768; i++) source[i] = (uint8_t)(seed + i * 3);
    CHECK(!dosvm_write(v, 0x20100, source, sizeof(source)));
    cur.tf.cs = 0x3000; cur.tf.eip = 0; cur.ds = cur.es = 0x2000;
    cur.tf.eflags = V86_VM | V86_IF | 2;
    unsigned exits = fx27_run(v, sizeof(code) - 1, 100000, scalar);
    for (unsigned i = 0; i < 768; i++) CHECK(v->vga.dac[i / 3][i % 3] == (uint8_t)(source[i] >> 2));
    return exits;
}
static void war2_alt_enter(void)
{
    (void)dosvm_fullscreen_input(INPUT_KEY, 0x38, 1);
    CHECK(dosvm_fullscreen_input(INPUT_KEY, 0x1C, 1));
    CHECK(dosvm_fullscreen_input(INPUT_KEY, 0x1C, 0));
    (void)dosvm_fullscreen_input(INPUT_KEY, 0x38, 0);
}
static void test_vbe_native_war2_t23(void)
{
    bool available = fs_vbe_available;
    window_fixtures(); fs_vbe_available = true; fs_direct_available = false;
    (void)dosvm_fullscreen_input(INPUT_RESYNC, 0, 0);
    struct dosvm *v = fx27_vm(false);
    struct dosvm_key key = dosvm_key(v); v->owner_pid = 10; fs_expected_owner = v;
    CHECK(dosvm_input_focus(key, true)); v->fullscreen_parked = true;
    unsigned requests = fs_entry_requests;
    /* 1. Alt+Enter on the Blizzard logo (13h): VGA full screen, whose VGA
     *    ports go to the adapter (no modeled port, no lean exit). */
    war2_alt_enter(); dosvm_fullscreen_poll();
    CHECK(v->fullscreen && fs_display && !fullscreen_linear && !dosvm_vga_ports_modeled(v));
    CHECK(fs_entry_requests == requests + 1);
    /* 2. 4F02h BX=0101h in full screen: ordered return, windowed 101h. */
    struct v86_frame f = vf(0x4F02, 0x0101, 0, 0);
    CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F && v->fullscreen_hold);
    CHECK(v->vbe_mode == 0x101 && !v->vbe_linear);
    dosvm_fullscreen_poll();
    CHECK(!v->fullscreen && !v->fullscreen_hold && !fs_display && !v->fullscreen_error);
    CHECK(v->fullscreen_cause == DOSVM_FS_MODE_CHANGE && dosvm_vga_ports_modeled(v));
    /* The title screen through the bank window: banks 0 and 2. */
    CHECK(dosvm_vga_retry(v, 0xA0000, 0x30000, false)); vbe_window_maps(v, 0);
    vbe_window_store(v, 0x100, 0x21);
    f = vf(0x4F05, 0, 0, 2); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F);
    vbe_window_maps(v, 2); vbe_window_store(v, 0x200, 0x22);
    /* 3. Alt+Enter on the 101h screen: VBE full screen, linear transport,
     *    RAM buffer kept (windowed mode), forced flush at entry. */
    war2_alt_enter(); dosvm_fullscreen_poll();
    CHECK(v->fullscreen && fs_display && fullscreen_linear && !v->vbe_direct.pages);
    CHECK(fs_entry_requests == requests + 2 && dosvm_vga_ports_modeled(v) && !fullscreen_frame_seen);
    CHECK(fs_linear[0x100] == 0x21 && fs_linear[2 * 65536 + 0x200] == 0x22);
    /* 4. The first guest frame while BOOT.LOG does not drain: the poll that
     *    sees it neither waits nor stops the transport. */
    CHECK(dosvm_vga_retry(v, 0xA0000, 0x30000, false)); vbe_window_maps(v, 2);
    vbe_window_store(v, 0x300, 0x23);
    fs_checkpoint_wait_ms = BOOTLOG_SYNC_TIMEOUT_MS;
    uint64_t before = g_ticks; unsigned records = fs_records;
    dosvm_fullscreen_poll();
    uint64_t waited = g_ticks - before;
    CHECK(fullscreen_frame_seen && fs_linear[2 * 65536 + 0x300] == 0x23);
    vbe_window_store(v, 0x301, 0x24); dosvm_fullscreen_poll();
    fs_checkpoint_wait_ms = 0;
    CHECK(fs_linear[2 * 65536 + 0x301] == 0x24 && v->fullscreen);
    printf("T23 c42f556e war2 first frame: display worker waited %llu ms (sync timeout %u ms), records=%u\n",
           (unsigned long long)waited, BOOTLOG_SYNC_TIMEOUT_MS, fs_records - records);
    CHECK(!waited && fs_records == records + 1);
    /* 5. One full palette load per retrace: the scalar control takes 769
     *    exits; the fx-27 lean exit and DAC burst apply as in the window. */
    unsigned scalar = war2_palette(v, 1, true);
    unsigned lean0 = v->perf.lean_exits, dac0 = v->perf.dac_batched;
    unsigned exits = war2_palette(v, 2, false);
    printf("T23 c42f556e war2 VBE full-screen palette load: %u exits (scalar control %u), lean=%u dac_batched=%u\n",
           exits, scalar, v->perf.lean_exits - lean0, v->perf.dac_batched - dac0);
    CHECK(scalar == 769 && exits <= 2 && v->perf.dac_batched - dac0 >= 760);
    /* The poll loads the new palette into the adapter's 8-bit CLUT (the
     *    fake card keeps six bits per entry). */
    dosvm_fullscreen_poll();
    for (unsigned i = 0; i < 256; i++) for (unsigned k = 0; k < 3; k++) {
        unsigned c = v->vga.dac[i][k];
        CHECK(fs_card.dac[i][k] == (uint8_t)(((c << 2) | (c >> 4)) & 63));
    }
    /* 6. Alt+Enter: hotkey return; the game continues in its window. */
    war2_alt_enter(); dosvm_fullscreen_poll();
    CHECK(!v->fullscreen && !fs_display && !v->fullscreen_hold && !v->fullscreen_suspended && !v->fullscreen_error);
    CHECK(v->fullscreen_cause == DOSVM_FS_HOTKEY && v->info.state < DOSVM_STOPPING);
    vbe_window_maps(v, 2);                    /* the bank window stayed open */
    uint8_t byte = 0; CHECK(!dosvm_read(v, 0xA0301, &byte, 1) && byte == 0x24);
    CHECK(war2_palette(v, 3, false) <= 2);
    /* 7. A screen that only changes its palette keeps full screen past the
     *    5 s first-frame watchdog (the DAC change is the guest's store). */
    war2_alt_enter(); dosvm_fullscreen_poll();
    CHECK(v->fullscreen && fullscreen_linear && !fullscreen_frame_seen);
    CHECK(war2_palette(v, 4, false) <= 2);
    g_ticks += 6000;
    dosvm_fullscreen_poll();
    CHECK(v->fullscreen && fs_display && fullscreen_frame_seen && !v->fullscreen_error);
    CHECK(!dosvm_fullscreen_leave(key, false) && !fs_display && !v->fullscreen);
    fs_expected_owner = 0; fs_vbe_available = available;
    (void)dosvm_fullscreen_input(INPUT_RESYNC, 0, 0);
    destroy(v, true); empty();
    puts("PASS T23 c42f556e war2: 13h full screen, 4F02h 0101h mode-change return, banked window, VBE full screen with "
         "lean/burst palette loads, non-blocking first-frame record, palette-only first frame, hotkey return to the window");
}
static void test_vbe_window(void)
{
    bool available = fs_vbe_available; fs_vbe_available = false;
    struct dosvm *v; CHECK(!dosvm_create(0, &v)); /* default VM: room for the 1 MiB buffer */
    /* 4F00h: VBE 2.0, 1 MiB, the mode list with 101h. */
    struct v86_frame f = vf(0x4F00, 0, 0, 0); f.es = 0x2000;
    CHECK(!dosvm_write(v, 0x20000, "VBE2", 4));
    CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F);
    uint8_t info[512];
    CHECK(!dosvm_read(v, 0x20000, info, sizeof(info)));
    CHECK(!memcmp(info, "VESA", 4) && vm_u16(info + 4) == 0x200 && vm_u16(info + 18) == 16);
    uint8_t list[8]; CHECK(!dosvm_read(v, vm_far(vm_u32(info + 14)), list, sizeof(list)));
    CHECK(vm_u16(list) == 0x101 && vm_u16(list + 2) == 0x103 && vm_u16(list + 4) == 0x111 && vm_u16(list + 6) == 0x114);
    char product[16]; CHECK(!dosvm_read(v, vm_far(vm_u32(info + 26)), product, sizeof(product)));
    CHECK(!strcmp(product, "CiukiOS VBE"));
    /* 4F01h 101h: windowed and linear, window A 64 KiB at A000h, WinFuncPtr. */
    f = vf(0x4F01, 0, 0x101, 0); f.es = 0x2000;
    CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F && !dosvm_read(v, 0x20000, info, 256));
    CHECK(vm_u16(info) == 0x009B && info[2] == 7 && !info[3] && vm_u16(info + 4) == 64 && vm_u16(info + 6) == 64);
    CHECK(vm_u16(info + 8) == 0xA000 && !vm_u16(info + 10) && vm_u32(info + 12) == 0xF0000104);
    CHECK(vm_u16(info + 16) == 640 && vm_u16(info + 18) == 640 && vm_u16(info + 20) == 480);
    CHECK(info[24] == 1 && info[25] == 8 && info[26] == 1 && info[27] == 4 && !info[29] && info[30] == 1);
    CHECK(vm_u32(info + 40) == DOS_VBE_PHYS);
    f = vf(0x4F01, 0, 0x111, 0); f.es = 0x2000;
    CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F && !dosvm_read(v, 0x20000, info, 256));
    CHECK(vm_u16(info) == 0x00BB && info[25] == 16 && info[27] == 6 && info[31] == 5 && info[32] == 11);
    /* The direct-call window function: mov ax,4F05h; pushf; call far F000:0100; retf. */
    static const uint8_t stub[] = {0xB8, 0x05, 0x4F, 0x9C, 0x9A, 0x00, 0x01, 0x00, 0xF0, 0xCB};
    uint8_t rom[sizeof(stub) + 4]; CHECK(!dosvm_read(v, 0xF0100, rom, sizeof(rom)));
    CHECK(rom[0] == 0xCD && rom[1] == 0xFE && rom[2] == 0xCF && !memcmp(rom + 4, stub, sizeof(stub)));
    /* 4F0Ah: the real-mode pointer and length of the protected-mode table:
     * SetWindow at 8, SetDisplayStart at 11h, SetPrimaryPalette at 12h, no
     * ports/memory sub-table. BL other than 0 fails. */
    f = vf(0x4F0A, 0, 0, 0); f.es = 0x1234; f.tf.edi = 0x5678;
    CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F && f.es == 0xF000 &&
          (uint16_t)f.tf.edi == DOSVM_VBE_PM_TABLE && (uint16_t)f.tf.ecx == 59);
    uint8_t pm[59]; CHECK(!dosvm_read(v, 0xF0000 + DOSVM_VBE_PM_TABLE, pm, sizeof(pm)));
    CHECK(vm_u16(pm) == 8 && vm_u16(pm + 2) == 0x11 && vm_u16(pm + 4) == 0x12 && !vm_u16(pm + 6));
    static const uint8_t set_window[] = {0x50, 0x66, 0xB8, 0x05, 0x4F, 0xCD, 0x10, 0x58, 0xC3};
    CHECK(!memcmp(pm + 8, set_window, sizeof(set_window)) && pm[0x11] == 0xC3 && pm[58] == 0xC3);
    f = vf(0x4F0A, 1, 0, 0); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x014F);
    /* No window yet: 4F05h fails outside a VBE mode. */
    f = vf(0x4F05, 0, 0, 1); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x014F);
    /* 4F02h 101h without D14: the windowed model, bank 0 mapped at once. */
    f = vf(0x4F02, 0x0101, 0, 0);
    CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F && v->vbe_mode == 0x101 && !v->vbe_linear);
    CHECK(v->vbe_buffer.used && v->vbe_buffer.pages == 256);
    vbe_window_maps(v, 0);
    f = vf(0x4F03, 0, 0, 0); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.ebx == 0x0101);
    port_out(v, 0x3C8, 1, 7); port_out(v, 0x3C9, 1, 63); port_out(v, 0x3C9, 1, 0); port_out(v, 0x3C9, 1, 0);
    port_out(v, 0x3C8, 1, 9); port_out(v, 0x3C9, 1, 0); port_out(v, 0x3C9, 1, 0); port_out(v, 0x3C9, 1, 63);
    struct ciuki_rect damage;
    memset(frame_buffer, 0xEE, sizeof(frame_buffer));
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, g_ticks, &damage) == 1 && damage.height == 480);
    CHECK(vbe_window_pixel(0, 0) == 0 && vbe_window_pixel(639, 479) == 0);
    /* Bank 0 store: pixel (10,1) = offset 650. */
    vbe_window_store(v, 650, 7);
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, g_ticks + 100, &damage) == 1);
    CHECK(vbe_window_pixel(10, 1) == 0xFF0000 && damage.dst_y <= 1 && damage.dst_y + damage.height > 1);
    /* 4F05h: set and get window A; window B and out-of-range banks fail. */
    f = vf(0x4F05, 0, 0, 1); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F);
    vbe_window_maps(v, 1);
    f = vf(0x4F05, 0x0100, 0, 0); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F && (uint16_t)f.tf.edx == 1);
    f = vf(0x4F05, 0x0001, 0, 2); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x014F);
    f = vf(0x4F05, 0, 0, 16); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x014F && v->vbe_bank == 1);
    /* Bank 1 store at window offset 0x100: buffer 65792 = pixel (512,102);
     * the bank switch carried bank 0's D pages, not new damage. */
    vbe_window_store(v, 0x100, 9);
    uint8_t byte = 0; CHECK(!dosvm_read(v, v->vbe_buffer.base + 65536 + 0x100, &byte, 1) && byte == 9);
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, g_ticks + 200, &damage) == 1);
    CHECK(vbe_window_pixel(512, 102) == 0x0000FF && vbe_window_pixel(10, 1) == 0xFF0000);
    CHECK(damage.dst_y <= 102 && damage.dst_y + damage.height > 102);
    /* A kernel store into the window (a DOS read into A000h) lands in the
     * current bank and is damage for the presenter. */
    uint8_t row[4] = {7, 7, 7, 7};
    CHECK(!dosvm_write(v, 0xA0000 + 640 * 2, row, sizeof(row)));
    CHECK(!dosvm_read(v, v->vbe_buffer.base + 65536 + 1280, &byte, 1) && byte == 7);
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, g_ticks + 300, &damage) == 1);
    CHECK(vbe_window_pixel(256, 104) == 0xFF0000 && vbe_window_pixel(259, 104) == 0xFF0000);
    CHECK(!dosvm_read(v, 0xA0000 + 0x100, &byte, 1) && byte == 9);
    /* PTEs replaced behind the window (a full-screen aperture restore): the
     * next guest access maps it again instead of reaching the VGA planes. */
    dosvm_vga_sync(v, true);
    CHECK(!v->video_direct && !(*window_pte(v, 0xA0000) & PTE_U));
    CHECK(dosvm_vga_retry(v, 0xA0005, 0x30000, false));
    vbe_window_maps(v, 1);
    CHECK(!dosvm_vga_retry(v, 0xA0005, 0x30000, false)); /* already mapped: no retry loop */
    CHECK(!dosvm_vga_retry(v, 0xB8000, 0x30000, false));
    /* DPMI 0800h maps the private linear token without a native provider. */
    v->dpmi.clients = 1;
    f = vf(0x0800, DOS_VBE_PHYS >> 16, 0, 0); f.tf.esi = 16;
    CHECK(!dosvm_vbe_map(v, &f, 0x0800) && (((f.tf.ebx & 65535) << 16) | (f.tf.ecx & 65535)) == v->vbe_buffer.base);
    f = vf(0x0801, v->vbe_buffer.base >> 16, v->vbe_buffer.base & 65535, 0);
    CHECK(!dosvm_vbe_map(v, &f, 0x0801)); v->dpmi.clients = 0;
    /* D14: the linear model closes the window; 4F05h then fails with 03h. */
    f = vf(0x4F02, 0xC101, 0, 0);
    CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F && v->vbe_linear && !v->video_direct);
    CHECK(!(*window_pte(v, 0xA0000) & PTE_U) && !dosvm_vga_retry(v, 0xA0000, 0x30000, false));
    f = vf(0x4F03, 0, 0, 0); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.ebx == 0xC101);
    f = vf(0x4F05, 0, 0, 1); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x034F);
    /* Back to the windowed model, then a VGA mode: the BIOS clears the VGA
     * planes, the window closes and A0000h is the VGA model again. */
    f = vf(0x4F02, 0x8101, 0, 0);
    CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F && !v->vbe_linear);
    vbe_window_maps(v, 0);
    CHECK(!dosvm_read(v, v->vbe_buffer.base + 650, &byte, 1) && byte == 7); /* D15: memory kept */
    v->vga.plane[0][0] = 0x5A;
    int10(v, 0x13, 0, 0, 0);
    CHECK(!v->vbe_mode && v->video_direct != VIDEO_BANK && !v->vga.plane[0][0]);
    CHECK((*window_pte(v, 0xA0000) & ~0xFFFu) != dosvm_vbe_frame(v, 0));
    byte = 0x33; CHECK(!dosvm_write(v, 0xA0000, &byte, 1) && cvga_peek_vram(&v->vga, 0xA0000) == 0x33);
    /* Teardown with the window open: the alias is closed before the
     * address space frees the buffer frames (no double free). */
    f = vf(0x4F02, 0x0103, 0, 0);
    CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F && v->vbe_mode == 0x103);
    f = vf(0x4F05, 0, 0, 7); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F);
    vbe_window_maps(v, 7);
    destroy(v, true); empty();

    /* Full-screen entry without a provider stays windowed (unchanged). */
    window_fixtures();
    struct ciuki_dosvm_command w; CHECK(!create_window(0, "VBE window", &w));
    struct dosvm_key key = {w.id, w.generation}; v = dosvm_lookup(key);
    CHECK(dosvm_input_focus(key, true)); v->fullscreen_parked = true;
    f = vf(0x4F02, 0x0101, 0, 0); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F);
    CHECK(dosvm_fullscreen_enter(key) == DISPLAY_FULLSCREEN_WINDOWED && !v->fullscreen);
    vbe_window_maps(v, 0);
    /* With a native provider the windowed mode uses the linear full-screen
     * path: the window reopens on the first access and its D pages reach
     * the native frame buffer through the flush. */
    fs_vbe_available = true;
    CHECK(!dosvm_fullscreen_enter(key) && v->fullscreen && !v->video_direct);
    CHECK(dosvm_vga_retry(v, 0xA1000, 0x30000, false));
    vbe_window_maps(v, 0);
    vbe_window_store(v, 0x1000, 0x42);
    dosvm_fullscreen_poll(); CHECK(fs_linear[0x1000] == 0x42);
    f = vf(0x4F05, 0, 0, 2); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F);
    vbe_window_maps(v, 2);
    vbe_window_store(v, 3, 0x24);
    dosvm_fullscreen_poll(); CHECK(fs_linear[2 * 65536 + 3] == 0x24);
    CHECK(!dosvm_fullscreen_leave(key, false) && !v->fullscreen && !fs_display);
    vbe_window_maps(v, 2);
    /* 101-dosvid2 direct LFB lease (merge 2026-10-25): a windowed mode never
     * borrows the provider's LFB; it keeps the RAM buffer, the bank window
     * and the copy flush. */
    fs_direct_available = true; fs_direct_phys = 0x300000; fs_direct_cache = 0; fs_expected_owner = v;
    CHECK(!dosvm_fullscreen_enter(key) && v->fullscreen && !v->vbe_direct.pages && !v->video_direct);
    CHECK(dosvm_vga_retry(v, 0xA0000, 0x30000, false));
    vbe_window_maps(v, 2);
    vbe_window_store(v, 7, 0x55);
    dosvm_fullscreen_poll(); CHECK(fs_linear[2 * 65536 + 7] == 0x55 && !v->vbe_direct.pages);
    CHECK(!dosvm_fullscreen_leave(key, false) && !v->fullscreen);
    /* The linear model (D14, memory kept) takes the lease; no bank window. */
    f = vf(0x4F02, 0xC101, 0, 0);
    CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F && v->vbe_linear && !v->video_direct);
    CHECK(!dosvm_fullscreen_enter(key) && v->fullscreen && v->vbe_direct.pages);
    CHECK(!dosvm_vbe_frame(v, 0) && !dosvm_vga_retry(v, 0xA0000, 0x30000, false));
    f = vf(0x4F05, 0, 0, 1); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x034F);
    *(uint8_t *)P2V(fs_direct_phys + 2 * 65536 + 9) = 0x66;   /* a guest store through the lease */
    /* A windowed mode set in full screen (D15): the lease is revoked with
     * its copy-back before the mode changes, the VM returns to the window,
     * and the bank window then shows the RAM copy of the leased frame. */
    f = vf(0x4F02, 0x8101, 0, 0);
    CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F && !v->vbe_linear);
    CHECK(!v->vbe_direct.pages && !v->vbe_direct.copying && v->fullscreen_hold && !v->video_direct);
    dosvm_fullscreen_poll();
    CHECK(!v->fullscreen && !v->fullscreen_hold && !fs_display);
    CHECK(dosvm_vga_retry(v, 0xA0000, 0x30000, false));
    vbe_window_maps(v, 0);
    f = vf(0x4F05, 0, 0, 2); CHECK(!dosvm_vga_bios(v, &f) && (uint16_t)f.tf.eax == 0x004F);
    vbe_window_maps(v, 2);
    uint8_t leased = 0; CHECK(!dosvm_read(v, 0xA0009, &leased, 1) && leased == 0x66);
    CHECK(!dosvm_read(v, 0xA0007, &leased, 1) && leased == 0x55);
    fs_direct_available = false; fs_expected_owner = 0;
    /* Review P2: banked mode -> native full screen -> reopened bank -> mode
     * 13h -> full-screen return. The VGA mode set closes the bank window
     * itself (nothing else touches A0000h in a VBE full screen), so a native
     * mode 13h store after the return faults into the VGA path instead of
     * landing in the old VBE bank. */
    CHECK(!dosvm_fullscreen_enter(key) && v->fullscreen && !v->video_direct);
    CHECK(dosvm_vga_retry(v, 0xA0000, 0x30000, false));
    vbe_window_maps(v, 2);
    vbe_window_store(v, 0x20, 0x5E);                             /* a pending bank store */
    uint8_t banked = 0; CHECK(!dosvm_read(v, v->vbe_buffer.base + 2 * 65536 + 5, &banked, 1));
    int10(v, 0x13, 0, 0, 0);
    CHECK(!v->vbe_mode && v->video_direct != VIDEO_BANK && v->fullscreen_hold);
    for (unsigned page = 0; page < 16; page++) {
        uint32_t pte = *window_pte(v, 0xA0000 + page * PAGE_SIZE);
        CHECK(!(pte & PTE_U) && (pte & ~0xFFFu) == v->frames[0xA0 + page]);
    }
    CHECK(*vbe_pte(v, 32) & PTE_D);                              /* the bank store was harvested */
    dosvm_fullscreen_poll();
    CHECK(!v->fullscreen && !v->fullscreen_hold && !fs_display);
    CHECK(!(*window_pte(v, 0xA0000) & PTE_U) && (*window_pte(v, 0xA0000) & ~0xFFFu) == v->frames[0xA0]);
    CHECK(dosvm_vga_retry(v, 0xA0005, 0x30000, false) && v->video_direct == VIDEO_LINEAR);
    linear_store(v, 5, 0x3C);                                    /* native mode 13h store */
    dosvm_vga_sync(v, true);
    CHECK(cvga_peek_vram(&v->vga, 0xA0005) == 0x3C);
    uint8_t unchanged = 0;
    CHECK(!dosvm_read(v, v->vbe_buffer.base + 2 * 65536 + 5, &unchanged, 1) && unchanged == banked);
    CHECK(!dosvm_read(v, v->vbe_buffer.base + 2 * 65536 + 0x20, &unchanged, 1) && unchanged == 0x5E);
    struct ciuki_dosvm_command close = command_for(key.id, key.generation);
    CHECK(!finish_test_vm(&close, sizeof(close))); empty();
#ifdef CIUKI_DOSVM_GUEST_CPU
    vbe_window_function_cpu();
#endif
    fs_vbe_available = available;
    puts("PASS war2vbe windowed VBE without a native provider: 4F00h/4F01h/4F02h/4F03h/4F05h, WinFuncPtr stub, bank aliasing, "
         "window and kernel stores presented with the DAC palette, linear/VGA returns, teardown, native full-screen flush, "
         "direct LFB lease for linear modes only with its copy-back seen through the bank window, "
         "VGA mode set in a VBE full screen closes the bank window");
}
