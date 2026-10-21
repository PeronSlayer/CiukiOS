/* f3-02 host cases, included by dosvm_test.c after the production sources:
 * VGA BIOS/ports, guarded-aperture emulation, presenter damage and its
 * equivalence with the retained vga_presenter, golden VMVIDEO.COM stage
 * digests and the native window boundary (syscall 70). No guest DOS runs.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "../../src/kernel/vm/vga_presenter.h"

/* ---- Native boundary fakes: processes, descriptors, surfaces, user copies ---- */
struct surface { struct ciuki_surface_info info; uint8_t *pixels; uint32_t references; };
static struct process fake_proc[3];
static struct process *fake_current;
static struct desktop_description fake_desc[4];
static const struct process *desc_owner[4];
static struct surface fake_surface[3];
static uint8_t user_mem[4096];
static bool user_fault;
static unsigned pins;
static unsigned record_pin_walks, surface_rows;
static bool caught_signal;
static struct proc_thread window_thread;
int dosvm_model_user_read(const struct uaddr *, void *, uint32_t, uint32_t);
int dosvm_model_user_write(const struct uaddr *, uint32_t, const void *, uint32_t);
int dosvm_model_read(const struct uaddr *, void *, uint32_t, uint32_t);
int dosvm_model_write(const struct uaddr *, uint32_t, const void *, uint32_t);
struct proc_thread *proc_thread_for(const struct task *t) { return t == &caller ? &window_thread : 0; }
bool proc_signal_caught(struct proc_thread *t) { CHECK(t == &window_thread); return caught_signal; }
struct process *proc_current(void) { return fake_current; }
struct process *proc_find(uint32_t pid)
{
    for (unsigned i = 0; i < 3; i++) if (pid && fake_proc[i].pid == pid) return &fake_proc[i];
    return 0;
}
struct desktop_description *desktop_fd(const struct process *p, int32_t fd)
{ return fd >= 0 && fd < 4 && desc_owner[fd] == p ? &fake_desc[fd] : 0; }
const struct ciuki_surface_info *surface_geometry(const struct surface *s) { return &s->info; }
void surface_retain(struct surface *s) { s->references++; }
void surface_release(struct surface *s) { CHECK(s->references > 1); s->references--; }
int surface_write(struct surface *s, uint32_t offset, const void *data, uint32_t bytes)
{
    surface_rows++;
    if (offset > s->info.allocation_bytes || bytes > s->info.allocation_bytes - offset) return -EINVAL;
    memcpy(s->pixels + offset, data, bytes);
    return 0;
}
int copy_from_user(void *dst, uint32_t src, uint32_t bytes)
{
    if (!pins) record_pin_walks++; /* production copy resolves/pins absent an outer pin */
    if (user_fault || src > sizeof(user_mem) || bytes > sizeof(user_mem) - src) return -EFAULT;
    memcpy(dst, user_mem + src, bytes);
    return 0;
}
int copy_to_user(uint32_t dst, const void *src, uint32_t bytes)
{
    if (!pins) record_pin_walks++;
    if (dst > sizeof(user_mem) || bytes > sizeof(user_mem) - dst) return -EFAULT;
    memcpy(user_mem + dst, src, bytes);
    return 0;
}
int ua_pin(struct uaddr *u, struct ua_pin *pin, uint32_t base, uint32_t bytes, bool write)
{
    (void)u; CHECK(write && !pins);
    record_pin_walks++;
    if (base > sizeof(user_mem) || bytes > sizeof(user_mem) - base) return -EFAULT;
    pin->base = base; pin->end = base + bytes; pins++;
    return 0;
}
void ua_unpin(struct uaddr *u, struct ua_pin *pin) { (void)u; (void)pin; CHECK(pins == 1); pins--; }
int ua_read(const struct uaddr *u, void *dst, uint32_t src, uint32_t bytes)
{
    (void)u;
    if (user_fault || src > sizeof(user_mem) || bytes > sizeof(user_mem)-src) return -EFAULT;
    memcpy(dst,user_mem+src,bytes); return 0;
}
int ua_write(const struct uaddr *u, uint32_t dst, const void *src, uint32_t bytes)
{
    (void)u;
    if (user_fault || dst > sizeof(user_mem) || bytes > sizeof(user_mem)-dst) return -EFAULT;
    memcpy(user_mem+dst,src,bytes); return 0;
}
int ua_user_read(const struct uaddr *u, void *dst, uint32_t src, uint32_t bytes)
{ return u ? dosvm_model_user_read(u,dst,src,bytes) : ua_read(u,dst,src,bytes); }
int ua_user_write(const struct uaddr *u, uint32_t dst, const void *src, uint32_t bytes)
{ return u ? dosvm_model_user_write(u,dst,src,bytes) : ua_write(u,dst,src,bytes); }

/* ---- Helpers ---- */
static struct v86_frame vf(unsigned ax, unsigned bx, unsigned cx, unsigned dx)
{ return (struct v86_frame){.tf = {.eax = ax, .ebx = bx, .ecx = cx, .edx = dx, .eflags = V86_VM | V86_IF | 2}}; }
static void int10(struct dosvm *v, unsigned ax, unsigned bx, unsigned cx, unsigned dx)
{ struct v86_frame f = vf(ax, bx, cx, dx); CHECK(!dosvm_bios(v, &f, 0x10)); }
static void port_out(struct dosvm *v, uint16_t port, unsigned width, uint32_t value)
{ CHECK(!v86_io(&v->monitor, port, width, true, &value)); }
static uint32_t port_in(struct dosvm *v, uint16_t port, unsigned width)
{ uint32_t value = 0; CHECK(!v86_io(&v->monitor, port, width, false, &value)); return value; }
static uint8_t frame_buffer[DOSVM_VIDEO_WIDTH * DOSVM_VIDEO_HEIGHT * 4];
static uint8_t reference_buffer[DOSVM_VIDEO_WIDTH * DOSVM_VIDEO_HEIGHT * 4];
static unsigned row_fail_at = UINT32_MAX;
static int capture_row(void *arg, unsigned y, const void *pixels, unsigned bytes)
{
    if (y == row_fail_at) return -EIO;
    CHECK(bytes == DOSVM_VIDEO_WIDTH * 4 && y < DOSVM_VIDEO_HEIGHT);
    memcpy((uint8_t *)arg + y * DOSVM_VIDEO_WIDTH * 4, pixels, bytes);
    return 0;
}
/* The retained presenter (cvp_present, CVP_FORCE_FULL) on the same window
 * rectangle must produce the same pixels as the row-based kernel form. */
static cvp_state reference_presenter;
static void compare_with_reference(struct dosvm *v, uint64_t tick)
{
    struct ciuki_rect d;
    memset(frame_buffer, 0xEE, sizeof(frame_buffer));
    v->video_presented = false;
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, tick, &d) == 1 && !d.dst_y && d.height == DOSVM_VIDEO_HEIGHT);
    cvga_geometry g;
    CHECK(cvga_get_geometry(&v->vga, &g) && g.width && g.height);
    unsigned scale = DOSVM_VIDEO_WIDTH / g.width < DOSVM_VIDEO_HEIGHT / g.height ?
                     DOSVM_VIDEO_WIDTH / g.width : DOSVM_VIDEO_HEIGHT / g.height;
    unsigned w = g.width * scale, h = g.height * scale;
    unsigned left = (DOSVM_VIDEO_WIDTH - w) / 2, top = (DOSVM_VIDEO_HEIGHT - h) / 2;
    cvp_format format = {DOSVM_VIDEO_WIDTH * 4, DOSVM_VIDEO_WIDTH, DOSVM_VIDEO_HEIGHT, 4, 8, 16, 8, 8, 8, 0, 0};
    cvp_request q;
    memset(&q, 0, sizeof(q));
    q.window = (cvp_rect){(int16_t)left, (int16_t)top, (int16_t)(left + w), (int16_t)(top + h)};
    q.flags = CVP_FORCE_FULL | ((tick / DOSVM_BLINK_TICKS) & 1 ? 0 : CVP_BLINK_VISIBLE) |
              ((tick / DOSVM_CURSOR_TICKS) & 1 ? 0 : CVP_CURSOR_VISIBLE);
    memset(reference_buffer, 0, sizeof(reference_buffer));
    memset(&reference_presenter, 0, sizeof(reference_presenter));
    cvp_init(&reference_presenter);
    cvp_stats stats;
    CHECK(cvp_present(&reference_presenter, &v->vga, &format, &q, reference_buffer, &stats) == CVP_OK && stats.complete);
    for (unsigned i = 0; i < sizeof(frame_buffer); i += 4) reference_buffer[i + 3] = 0; /* X byte */
    CHECK(!memcmp(frame_buffer, reference_buffer, sizeof(frame_buffer)));
}

/* ---- VGA BIOS, ports and synthetic ROM fonts ---- */
static void test_video_bios_ports(void)
{
    struct dosvm *v = create();
    uint8_t mode = 0;
    CHECK(!dosvm_read(v, 0x449, &mode, 1) && mode == 3 && (v->vga.seq[1] & 1));
    struct v86_frame f = vf(0x4F00, 0, 0, 0); f.es = 0x2000;
    CHECK(!dosvm_vga_bios(v, &f) && (f.tf.eax & 0xFFFF) == 0x014F);
    f = vf(0x4F02, 0x4103, 0, 0);
    CHECK(!dosvm_bios(v, &f, 0x10) && (f.tf.eax & 0xFFFF) == 0x014F && !dosvm_read(v, 0x449, &mode, 1) && mode == 3);
    int10(v, 0x0013, 0, 0, 0);
    cvga_geometry g;
    CHECK(!dosvm_read(v, 0x449, &mode, 1) && mode == 0x13 && cvga_get_geometry(&v->vga, &g) && g.width == 320 && g.height == 200);
    f = vf(0x0F00, 0, 0, 0); CHECK(!dosvm_bios(v, &f, 0x10) && (f.tf.eax & 0xFFFF) == 0x2813);
    f = vf(0x1A00, 0, 0, 0); CHECK(!dosvm_bios(v, &f, 0x10) && (f.tf.eax & 0xFF) == 0x1A && (f.tf.ebx & 0xFF) == 8);
    /* Guest buffers are validated whole before any DAC change. */
    uint8_t rgb[6] = {1, 2, 3, 4, 5, 6}, saved[3];
    memcpy(saved, v->vga.dac[0x10], 3);
    f = vf(0x1017, 0x10, 2, 0); f.es = 0xF000; CHECK(dosvm_vga_bios(v, &f) == -EFAULT); /* ROM destination */
    f = vf(0x1012, 0x10, 0x8000, 0); f.es = 0x2000; CHECK(dosvm_vga_bios(v, &f) == -EFAULT);
    f = vf(0x1012, 0x10, 2, 0xFFFE); f.es = 0x2000; CHECK(dosvm_vga_bios(v, &f) == -EFAULT);
    CHECK(!memcmp(saved, v->vga.dac[0x10], 3));
    CHECK(!dosvm_write(v, 0x20000, rgb, sizeof(rgb)));
    f = vf(0x1012, 0x10, 2, 0); f.es = 0x2000;
    CHECK(!dosvm_vga_bios(v, &f) && v->vga.dac[0x10][0] == 1 && v->vga.dac[0x11][2] == 6);
    /* Indexed OUTW, DAC ports and bytes beyond 3DFh. */
    port_out(v, 0x3C4, 2, 0xFFFF0B02u);
    CHECK(v->vga.seq_index == 2 && v->vga.seq[2] == 0x0B);
    port_out(v, 0x3C8, 1, 0xFF41); port_out(v, 0x3C9, 1, 12); port_out(v, 0x3C9, 1, 34); port_out(v, 0x3C9, 1, 56);
    port_out(v, 0x3C7, 1, 0x41);
    CHECK(port_in(v, 0x3C9, 1) == 12 && port_in(v, 0x3C9, 1) == 34 && port_in(v, 0x3C9, 1) == 56);
    CHECK((port_in(v, 0x3DE, 4) >> 16) == 0xFFFF);
    /* Input status 1 (f3-06): each frame of the programmed timing ends with
     * the vertical blank, display disabled before and after the sync, each
     * region a fixed number of reads; bit 0 alternates in the active period. */
    CHECK(vga_frame_us(&v->vga) == 14268);                       /* 13h: 449 lines at 31.469 kHz, 70.09 Hz */
    uint64_t ticks = g_ticks; g_ticks = 1000;
    for (unsigned i = 0; i < 16; i++) CHECK(port_in(v, 0x3DA, 1) == 1);
    for (unsigned i = 0; i < 4; i++) CHECK(port_in(v, 0x3DA, 1) == 9);
    for (unsigned i = 0; i < 16; i++) CHECK(port_in(v, 0x3DA, 1) == 1);
    uint8_t a = (uint8_t)port_in(v, 0x3DA, 1), b = (uint8_t)port_in(v, 0x3DA, 1);
    CHECK(a != b && (a | b) == 1);                               /* active: horizontal blanking */
    CHECK(v->video_vblank_us > 1000000 && v->video_vblank_us <= 1000000 + 14268);
    for (unsigned i = 0; i < 1000; i++) CHECK(!(port_in(v, 0x3DA, 1) & 8)); /* no time, no frame */
    g_ticks = (v->video_vblank_us + 999) / 1000;
    CHECK(port_in(v, 0x3DA, 1) == 1 && v->video_blank == 1);
    g_ticks = ticks;
    /* ROM fonts: ASCII from the console bitmap, CP437 letters/boxes/blocks drawn. */
    const uint8_t *rom16 = P2V(v->frames[0xF2]), *rom8 = P2V(v->frames[0xF3]);
    CHECK(rom16['A' * 16 + 5] == font_cfn_regular[95 + (('A' - 32) * 16 + 5) * 2 + 1] && rom16['A' * 16 + 5]);
    CHECK(rom8['A' * 8 + 2] == (font_cfn_regular[95 + (('A' - 32) * 16 + 4) * 2 + 1] |
                                font_cfn_regular[95 + (('A' - 32) * 16 + 5) * 2 + 1]));
    CHECK(!memcmp(rom16 + 0x82 * 16, rom16 + 'e' * 16, 16) && !memcmp(rom16 + 0x01 * 16, rom16 + '?' * 16, 16));
    CHECK(rom16[0xC4 * 16 + 7] == 0xFF && !rom16[0xC4 * 16 + 6] && rom16[0xB3 * 16] == 0x18 && rom16[0xB3 * 16 + 15] == 0x18);
    CHECK(rom16[0xC9 * 16 + 5] == 0x7F && rom16[0xC9 * 16 + 6] == 0x60 && rom16[0xC9 * 16 + 7] == 0x67 &&
          rom16[0xC9 * 16 + 12] == 0x66 && !rom16[0xC9 * 16 + 4]);
    CHECK(rom16[0xCE * 16 + 0] == 0x66 && rom16[0xCE * 16 + 5] == 0xE7 && !rom16[0xCE * 16 + 6] && rom16[0xCE * 16 + 7] == 0xE7);
    CHECK(rom16[0xDB * 16 + 15] == 0xFF && rom16[0xDC * 16 + 7] == 0 && rom16[0xDC * 16 + 8] == 0xFF && !rom16[0]);
    destroy(v, true); empty();
    printf("DOS VM video: VBE status, INT 10h buffers, VGA ports, retrace and ROM fonts PASS\n");
}

/* ---- #PF on the guarded aperture, emulated through the VGA bus ---- */
static void test_planar_latch_fallback(void)
{
    struct dosvm *v = create();
    int10(v, 0x13, 0, 0, 0);
    port_out(v, 0x3C4, 2, 0x0604);
    port_out(v, 0x3C4, 2, 0x0102);
    CHECK(v->video_direct == 1);
    for (unsigned p = 0; p < 4; p++) for (unsigned at = 0; at < 32; at++)
        cvga_store_plane(&v->vga, p, 0x100 + at, (uint8_t)(p * 41 + at));
    /* VL_LatchToScreen: video-to-video REP MOVSB with write mode 1 and
     * all four planes enabled. CPU read-map data cannot represent this copy. */
    port_out(v, 0x3CE, 2, 0x4105);
    port_out(v, 0x3C4, 2, 0x0F02);
    CHECK(!v->video_direct);
    const uint8_t rep[] = {0xF3, 0xA4};
    CHECK(!dosvm_write(v, 0x30000, rep, sizeof(rep)));
    struct v86_frame f = vf(0, 0, 32, 0);
    f.tf.cs = 0x3000; f.ds = f.es = 0xA000; f.tf.esi = 0x100; f.tf.edi = 0x200;
    CHECK(!dosvm_vga_fault(v, &f) && !f.tf.ecx && f.tf.eip == 2);
    for (unsigned p = 0; p < 4; p++) {
        CHECK(!memcmp(&v->vga.plane[p][0x100], &v->vga.plane[p][0x200], 32));
        CHECK(v->vga.latch[p] == (uint8_t)(p * 41 + 31));
    }
    /* Same-value stores still broadcast to a different plane; untouched
     * bytes in that dirty 4 KiB page must retain their per-plane contents. */
    port_out(v, 0x3CE, 2, 0x4005);
    port_out(v, 0x3C4, 2, 0x0302);
    CHECK(!v->video_direct);
    cvga_store_plane(&v->vga, 0, 0x400, 0x19);
    cvga_store_plane(&v->vga, 1, 0x400, 0xA3);
    cvga_store_plane(&v->vga, 0, 0x401, 0x72);
    cvga_store_plane(&v->vga, 1, 0x401, 0xE4);
    uint8_t byte = 0x19;
    CHECK(!dosvm_write(v, 0xA0400, &byte, 1));
    CHECK(v->vga.plane[0][0x400] == 0x19 && v->vga.plane[1][0x400] == 0x19 &&
          v->vga.plane[0][0x401] == 0x72 && v->vga.plane[1][0x401] == 0xE4);
    destroy(v, true); empty();
    puts("f3-16 VGA fallback: Wolf3D latch REP copies all four distinct planes, last latches exact, sparse/same-value multi-plane writes preserve untouched bytes PASS");
}
static void test_video_faults(void)
{
    struct dosvm *v = create();
    static const uint8_t code[] = {
        0x26, 0x89, 0x05,       /* 0: mov [es:di], ax */
        0xF3, 0xAB,             /* 3: rep stosw */
        0x26, 0x8A, 0x05,       /* 5: mov al, [es:di] */
        0xD9, 0x05,             /* 8: fld dword [di] -- unsupported */
        0x26, 0xA3, 0xFF, 0xFF, /* 10: mov [es:0FFFFh], ax -- crosses the 64 KiB limit */
        0x26, 0xF6, 0x35,       /* 14: div byte [es:di] -- #DE when the byte is zero */
    };
    CHECK(!dosvm_write(v, 0x30000, code, sizeof(code)));
    struct v86_frame f = vf(0x1E41, 0, 0, 0);
    f.tf.cs = 0x3000; f.es = 0xB800; f.ds = 0x3000; f.tf.user_ss = 0x2000; f.tf.user_esp = 0x100;
    CHECK(!dosvm_vga_fault(v, &f) && f.tf.eip == 3 && cvga_peek_vram(&v->vga, 0xB8000) == 'A' &&
          cvga_peek_vram(&v->vga, 0xB8001) == 0x1E);
    f.tf.ecx = 5000; f.tf.edi = 2; f.tf.eax = 0x0741;
    CHECK(!dosvm_vga_fault(v, &f) && f.tf.eip == 3 && (f.tf.ecx & 0xFFFF) == 5000 - VIDEO_STRING_BUDGET &&
          (f.tf.edi & 0xFFFF) == 2 + VIDEO_STRING_BUDGET * 2);
    CHECK(!dosvm_vga_fault(v, &f) && f.tf.eip == 5 && !(f.tf.ecx & 0xFFFF) && (f.tf.edi & 0xFFFF) == 2 + 5000 * 2 &&
          cvga_peek_vram(&v->vga, 0xB8000 + 2 + 4999 * 2) == 0x41);
    f.tf.edi = 0; f.tf.eax = 0;
    CHECK(!dosvm_vga_fault(v, &f) && (f.tf.eax & 0xFF) == 'A' && f.tf.eip == 8);
    struct v86_frame before = f;
    CHECK(dosvm_vga_fault(v, &f) == -EFAULT && !memcmp(&before, &f, sizeof(f)));
    f.tf.eip = 10; before = f;
    CHECK(dosvm_vga_fault(v, &f) == -EFAULT && f.tf.eip == 10);
    /* #DE reflects through the guest IVT with the faulting IP on its stack. */
    uint8_t zero_byte = 0;
    CHECK(!dosvm_write(v, 0xB8100, &zero_byte, 1));
    v->monitor.state = V86_RUNNING; v->monitor.vif = true;
    f.tf.eip = 14; f.tf.edi = 0x100; f.tf.eax = 0x1234; f.tf.user_esp = 0x100;
    CHECK(!dosvm_vga_fault(v, &f) && f.tf.cs == 0xF000 && f.tf.eip == 0 && (f.tf.user_esp & 0xFFFF) == 0xFA);
    uint16_t pushed[3];
    CHECK(!dosvm_read(v, 0x20000 + 0xFA, pushed, sizeof(pushed)) && pushed[0] == 14 && pushed[1] == 0x3000);
    f.tf.cs = 0x3000; f.tf.user_esp = 0x100;
    f.tf.cs = 0xA000; f.tf.eip = 0; CHECK(dosvm_vga_fault(v, &f) == -EFAULT);    /* no fetch from VRAM */
    f.tf.cs = 0x3000; f.es = 0x10000; CHECK(dosvm_vga_fault(v, &f) == -EFAULT);  /* invalid segment */
    /* The trap path uses the CR2 captured before interrupts were enabled. */
    CHECK(!dosvm_boot(v));
    struct task *saved = g_current;
    g_current = v->task; v->continuation = 0x12345678;
    f = vf(0x2F42, 0, 0, 0); f.tf.cs = 0x3000; f.es = 0xB800; f.tf.edi = 4; f.tf.vector = 14; f.tf.err = 7;
    f.tf.user_ss = 0x2000; f.tf.user_esp = 0x100;
    unsigned faults = v->video_faults, burst = v->video_burst;
    fake_cr2 = 0xB8004;
    /* f3-06 burst: after the faulting store the trap path also runs REP STOSW
     * (CX=0) and the aperture load, then stops before the FLD it cannot
     * decode; only the first instruction counts as a fault. */
    CHECK(dosvm_trap(&f.tf) && f.tf.eip == 8 && cvga_peek_vram(&v->vga, 0xB8004) == 'B' &&
          (f.tf.eax & 0xFFFF) == 0x2F42 && v->info.state < DOSVM_STOPPING && v->video_faults == faults + 1 &&
          v->video_burst == burst + 2);
    sti();
    /* A REP STOSW longer than the string budget: its progress is committed
     * and the burst stops on it, as an interrupted REP. */
    f.tf.eip = 0; f.tf.edi = 4; f.tf.ecx = 5000; f.tf.eax = 0x0743; f.tf.vector = 14; burst = v->video_burst;
    CHECK(dosvm_trap(&f.tf) && f.tf.eip == 3 && (f.tf.ecx & 0xFFFF) == 5000 - VIDEO_STRING_BUDGET &&
          (f.tf.edi & 0xFFFF) == 4 + VIDEO_STRING_BUDGET * 2 && v->video_burst == burst + 1 &&
          cvga_peek_vram(&v->vga, 0xB8004 + VIDEO_STRING_BUDGET * 2 - 2) == 0x43);
    sti();
    fake_cr2 = 0x500000; f.tf.eip = 0;
    if (!setjmp(guest_return)) { dosvm_trap(&f.tf); CHECK(false); }
    sti();
    CHECK(v->info.reason == DOSVM_FAULT && v->info.fault_vector == 14 && v->info.fault_address == 0x500000);
    g_current = saved; v->continuation = 0; fake_cr2 = 0xDEAD000;
    destroy(v, true); empty();
    printf("DOS VM video: guarded aperture emulation, REP restart, unsupported forms and trap CR2 PASS\n");
}

/* ---- Presenter: cadence, damage, phases, custom scanout, writer errors ---- */
static void test_video_presenter(void)
{
    struct dosvm *v = create();
    struct ciuki_rect d;
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 1000, &d) == 1 && !d.dst_y && d.height == 480 && d.width == 640);
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 1010, &d) == 0 && !d.height);   /* frame clock */
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 1040, &d) == 0 && !d.height);   /* no change */
    uint8_t ch = 'Z';
    CHECK(!dosvm_write(v, 0xB8000 + 5 * 160 + 10, &ch, 1));
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 1080, &d) == 1 && d.dst_y == 40 + 5 * 16 && d.height == 16);
    /* Cursor phase change redraws only cursor scanlines of text row 0. */
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 1250, &d) == 1 && d.dst_y >= 40 && d.dst_y + d.height <= 56 && d.height);
    /* Blinking attribute: the text-blink phase redraws its row. */
    uint8_t cell[2] = {'B', 0x87};
    CHECK(!dosvm_write(v, 0xB8000 + 10 * 160, cell, 2));
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 1300, &d) == 1 && d.dst_y == 40 + 10 * 16);
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 1510, &d) == 1 && d.dst_y <= 40 + 10 * 16 &&
          d.dst_y + d.height >= 40 + 11 * 16);
    /* Font (plane 2) change: every row. */
    int10(v, 0x1112, 0, 0, 0);
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 1600, &d) == 1 && !d.dst_y && d.height == 480);
    /* A writer error leaves the next frame whole. */
    ch = 'Q'; CHECK(!dosvm_write(v, 0xB8000, &ch, 1));
    row_fail_at = 40;
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 1700, &d) == -EIO && !v->video_presented);
    row_fail_at = UINT32_MAX;
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 1701, &d) == 1 && d.height == 480);
    /* Nine-dot text (720 pixels) is custom scanout: a black frame, then quiet. */
    int10(v, 0x0003, 0, 0, 0);
    cvga_write_port(&v->vga, 0x3C4, 1); cvga_write_port(&v->vga, 0x3C5, v->vga.seq[1] & ~1u);
    cvga_geometry g;
    CHECK(cvga_get_geometry(&v->vga, &g) && g.width > DOSVM_VIDEO_WIDTH);
    memset(frame_buffer, 0xEE, sizeof(frame_buffer));
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 1800, &d) == 1 && d.height == 480);
    for (unsigned i = 0; i < sizeof(frame_buffer); i++) CHECK(!frame_buffer[i]);
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 1840, &d) == 0);
    int10(v, 0x0003, 0, 0, 0);
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 1880, &d) == 1 && d.height == 480);
    destroy(v, true); empty();
    printf("DOS VM video: presenter cadence, row damage, blink/cursor phases and custom scanout PASS\n");
}

/* ---- Golden VMVIDEO.COM stage digests (src/probes/dos/vm_video.asm) ----
 * Each stage repeats the payload's INT 10h calls, port writes and VRAM
 * stores in the same order. The digest is SHA-256 of the 640x480 XRGB frame
 * (little-endian rows) at tick 0. Procedure on a deliberate change: run
 * this harness, copy the printed digests here and into the f3-vm suite's
 * vm-video cases; tests/host/test_dosvm.py checks that both agree. */
static const char *const video_golden[5] = {
    "0893476bb8b2355dd506ca57bba164a2a7b607df49280278f1b3f857e95424de",
    "c4ac397792548fb0228a889fd7567275dd8148637fa48405fef5386851fdf12c",
    "c019f0109e614d8ae01d35638587b23d92148b8d442dfd0deba73fe3a6df527c",
    "a6d61fa11188a1e81da20dc5f018ee49c264fa0beb004587970b061058d3742a",
    "81daa6ab021444158a8c45f09c72de3b13ee45d7ddf22c920a65ac496a4ec9c2",
};
static const struct { unsigned mode, width, height, scale, text; } video_geometry[5] = {
    {0x03, 640, 400, 1, 1}, {0x03, 640, 400, 1, 1}, {0x13, 320, 200, 2, 0},
    {0x13, 320, 240, 2, 0}, {0x12, 640, 480, 1, 0},
};
static void video_stage(struct dosvm *v, unsigned stage)
{
    static const uint16_t modex_crtc[] = {0x2C11, 0x0D06, 0x3E07, 0x4109, 0xEA10, 0xAC11, 0xDF12, 0xE715, 0x0616};
    static const char banner[] = "CiukiOS virtual VGA 80x25";
    uint8_t row[320];
    switch (stage) {
    case 1: {
        int10(v, 0x0003, 0, 0, 0);
        int10(v, 0x1003, 0, 0, 0);
        int10(v, 0x0100, 0, 0x2000, 0);
        for (unsigned i = 0; i < 2000; i++) {
            uint16_t w = (uint16_t)((((i >> 3) & 0xFF) << 8) | (i & 0xFF));
            CHECK(!dosvm_write(v, 0xB8000 + i * 2, &w, 2));
        }
        CHECK(!dosvm_write(v, 0x20000, banner, sizeof(banner) - 1));
        struct v86_frame f = vf(0x1300, 0x001E, sizeof(banner) - 1, 0x1800); f.es = 0x2000; f.tf.ebp = 0;
        CHECK(!dosvm_bios(v, &f, 0x10));
        port_out(v, 0x3C8, 1, 0x41); port_out(v, 0x3C9, 1, 12); port_out(v, 0x3C9, 1, 34); port_out(v, 0x3C9, 1, 56);
        port_out(v, 0x3C7, 1, 0x41);
        CHECK(port_in(v, 0x3C9, 1) == 12 && port_in(v, 0x3C9, 1) == 34 && port_in(v, 0x3C9, 1) == 56);
        break;
    }
    case 2:
        int10(v, 0x1112, 0, 0, 0);
        int10(v, 0x0100, 0, 0x2000, 0);
        for (unsigned i = 0; i < 4000; i++) {
            uint16_t w = (uint16_t)((((i >> 4) & 0xFF) << 8) | (i & 0xFF));
            CHECK(!dosvm_write(v, 0xB8000 + i * 2, &w, 2));
        }
        break;
    case 3:
        int10(v, 0x0013, 0, 0, 0);
        port_out(v, 0x3C8, 1, 0x40);
        for (unsigned k = 0; k < 64; k++) {
            port_out(v, 0x3C9, 1, k); port_out(v, 0x3C9, 1, 63 - k); port_out(v, 0x3C9, 1, (k * 2) & 63);
        }
        for (unsigned y = 0; y < 200; y++) {
            for (unsigned x = 0; x < 320; x++) row[x] = (uint8_t)(x + y);
            CHECK(!dosvm_write(v, 0xA0000 + y * 320, row, 320));
        }
        break;
    case 4: {
        int10(v, 0x0013, 0, 0, 0);
        port_out(v, 0x3C4, 2, 0x0604); port_out(v, 0x3C4, 2, 0x0F02);
        static uint8_t zero[65536];
        CHECK(!dosvm_write(v, 0xA0000, zero, sizeof(zero)));
        port_out(v, 0x3D4, 2, 0x0014); port_out(v, 0x3D4, 2, 0xE317);
        port_out(v, 0x3C2, 1, 0xE3);
        for (unsigned i = 0; i < ARRAY_SIZE(modex_crtc); i++) port_out(v, 0x3D4, 2, modex_crtc[i]);
        for (unsigned p = 0; p < 4; p++) {
            port_out(v, 0x3C4, 2, (uint32_t)((1u << p) << 8 | 2));
            for (unsigned y = 0; y < 240; y++) {
                for (unsigned k = 0; k < 80; k++) row[k] = (uint8_t)(p + y + 4 * k);
                CHECK(!dosvm_write(v, 0xA0000 + y * 80, row, 80));
            }
        }
        break;
    }
    case 5:
        int10(v, 0x0012, 0, 0, 0);
        for (unsigned p = 0; p < 4; p++) {
            port_out(v, 0x3C4, 2, (uint32_t)((1u << p) << 8 | 2));
            for (unsigned y = 0; y < 480; y++) {
                for (unsigned bx = 0; bx < 80; bx++) row[bx] = ((((bx + (y >> 3)) & 15) >> p) & 1) ? 0xFF : 0;
                CHECK(!dosvm_write(v, 0xA0000 + y * 80, row, 80));
            }
        }
        break;
    default: CHECK(false);
    }
}
/* DOSVM_VIDEO_DUMP=<directory> writes each stage frame as a P6 PPM for review. */
static void dump_frame(unsigned stage)
{
    const char *directory = getenv("DOSVM_VIDEO_DUMP");
    if (!directory) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/vmvideo-stage%u.ppm", directory, stage);
    FILE *out = fopen(path, "wb");
    CHECK(out);
    fprintf(out, "P6\n%u %u\n255\n", DOSVM_VIDEO_WIDTH, DOSVM_VIDEO_HEIGHT);
    for (unsigned i = 0; i < sizeof(frame_buffer); i += 4) {
        uint8_t rgb[3] = {frame_buffer[i + 2], frame_buffer[i + 1], frame_buffer[i]};
        CHECK(fwrite(rgb, 1, 3, out) == 3);
    }
    CHECK(!fclose(out));
}
static void test_video_goldens(void)
{
    struct dosvm *v = create();
    bool matched = true;
    for (unsigned stage = 1; stage <= 5; stage++) {
        video_stage(v, stage);
        struct dosvm_frame fr;
        CHECK(!dosvm_frame_digest(v, 0, &fr));
        char hex[65];
        sha256_hex(fr.digest, hex);
        printf("VIDEO golden stage=%u mode=%u width=%u height=%u scale=%u digest=%s\n",
               stage, fr.mode, fr.width, fr.height, fr.scale, hex);
        CHECK(fr.mode == video_geometry[stage - 1].mode && fr.width == video_geometry[stage - 1].width &&
              fr.height == video_geometry[stage - 1].height && fr.scale == video_geometry[stage - 1].scale &&
              fr.text == video_geometry[stage - 1].text);
        if (strcmp(hex, video_golden[stage - 1])) matched = false;
        compare_with_reference(v, DOSVM_CURSOR_TICKS + DOSVM_BLINK_TICKS);
        compare_with_reference(v, 0);
        dump_frame(stage);
    }
    CHECK(matched);
    destroy(v, true); empty();
    printf("DOS VM video: five golden VMVIDEO stage digests and retained-presenter equivalence PASS\n");
}

/* ---- Native window boundary: syscall 70 ---- */
#define RECORD 64u
static int dosvm_call(unsigned op, struct ciuki_dosvm_command *c, uint32_t size)
{
    memcpy(user_mem + RECORD, c, sizeof(*c));
    struct trap_frame tf;
    memset(&tf, 0, sizeof(tf));
    tf.ebx = op; tf.ecx = RECORD; tf.edx = size;
    int e = dosvm_client_syscall(&tf);
    memcpy(c, user_mem + RECORD, sizeof(*c));
    CHECK(!pins);
    return e;
}
static struct ciuki_dosvm_command command_for(uint32_t id, uint32_t generation)
{
    struct ciuki_dosvm_command c;
    memset(&c, 0, sizeof(c));
    c.version = CIUKI_DOSVM_VERSION; c.size = sizeof(c); c.id = id; c.generation = generation;
    return c;
}
static int create_window(int32_t fd, const char *name, struct ciuki_dosvm_command *out)
{
    struct ciuki_dosvm_command c = command_for(0, 0);
    c.surface_fd = fd;
    memcpy(c.name, name, strlen(name));
    int e = dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c));
    if (out) *out = c;
    return e;
}
static int key_event(struct ciuki_dosvm_command *c, int code, int value)
{
    struct ciuki_dosvm_command k = command_for(c->id, c->generation);
    k.event.type = CIUKI_INPUT_KEY; k.event.code = code; k.event.value = value;
    return dosvm_call(CIUKI_DOSVM_INPUT, &k, sizeof(k));
}
/* The guest side of a key: the synthetic INT 9 runs once per 8042 byte
 * (f3-01 model), then INT 16h AH=01h/00h reads the BIOS buffer. */
static uint16_t take_key(struct dosvm *v)
{
    while (v->kbd.count) CHECK(!dosvm_kbd_bios(v));
    struct v86_frame f = vf(0x0100, 0, 0, 0);
    CHECK(!dosvm_bios(v, &f, 0x16));
    if (f.tf.eflags & V86_ZF) return 0;
    f = vf(0x0000, 0, 0, 0);
    CHECK(!dosvm_bios(v, &f, 0x16));
    return (uint16_t)f.tf.eax;
}
static unsigned take_bios_count(struct dosvm *v) { CHECK(!dosvm_keyboard_refresh(v)); return v->key_count; }
static uint16_t press(struct dosvm *v, struct ciuki_dosvm_command *c, int code)
{
    CHECK(!key_event(c, code, 1) && !key_event(c, code, 0));
    return take_key(v);
}
static void window_fixtures(void)
{
    static uint8_t pixels[3][DOSVM_VIDEO_WIDTH * DOSVM_VIDEO_HEIGHT * 4];
    memset(fake_proc, 0, sizeof(fake_proc));
    for (unsigned i = 0; i < 3; i++) { fake_proc[i].pid = 10 + i; fake_proc[i].state = PROC_LIVE; }
    fake_proc[0].dosvm_capability = fake_proc[1].dosvm_capability = true;
    for (unsigned i = 0; i < 3; i++) {
        uint32_t w = i == 1 ? 320 : 640, h = i == 1 ? 200 : 480;
        fake_surface[i] = (struct surface){{sizeof(struct ciuki_surface_info), w, h, w * 4, CIUKI_SURFACE_XRGB8888,
                                            (w * h * 4 + 4095) & ~4095u}, pixels[i], 1};
    }
    fake_desc[0] = (struct desktop_description){.kind = DESKTOP_SURFACE, .maximum = PROT_READ | PROT_WRITE, .u.surface = &fake_surface[0]};
    fake_desc[1] = (struct desktop_description){.kind = DESKTOP_SURFACE, .maximum = PROT_READ, .u.surface = &fake_surface[0]};
    fake_desc[2] = (struct desktop_description){.kind = DESKTOP_SURFACE, .maximum = PROT_READ | PROT_WRITE, .u.surface = &fake_surface[1]};
    fake_desc[3] = (struct desktop_description){.kind = DESKTOP_SURFACE, .maximum = PROT_READ | PROT_WRITE, .u.surface = &fake_surface[2]};
    desc_owner[0] = desc_owner[1] = desc_owner[2] = &fake_proc[0]; desc_owner[3] = &fake_proc[1];
    fake_current=&fake_proc[0];
}
static void test_client_boundary(void)
{
    window_fixtures();
    struct ciuki_dosvm_command c, w;
    /* Query/remove use the actual syscall validation and fixture storage. */
    struct ciuki_dosvm_command q=command_for(0,0);q.launch.game=2;
    unsigned before=program_calls;
    fake_current=&fake_proc[2];
    CHECK(dosvm_call(CIUKI_DOSVM_GAME_REMOVE,&q,sizeof(q))==-EPERM && program_calls==before);
    CHECK(!dosvm_call(CIUKI_DOSVM_GAME_QUERY,&q,sizeof(q)) && q.state==1 && q.changed==8192);
    fake_proc[2].programs_capability=true;
    for(unsigned op=CIUKI_DOSVM_GAME_QUERY;op<=CIUKI_DOSVM_GAME_REMOVE;op++){
        q=command_for(0,0);q.launch.game=2;q.version=2;
        CHECK(dosvm_call(op,&q,sizeof(q))==-EOPNOTSUPP);
        q.version=1;q.size--;CHECK(dosvm_call(op,&q,sizeof(q))==-EINVAL);q.size++;
        q.launch.reserved[0]=1;CHECK(dosvm_call(op,&q,sizeof(q))==-EINVAL);q.launch.reserved[0]=0;
        q.state=1;CHECK(dosvm_call(op,&q,sizeof(q))==-EINVAL);q.state=0;
        CHECK(dosvm_call(op,&q,sizeof(q)-1)==-EINVAL);
        q.launch.game=0;CHECK(dosvm_call(op,&q,sizeof(q))==-EINVAL);q.launch.game=4;
        CHECK(dosvm_call(op,&q,sizeof(q))==-EINVAL);q.launch.game=2;
        user_fault=true;CHECK(dosvm_call(op,&q,sizeof(q))==-EFAULT);user_fault=false;
        program_result=1;CHECK(!dosvm_call(op,&q,sizeof(q)) && q.state==2);program_result=0;
    }
    fake_proc[2].programs_capability=false;
    CHECK(!dosvm_game_remove_reserve(2));
    struct dosvm *reserved_vm=0;struct dosvm_config reserved_config={.game=2};
    CHECK(dosvm_create(&reserved_config,&reserved_vm)==-EBUSY && !reserved_vm);
    dosvm_game_remove_release(2);
    program_result=1;
    CHECK(dosvm_create(&reserved_config,&reserved_vm)==-ENOENT && !reserved_vm && !game_creating[2]);
    program_result=0;
    game_creating[2]++;CHECK(dosvm_game_remove_reserve(2)==-EBUSY);game_creating[2]--;
    for(unsigned kernel=0;kernel<3;kernel++){
        slots[0].info.key=(struct dosvm_key){1,1};slots[0].info.state=DOSVM_BOOTING;
        slots[0].game=2;slots[0].kernel=kernel;
        CHECK(dosvm_game_remove_reserve(2)==-EBUSY);
        slots[0].info.state=DOSVM_STOPPING;CHECK(dosvm_game_remove_reserve(2)==-EBUSY);
        memset(&slots[0],0,sizeof(slots[0]));
    }
    /* Authority and record validation, each before any reservation. */
    fake_current = &fake_proc[2];
    CHECK(create_window(0, "CiukiDOS", 0) == -EPERM);
    fake_current = 0;
    CHECK(create_window(0, "CiukiDOS", 0) == -EPERM);
    fake_current = &fake_proc[0];
    c = command_for(0, 0); CHECK(dosvm_call(0, &c, sizeof(c)) == -EINVAL && dosvm_call(12, &c, sizeof(c)) == -EINVAL);
    c = command_for(0, 0); c.surface_fd = 0; c.name[0] = 'X';
    CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c) - 1) == -EINVAL);
    c.version = 2; CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c)) == -EOPNOTSUPP);
    c.version = CIUKI_DOSVM_VERSION; c.size = 100; CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c)) == -EINVAL);
    c.size = sizeof(c); user_fault = true; CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c)) == -EFAULT); user_fault = false;
    c.state = 1; CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c)) == -EINVAL); c.state = 0;
    c.launch.kernel=3; CHECK(dosvm_call(CIUKI_DOSVM_CREATE,&c,sizeof(c))==-EINVAL);
    c.launch.kernel=1; CHECK(dosvm_call(CIUKI_DOSVM_CREATE,&c,sizeof(c))==-EINVAL); /* shell */
    c.launch.game=4; CHECK(dosvm_call(CIUKI_DOSVM_CREATE,&c,sizeof(c))==-EINVAL);
    c.launch.game=1; c.launch.reserved[0]=2; CHECK(dosvm_call(CIUKI_DOSVM_CREATE,&c,sizeof(c))==-EINVAL);
    c.launch.reserved[0]=0; c.launch.reserved[1]=1;
    CHECK(dosvm_call(CIUKI_DOSVM_CREATE,&c,sizeof(c))==-EINVAL); c.launch.reserved[1]=0;
    c.launch.reserved[0]=0; workload_disk=true;
    CHECK(dosvm_call(CIUKI_DOSVM_CREATE,&c,sizeof(c))==-EIO && fake_surface[0].references==1);
    workload_disk=false; c.launch.kernel=c.launch.game=0;
    c.id = 1; CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c)) == -EINVAL); c.id = 0;
    c.name[0] = 0; CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c)) == -EINVAL);
    c.name[0] = '\n'; CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c)) == -EINVAL);
    memset(c.name, 'N', sizeof(c.name)); CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c)) == -EINVAL);
    memset(c.name, 0, sizeof(c.name)); c.name[0] = 'A'; c.name[2] = 'B'; /* bytes after the NUL */
    CHECK(dosvm_call(CIUKI_DOSVM_CREATE, &c, sizeof(c)) == -EINVAL);
    CHECK(create_window(9, "CiukiDOS", 0) == -EBADF && create_window(1, "CiukiDOS", 0) == -EACCES &&
          create_window(2, "CiukiDOS", 0) == -EINVAL && create_window(3, "CiukiDOS", 0) == -EBADF);
    { struct dosvm_ledger l; dosvm_snapshot(&l); CHECK(!l.vms && fake_surface[0].references == 1); }
    /* The removed/pending-game errno must reach DOS Box's clear message. */
    c=command_for(0,0);c.surface_fd=0;strcpy(c.name,"Removed");c.launch.game=2;
    program_result=-ENOENT;CHECK(dosvm_call(CIUKI_DOSVM_CREATE,&c,sizeof(c))==-ENOENT);
    program_result=1;CHECK(dosvm_call(CIUKI_DOSVM_CREATE,&c,sizeof(c))==-ENOENT);
    program_result=0;
    /* Creation, one VM per owner, status and presentation. */
    CHECK(!create_window(0, "CiukiDOS", &w) && w.id && w.generation && w.state == CIUKI_DOSVM_BOOTING &&
          w.video_mode == 3 && fake_surface[0].references == 2);
    struct dosvm *v = dosvm_lookup((struct dosvm_key){w.id, w.generation});
    CHECK(v && v->owner_pid == 10 && !strcmp(v->name, "CiukiDOS") && v->info.state == DOSVM_BOOTING);
    CHECK(create_window(0, "Second", 0) == -EBUSY);
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_STATUS, &c, sizeof(c)) && c.state == CIUKI_DOSVM_BOOTING && !c.reason && !c.focused);
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT, &c, sizeof(c)) && c.changed == 1 && !c.damage.dst_y &&
          c.damage.height == 480 && c.damage.width == 640 && c.video_mode == 3);
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT, &c, sizeof(c)) && !c.changed);
    c = command_for(w.id, w.generation + 1); CHECK(dosvm_call(CIUKI_DOSVM_STATUS, &c, sizeof(c)) == -ESRCH);
    c = command_for(DOSVM_LIMIT + 1, w.generation); CHECK(dosvm_call(CIUKI_DOSVM_STATUS, &c, sizeof(c)) == -ESRCH);
    fake_current = &fake_proc[1];
    c = command_for(w.id, w.generation); CHECK(dosvm_call(CIUKI_DOSVM_PRESENT, &c, sizeof(c)) == -ESRCH);
    fake_current = &fake_proc[0];
    /* Focus and the f3-01 device path: set-1 bytes, IRQ1, synthetic INT 9. */
    CHECK(key_event(&w, 0x1E, 1) == -EACCES && !v->kbd.count && !take_key(v));
    c = command_for(w.id, w.generation); c.focused = 2; CHECK(dosvm_call(CIUKI_DOSVM_FOCUS, &c, sizeof(c)) == -EINVAL);
    c = command_for(w.id, w.generation); c.focused = 1; CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS, &c, sizeof(c)));
    CHECK(v->window_input && !key_event(&w, 0x1E, 1) && v->kbd.count == 1 && v->kbd.bytes[v->kbd.head] == 0x1E &&
          (v->monitor.pic[0].irr & 2) && take_key(v) == 0x1E61 && !key_event(&w, 0x1E, 0) && !take_key(v));
    CHECK(press(v, &w, 0x1C) == 0x1C0D && press(v, &w, 0x2B) == 0x2B5C &&
          press(v, &w, 0x148) == 0x4800 && press(v, &w, 0x3B) == 0x3B00 && press(v, &w, 0x0E) == 0x0E08 &&
          press(v, &w, 0x01) == 0x011B && press(v, &w, 0x153) == 0x5300 && press(v, &w, 0x11C) == 0x1C0D &&
          press(v, &w, 0x135) == 0x352F && press(v, &w, 0x57) == 0x8500 && press(v, &w, 0x4A) == 0x4A2D);
    CHECK(!key_event(&w, 0x2A, 1));
    CHECK(press(v, &w, 0x1E) == 0x1E41 && press(v, &w, 0x27) == 0x273A && press(v, &w, 0x03) == 0x0340 &&
          press(v, &w, 0x0F) == 0x0F00 && press(v, &w, 0x3C) == 0x5500);
    CHECK(!key_event(&w, 0x2A, 0) && press(v, &w, 0x3A) == 0);
    CHECK(press(v, &w, 0x1E) == 0x1E41 && press(v, &w, 0x02) == 0x0231);
    CHECK(press(v, &w, 0x3A) == 0 && press(v, &w, 0x47) == 0x4700 && press(v, &w, 0x45) == 0 &&
          press(v, &w, 0x47) == 0x4737 && press(v, &w, 0x45) == 0 && !key_event(&w, 0x1D, 1));
    CHECK(press(v, &w, 0x2E) == 0x2E03 && press(v, &w, 0x14B) == 0x7300 && press(v, &w, 0x1C) == 0x1C0A &&
          press(v, &w, 0x07) == 0x071E);
    CHECK(!key_event(&w, 0x1D, 0) && !key_event(&w, 0x38, 1));
    CHECK(press(v, &w, 0x1E) == 0x1E00 && press(v, &w, 0x02) == 0x7800 && press(v, &w, 0x58) == 0x8C00);
    CHECK(!key_event(&w, 0x38, 0) && !take_key(v));
    CHECK(!key_event(&w, 0x1E, 1) && !key_event(&w, 0x1E, 2) && !key_event(&w, 0x1E, 0) &&
          take_key(v) == 0x1E61 && take_key(v) == 0x1E61 && !take_key(v));              /* typematic repeat */
    c = command_for(w.id, w.generation); c.event.type = CIUKI_INPUT_TEXT; c.event.code = 'a';
    CHECK(!dosvm_call(CIUKI_DOSVM_INPUT, &c, sizeof(c)) && !v->kbd.count && !take_key(v));
    c = command_for(w.id, w.generation); c.event.type = CIUKI_INPUT_MOTION; c.event.value = 5; c.event.value2 = 7;
    CHECK(!dosvm_call(CIUKI_DOSVM_INPUT, &c, sizeof(c)) && !take_key(v));
    CHECK(key_event(&w, 0x201, 1) == -EINVAL && key_event(&w, 0x1E, 3) == -EINVAL);
    c = command_for(w.id, w.generation); c.event.type = CIUKI_INPUT_BUTTON; c.event.code = 4; c.event.value = 1;
    CHECK(dosvm_call(CIUKI_DOSVM_INPUT, &c, sizeof(c)) == -EINVAL);
    c = command_for(w.id, w.generation); c.event.type = 9; CHECK(dosvm_call(CIUKI_DOSVM_INPUT, &c, sizeof(c)) == -EINVAL);
    /* An undrained 8042 queue overflows: the event is refused (EAGAIN). */
    unsigned queued = 0;
    while (!key_event(&w, 0x1E, 1) && !key_event(&w, 0x1E, 0)) queued++;
    CHECK(queued > 100 && queued <= DOSVM_KBD_BYTES / 2 && v->kbd.overflows && key_event(&w, 0x30, 1) == -EAGAIN);
    while (take_key(v)) {}                                           /* drain the BIOS buffer */
    if (v->kbd.held[0x1E >> 3] & (1u << (0x1E & 7))) CHECK(!key_event(&w, 0x1E, 0) && !take_key(v));
    /* Focus loss releases held keys (break codes reach INT 9); RESYNC likewise. */
    CHECK(!key_event(&w, 0x2A, 1));
    c = command_for(w.id, w.generation); CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS, &c, sizeof(c)));
    CHECK(key_event(&w, 0x1E, 1) == -EACCES);
    c = command_for(w.id, w.generation); c.focused = 1; CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS, &c, sizeof(c)));
    CHECK(press(v, &w, 0x1E) == 0x1E61 && !key_event(&w, 0x36, 1));
    c = command_for(w.id, w.generation); c.event.type = CIUKI_INPUT_RESYNC;
    CHECK(!dosvm_call(CIUKI_DOSVM_INPUT, &c, sizeof(c)) && press(v, &w, 0x1E) == 0x1E61);
    /* Buttons and motion reach the INT 33h model while focused. */
    c = command_for(w.id, w.generation); c.event.type = CIUKI_INPUT_BUTTON; c.event.code = 1; c.event.value = 1;
    CHECK(!dosvm_call(CIUKI_DOSVM_INPUT, &c, sizeof(c)) && (v->mouse.buttons & 1));
    c.event.value = 0; CHECK(!dosvm_call(CIUKI_DOSVM_INPUT, &c, sizeof(c)) && !(v->mouse.buttons & 1));
    /* Status reports a guest-requested stop; close does not force a stopped VM. */
    uint32_t zero = 0;
    CHECK(!v86_io(&v->monitor, DOSVM_CONTROL_PORT, 1, true, &zero) && v->info.state == DOSVM_STOPPING);
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_STATUS, &c, sizeof(c)) && c.state == CIUKI_DOSVM_STOPPED && c.reason == DOSVM_EXIT);
    c = command_for(w.id, w.generation); CHECK(dosvm_call(CIUKI_DOSVM_PRESENT, &c, sizeof(c)) == -ESRCH);
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE, &c, sizeof(c)) && v->info.state == DOSVM_DEAD && !v->info.data_loss &&
          fake_surface[0].references == 1);
    c = command_for(w.id, w.generation); CHECK(dosvm_call(CIUKI_DOSVM_CLOSE, &c, sizeof(c)) == -ESRCH);
    empty();
    /* Two owners hold two VMs; the owner's death collects its VM only. */
    CHECK(!create_window(0, "First", &w));
    fake_current = &fake_proc[1];
    struct ciuki_dosvm_command other;
    CHECK(!create_window(3, "Other", &other) && other.id != w.id && fake_surface[2].references == 2);
    fake_proc[0].state = PROC_STOPPING;
    bindings[w.id - 1].busy = true;
    dosvm_client_poll();
    CHECK(dosvm_valid((struct dosvm_key){w.id, w.generation}));          /* busy: next pass */
    bindings[w.id - 1].busy = false;
    dosvm_client_poll();
    CHECK(!dosvm_valid((struct dosvm_key){w.id, w.generation}) && fake_surface[0].references == 1 && !bindings[w.id - 1].pid);
    CHECK(dosvm_valid((struct dosvm_key){other.id, other.generation}));
    fake_proc[0].state = PROC_LIVE;
    fake_proc[1].pid = 0;                                                 /* reaped */
    dosvm_client_poll();
    CHECK(!dosvm_valid((struct dosvm_key){other.id, other.generation}) && fake_surface[2].references == 1);
    fake_proc[1].pid = 11;
    empty();
    /* Owner stopping during creation, allocation and storage failures. */
    fake_current = &fake_proc[0];
    fake_proc[0].state = PROC_STOPPING;
    CHECK(create_window(0, "Late", 0) == -EINTR && fake_surface[0].references == 1);
    fake_proc[0].state = PROC_LIVE;
    empty();
    allocation_calls = 0; fail_at = 1;
    CHECK(create_window(0, "NoMemory", 0) == -ENOMEM && fake_surface[0].references == 1);
    fail_at = 0; empty();
    io_calls = 0; io_fail_at = 1;
    CHECK(create_window(0, "NoDisk", 0) == -EIO && fake_surface[0].references == 1);
    io_fail_at = 0; empty();
    bad_signature = true;
    CHECK(create_window(0, "NoBoot", 0) == -EIO && fake_surface[0].references == 1);
    bad_signature = false; empty();
    /* A live VM closed by its window is forced, and reports possible loss. */
    CHECK(!create_window(0, "Forced", &w));
    v = dosvm_lookup((struct dosvm_key){w.id, w.generation});
    uint8_t sector[512] = {1};
    CHECK(!dosvm_disk_write(v, 9, sector));
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE, &c, sizeof(c)) && v->info.state == DOSVM_DEAD && v->info.data_loss &&
          v->info.reason == DOSVM_TERMINATE && fake_surface[0].references == 1);
    empty();
    fake_current = 0;
    printf("DOS VM window boundary: authority, record validation, focus/keyboard adapter, owner death and failures PASS\n");
}

/* f3-02 QEMU finding (vm-window, 2026-10-11): the shell waits for a line in
 * INT 16h AH=00h, which blocked inside the monitor until the BIOS buffer held
 * a key, while window keys only reach the virtual 8042 and need the guest's
 * INT 9 to get there: the runner's keys stayed queued until the controller
 * typed into the BIOS buffer directly, and three of them then advanced
 * VMVIDEO from stage 1 to 4. This test drives the runner's exact 30 key
 * events through syscall 72 into a guest already blocked in INT 16h AH=00h,
 * then runs the production ROM stubs (F000:0090 INT 9, F000:0160 INT 16h)
 * through dosvm_trap: only trapping instructions execute, as in
 * test_device_irq_delivery. */
static struct ciuki_dosvm_command typing_window;
static unsigned typing_sleeps, typing_pending;
static const struct { int code, value; } typing_keys[] = {
    {0x2B,1},{0x2B,0},{0x2A,1},{0x2F,1},{0x2F,0},{0x32,1},{0x32,0},{0x2F,1},{0x2F,0},{0x17,1},{0x17,0},
    {0x20,1},{0x20,0},{0x12,1},{0x12,0},{0x18,1},{0x18,0},{0x2A,0},{0x34,1},{0x34,0},{0x2A,1},{0x2E,1},
    {0x2E,0},{0x18,1},{0x18,0},{0x32,1},{0x32,0},{0x2A,0},{0x1C,1},{0x1C,0}};   /* \VMVIDEO.COM Enter (f3-vm.json) */
static void typing_hook(void)
{
    /* The guest is blocked in the AH=00h wait; the first key arrives on the
     * 5th scheduler sleep. Any wait after that is the old deadlock. */
    typing_sleeps++;
    if (typing_sleeps == 5) {
        CHECK(!key_event(&typing_window, typing_keys[0].code, typing_keys[0].value) &&
              !key_event(&typing_window, typing_keys[1].code, typing_keys[1].value));
        typing_pending = 2;
    }
    if (typing_pending && typing_sleeps > 205) {
        fprintf(stderr, "FAIL: INT 16h AH=00h still blocked 200 ms after window keys reached the 8042\n");
        exit(1);
    }
}
/* F000:0090: INT FEh; PUSH AX; MOV AL,20h; OUT 20h,AL; POP AX; IRET. */
static void typing_rom_int9(struct dosvm *v, struct v86_frame *f)
{
    CHECK(device_trap(v, f, 13) && f->tf.cs == 0xF000 && f->tf.eip == 0x92);     /* synthetic INT 9 */
    uint32_t ax = f->tf.eax;
    f->tf.user_esp -= 2; f->tf.eax = (ax & ~0xFFu) | 0x20; f->tf.eip = 0x95;     /* PUSH AX; MOV AL,20h */
    CHECK(device_trap(v, f, 13) && f->tf.eip == 0x97 && !(v->monitor.pic[0].isr & 2)); /* OUT 20h: EOI */
    f->tf.user_esp += 2; f->tf.eax = ax; f->tf.eip = 0x98;                        /* POP AX */
    CHECK(device_trap(v, f, 13));                                                 /* IRET */
}
/* One INT 16h AH=00h at 0200:0000; returns AX at the caller's next byte. */
static uint16_t typing_read(struct dosvm *v, struct v86_frame *f)
{
    const uint8_t int16[] = {0xCD, 0x16};
    f->tf.cs = 0x200; f->tf.eip = 0;
    CHECK(device_insn(v, f, 0, int16, 2) && f->tf.cs == 0xF000 && f->tf.eip == 0x160 && !v->monitor.vif);
    for (unsigned steps = 0; steps < 512; steps++) {
        if (f->tf.cs == 0xF000 && f->tf.eip == 0x90) { typing_rom_int9(v, f); continue; }
        CHECK(f->tf.cs == 0xF000 && f->tf.eip == 0x160);
        CHECK(device_trap(v, f, 13));                                             /* INT FEh: AH=00h */
        if (f->tf.cs == 0xF000 && f->tf.eip == 0x162) {
            uint16_t key = (uint16_t)f->tf.eax;
            CHECK(device_trap(v, f, 13) && f->tf.cs == 0x200 && f->tf.eip == 2 && v->monitor.vif); /* IRET */
            return key;
        }
    }
    CHECK(false);
    return 0;
}
static void test_client_typing(void)
{
    memset(fake_proc, 0, sizeof(fake_proc));
    fake_proc[0].pid = 10; fake_proc[0].state = PROC_LIVE; fake_proc[0].dosvm_capability = true;
    fake_current = &fake_proc[0];
    struct ciuki_dosvm_command c;
    CHECK(!create_window(0, "Typing", &typing_window));
    struct dosvm *v = dosvm_lookup((struct dosvm_key){typing_window.id, typing_window.generation});
    CHECK(v); v->info.dos_ready = true;
    c = command_for(typing_window.id, typing_window.generation); c.focused = 1;
    CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS, &c, sizeof(c)));      /* the desktop's FOCUS after CREATE_WINDOW */
    v->monitor.pic[0].imr = 0xFD;                               /* IRQ1 only: no ROM timer stub in this walk */
    struct v86_frame f = {0};
    f.tf.user_ss = 0x300; f.tf.user_esp = 0x100; f.tf.eflags = V86_VM | V86_IF | 2;
    v->monitor.vif = true; v->monitor.state = V86_RUNNING;
    /* Blocked first, as the shell is when the runner starts typing. */
    typing_sleeps = typing_pending = 0; sleep_hook = typing_hook;
    uint16_t words[13]; unsigned n = 0;
    words[n++] = typing_read(v, &f);
    CHECK(typing_sleeps >= 5 && v->monitor.stats.delivered[1] == 2 && !v->kbd.count);
    sleep_hook = 0;
    /* The rest of the line arrives while the shell echoes: all 28 events queue
     * in the 8042 before the next read, which runs one INT 9 per byte. */
    for (unsigned i = 2; i < ARRAY_SIZE(typing_keys); i++)
        CHECK(!key_event(&typing_window, typing_keys[i].code, typing_keys[i].value));
    CHECK(v->kbd.count == 28 && !take_bios_count(v));
    while (n < ARRAY_SIZE(words)) words[n++] = typing_read(v, &f);
    static const uint16_t line[13] = {0x2B5C, 0x2F56, 0x324D, 0x2F56, 0x1749, 0x2044, 0x1245, 0x184F,
                                      0x342E, 0x2E43, 0x184F, 0x324D, 0x1C0D};
    CHECK(!memcmp(words, line, sizeof(line)) && !v->kbd.count && !take_bios_count(v) && !v->kbd.shift);
    CHECK(v->monitor.stats.delivered[1] == 30 && !v->monitor.pic[0].isr && !(v->monitor.pic[0].irr & 2));
    /* Nothing queued: the read blocks again (no spin through the stub). */
    typing_sleeps = 0; typing_pending = 0; sleep_hook = device_hook;
    device_hook_arm(v, 50, device_hang_stop);
    const uint8_t int16[] = {0xCD, 0x16};
    f.tf.cs = 0x200; f.tf.eip = 0;
    CHECK(device_insn(v, &f, 0, int16, 2) && f.tf.eip == 0x160 && !device_trap(v, &f, 13));
    CHECK(!sleep_hook && v->info.reason == DOSVM_TERMINATE);
    c = command_for(typing_window.id, typing_window.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE, &c, sizeof(c)) && v->info.state == DOSVM_DEAD);
    empty();
    fake_current = 0;
    printf("DOS VM window typing: runner keys through syscall 72, 8042, IRQ1 and the ROM INT 9 into a blocked INT 16h AH=00h PASS\n");
}

/* f3-06 QEMU finding (wolf3d-qemu-t23, 2026-10-12): Wolf3D's SignonScreen
 * calls VW_SetScreen(0x8000,0) before SD_Startup installs its timer, so id's
 * VL_SetScreen (WOLFSRC/ID_VL_A.ASM) can leave its loop only through five
 * successive 3DAh reads with display disabled (bit 0) and no vertical sync
 * (bit 3): the bottom of the screen. The VM was found spinning there with
 * AX=5201 CX=0002 DX=03DA. Here every IN AL,DX is a real #GP through
 * dosvm_trap and v86_emulate to the production port handler, with the
 * guest's IF clear, and the host tick advances once per `rate` reads. */
static unsigned retrace_rate, retrace_reads;
static uint8_t retrace_in(struct dosvm *v, struct v86_frame *f)
{
    static const uint8_t in_al_dx = 0xEC;
    f->tf.cs = 0x3855; f->tf.eip = 0x6E; f->tf.edx = 0x3DA;
    CHECK(device_insn(v, f, 0x5200, &in_al_dx, 1) && f->tf.eip == 0x6F);
    if (++retrace_reads % retrace_rate == 0) g_ticks++;
    return (uint8_t)f->tf.eax;
}
/* VL_SetScreen's wait; returns the reads it took, 0 when over limit. */
static unsigned retrace_set_screen(struct dosvm *v, struct v86_frame *f, unsigned limit)
{
    unsigned start = retrace_reads;
waitdisplay:
    while (retrace_in(v, f) & 1) if (retrace_reads - start > limit) return 0;
loop:   /* sti; jmp $+2; cli; cmp [timecount],cx: TimeCount stays 0 */
    for (unsigned k = 0; k < 5; k++) {
        uint8_t al = retrace_in(v, f);
        if (retrace_reads - start > limit) return 0;
        if (al & 8) goto waitdisplay;
        if (!(al & 1)) goto loop;
    }
    return retrace_reads - start;                                /* @@setcrtc */
}
/* VL_WaitVBL(1): no sync, then sync. */
static bool retrace_wait_vbl(struct dosvm *v, struct v86_frame *f, unsigned limit)
{
    unsigned start = retrace_reads;
    while (retrace_in(v, f) & 8) if (retrace_reads - start > limit) return false;
    while (!(retrace_in(v, f) & 8)) if (retrace_reads - start > limit) return false;
    return true;
}
/* Sync starts per second over 140 consecutive VL_WaitVBL(1) calls. */
static unsigned retrace_rate_hz(struct dosvm *v, struct v86_frame *f)
{
    CHECK(retrace_wait_vbl(v, f, 100000));
    uint64_t t0 = g_ticks;
    for (unsigned i = 0; i < 140; i++) CHECK(retrace_wait_vbl(v, f, 100000));
    return (unsigned)((140000u + (g_ticks - t0) / 2) / (g_ticks - t0));
}
static void test_video_retrace(void)
{
    struct dosvm *v = create();
    CHECK(!dosvm_boot(v)); v->info.dos_ready = true;
    struct v86_frame f = vf(0x0013, 0, 0, 0);
    CHECK(!dosvm_vga_bios(v, &f));                               /* VL_SetVGAPlaneMode: INT 10h AX=0013h */
    port_out(v, 0x3C4, 2, 0x0604); port_out(v, 0x3D4, 2, 0x0014); port_out(v, 0x3D4, 2, 0xE317); /* VL_DePlaneVGA */
    port_out(v, 0x3C4, 2, 0x0F02); port_out(v, 0x3D4, 2, 0x1413);                                /* map mask, line width 40 */
    f = (struct v86_frame){0}; f.tf.user_ss = 0x529F; f.tf.user_esp = 0xFFBA; f.tf.eflags = V86_VM | V86_IF | 2;
    v->monitor.vif = false;                                      /* CLI, as in VL_SetScreen */
    g_ticks = 5000; retrace_reads = 0;
    const unsigned rates[] = {50, 5, 1};                         /* reads per host millisecond */
    for (unsigned i = 0; i < 3; i++) {
        retrace_rate = rates[i];
        uint64_t t0 = g_ticks;
        unsigned reads = retrace_set_screen(v, &f, 200000);
        printf("VL_SetScreen(0x8000,0) mode Y IF=0 rate=%u reads/ms: %u reads, %llu ms\n",
               rates[i], reads, (unsigned long long)(g_ticks - t0));
        CHECK(reads && g_ticks - t0 <= 30 + 40 / rates[i]);     /* two frames plus one blank */
    }
    retrace_rate = 50;
    unsigned hz13 = retrace_rate_hz(v, &f);
    v->monitor.vif = true;                                       /* the guest's IF does not matter */
    unsigned hz13_if = retrace_rate_hz(v, &f);
    f = vf(0x0012, 0, 0, 0); CHECK(!dosvm_vga_bios(v, &f));
    f = (struct v86_frame){0}; f.tf.user_ss = 0x529F; f.tf.user_esp = 0xFFBA; f.tf.eflags = V86_VM | V86_IF | 2;
    unsigned hz12 = retrace_rate_hz(v, &f);
    printf("VL_WaitVBL rate: mode Y %u Hz (IF=0), %u Hz (IF=1), mode 12h %u Hz\n", hz13, hz13_if, hz12);
    CHECK(hz13 == 70 && hz13_if == 70 && hz12 == 60);
    destroy(v, true); empty();
    printf("DOS VM video: Wolf3D VL_SetScreen/VL_WaitVBL loops on 3DAh through dosvm_trap, 70/60 Hz from the programmed timing, IF-independent PASS\n");
}

static void test_video(void)
{
    test_video_bios_ports();
    test_video_retrace();
    test_video_faults();
    test_planar_latch_fallback();
    test_video_presenter();
    test_video_goldens();
    test_client_boundary();
    test_client_typing();
}
