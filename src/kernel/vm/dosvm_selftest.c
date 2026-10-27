/* fx-selftest DOS full-screen checks: the desktop starts /Programs/dosbox on
 * request (device record 0x7F000), the controller types \FSTEST.COM, and
 * each stage enters and leaves full screen through the production path
 * (dosvm_fullscreen_enter/leave, the same calls as Alt+Enter), then EXIT
 * closes the window. The window's disk overlay is the DOS box's own
 * temporary file, removed when the VM stops (docs/design/self-test.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/dosvm.h>
#include <ciuki/task.h>
#include <ciuki/storage.h>
#include <ciuki/sync.h>
#include <ciuki/process.h>
#include <ciuki/supervisor.h>
#include <ciuki/fbdev.h>
#include <ciuki/display.h>
#include <ciuki/selftest.h>
#include "dosvm_internal.h"

#pragma weak display_selftest_info
#pragma weak biosvm_video_state_snapshot

static struct dosvm *vm;
static struct dosvm_key key;
static uint32_t client;
static struct dosvm_ledger baseline;
static bool baseline_taken;
/* The window's temporary copy-on-write file (dosvm_disk.c): dos.return
 * proves the DOS box removed it, so the run leaves no file but its log. */
static char overlay_path[sizeof(((struct dosvm *)0)->overlay_path)];
static struct vfs_table overlay_table;

static uint64_t now(void) { return g_ticks; }
static bool vm_live(void) { return vm && dosvm_valid(key); }
static void skip(struct selftest_result *r, const char *why) { selftest_set(r, SELFTEST_SKIP, why); }
static void fail(struct selftest_result *r, const char *why) { selftest_set(r, SELFTEST_FAIL, why); }

/* "C:\...>" on the 80x25 text screen (f3 probes' prompt rule). */
static bool prompt_shown(struct dosvm *v)
{
    const uint16_t *text = dosvm_text(v, 0);
    if (!text) return false;
    for (unsigned row = 0; row < 25; row++) {
        bool drive = false;
        for (unsigned col = 0; col < 80; col++) {
            unsigned i = row * 80 + col;
            if (col < 78 && (uint8_t)text[i] == 'C' && (uint8_t)text[i + 1] == ':' && (uint8_t)text[i + 2] == '\\')
                drive = true;
            if (drive && (uint8_t)text[i] == '>') return true;
        }
    }
    return false;
}
static bool at_prompt(void)
{
    if (!vm_live()) return false;
    struct dosvm_info info;
    dosvm_info(vm, &info);
    struct dosvm_video_info video;
    return info.dos_ready && dosvm_video_info(key, &video) && video.mode == 3 && prompt_shown(vm);
}
static int type_text(const char *text, uint64_t until)
{
    for (unsigned i = 0; text[i]; i++) {
        while (!dosvm_keyboard(key, (uint8_t)text[i])) {
            if (!vm_live() || now() >= until) return -EIO;
            task_sleep_ms(10);
        }
        task_sleep_ms(10);
    }
    while (!dosvm_keyboard(key, 0x1C0D)) {
        if (!vm_live() || now() >= until) return -EIO;
        task_sleep_ms(10);
    }
    return 0;
}
static int press_enter(uint64_t until) { return type_text("", until); }

void selftest_dos_window(void *ctx, const struct selftest_check *c, uint64_t until, struct selftest_result *r)
{
    (void)ctx; (void)c;
    vm = 0; client = 0; overlay_path[0] = 0;
    if (!supervisor_desktop_pid()) { skip(r, "no_desktop"); return; }
    dosvm_snapshot(&baseline); baseline_taken = true;
    uint64_t t0 = now();
    selftest_request_dosbox();
    struct dosvm *v = 0;
    while (!(v = dosvm_client_first(&client))) {
        if (now() + 500 >= until || now() - t0 > 40000) break;
        task_sleep_ms(20);
    }
    selftest_figures(r, "vms_before=%u launch_ms=%llu", baseline.vms, (unsigned long long)(now() - t0));
    if (!v) { selftest_device_running(); fail(r, "no_dos_window"); return; }
    vm = v; key = dosvm_key(v);
    uint64_t t1 = now();
    while (!at_prompt()) {
        if (!vm_live() || now() + 300 >= until) break;
        task_sleep_ms(20);
    }
    struct dosvm_info info = {0};
    if (vm_live()) dosvm_info(vm, &info);
    if (vm_live() && vm->overlay_created) {
        memcpy(overlay_path, vm->overlay_path, sizeof(overlay_path));
        overlay_path[sizeof(overlay_path) - 1] = 0;
    }
    bool focused = vm_live() && dosvm_focused_key().id == key.id && dosvm_focused_key().generation == key.generation;
    selftest_figures(r, "vm=%u generation=%u client=%u kernel=%u dos=%u.%u free_paras=%u prompt_ms=%llu focused=%u window_input=%u",
                     key.id, key.generation, client, vm_live() ? vm->kernel : 0, info.dos_major, info.dos_minor,
                     info.free_paragraphs, (unsigned long long)(now() - t1), focused, vm_live() ? vm->window_input : 0);
    if (!vm_live()) fail(r, "vm_stopped");
    else if (!at_prompt()) fail(r, "no_prompt");
    else selftest_set(r, SELFTEST_PASS, 0);
}

static bool repainted(void)
{
    struct display_selftest_info d = {0};
    if (display_selftest_info) display_selftest_info(&d);
    return !d.frame_pending;
}
static bool layout_same(const struct fb_device *a, const struct fb_device *b)
{
    return a->present && b->present && a->width == b->width && a->height == b->height &&
           a->bpp == b->bpp && a->pitch == b->pitch && a->phys == b->phys;
}
/* Enter, hold one second, leave; the window and the desktop mode return. */
static void fullscreen_round(uint16_t mode, uint64_t until, struct selftest_result *r)
{
    const struct fb_device desktop = *fbdev_get();
    struct biosvm_video_state before = {0}, after = {0};
    if (biosvm_video_state_snapshot) biosvm_video_state_snapshot(&before);
    uint64_t t0 = now();
    int e;
    do {
        e = dosvm_fullscreen_enter(key);
        if (e == -EBUSY || e == -EACCES) task_sleep_ms(20);
    } while ((e == -EBUSY || e == -EACCES) && now() - t0 < 4000 && vm_live());
    uint64_t enter_ms = now() - t0;
    int refusal = vm_live() ? vm->fullscreen_refusal : 0;
    selftest_figures(r, "mode=0x%x enter=%d enter_ms=%llu", mode, e, (unsigned long long)enter_ms);
    if (e == DISPLAY_FULLSCREEN_WINDOWED) {
        selftest_figures(r, "refusal=%d", refusal);
        if (mode > 0x7F && !display_fullscreen_supported(mode)) skip(r, "windowed_by_policy");
        else fail(r, "refused");
        return;
    }
    if (e) { fail(r, e == -EACCES ? "not_focused" : "enter"); return; }
    uint64_t held = now();
    bool stayed = true;
    while (now() - held < 1000) {
        if (!dosvm_fullscreen_active(key)) { stayed = false; break; }
        task_sleep_ms(10);
    }
    uint64_t held_ms = now() - held;
    t0 = now();
    int leave = 0;
    if (dosvm_fullscreen_active(key))
        do {
            leave = dosvm_fullscreen_leave(key, false);
            if (leave == -EBUSY) task_sleep_ms(10);
        } while (leave == -EBUSY && now() - t0 < 4000);
    uint64_t leave_ms = now() - t0;
    t0 = now();
    bool painted = true;
    while (!repainted()) {
        if (now() + 200 >= until || now() - t0 > 10000) { painted = false; break; }
        task_sleep_ms(10);
    }
    uint64_t repaint_ms = now() - t0;
    if (biosvm_video_state_snapshot) biosvm_video_state_snapshot(&after);
    struct display_selftest_info d = {0};
    if (display_selftest_info) display_selftest_info(&d);
    bool windowed = vm_live() && !dosvm_fullscreen_active(key);
    bool restored = layout_same(fbdev_get(), &desktop);
    int error = vm_live() ? vm->fullscreen_error : -ESRCH;
    bool suspended = vm_live() && vm->fullscreen_suspended;
    selftest_token(r->figures, sizeof(r->figures), "provider",
                   d.last_provider == 1 ? "native" : d.last_provider == 2 ? "firmware" : "none");
    selftest_figures(r, "held_ms=%llu stayed=%u leave=%d leave_ms=%llu window=%u error=%d suspended=%u restored=%u repaint_ms=%llu repainted=%u",
                     (unsigned long long)held_ms, stayed, leave, (unsigned long long)leave_ms, windowed, error,
                     suspended, restored, (unsigned long long)repaint_ms, painted);
    if (after.queries != before.queries)
        selftest_figures(r, "state_bytes=%u state_save=%d state_restore=%d", after.bytes, after.save_result,
                         after.restore_result);
    if (stayed && !leave && windowed && !error && !suspended && restored && painted) selftest_set(r, SELFTEST_PASS, 0);
    else fail(r, !stayed ? "returned_early" : leave ? "leave" : !windowed ? "still_fullscreen" :
                 error ? "transport_error" : suspended ? "suspended" : !restored ? "desktop_mode" : "no_repaint");
}
static bool video_mode_is(uint16_t mode)
{
    if (!vm_live()) return false;
    if (mode > 0x7F) return vm->vbe_mode == mode;
    struct dosvm_video_info video;
    return dosvm_video_info(key, &video) && video.mode == mode && !vm->vbe_mode;
}
static bool wait_mode(uint16_t mode, uint64_t until)
{
    while (!video_mode_is(mode)) {
        if (!vm_live() || now() + 2000 >= until) return false;
        task_sleep_ms(20);
    }
    return true;
}
void selftest_dos_mode13(void *ctx, const struct selftest_check *c, uint64_t until, struct selftest_result *r)
{
    (void)ctx; (void)c;
    if (!vm_live()) { skip(r, "no_dos_window"); return; }
    uint64_t t0 = now();
    if (type_text("\\FSTEST.COM", until)) { fail(r, "typing"); return; }
    bool set = wait_mode(0x13, until);
    selftest_figures(r, "program_ms=%llu", (unsigned long long)(now() - t0));
    if (!set) { fail(r, "mode_not_set"); return; }
    task_sleep_ms(300); /* the gradient is stored before INT 16h */
    fullscreen_round(0x13, until, r);
}
void selftest_dos_vbe101(void *ctx, const struct selftest_check *c, uint64_t until, struct selftest_result *r)
{
    (void)ctx; (void)c;
    if (!vm_live()) { skip(r, "no_dos_window"); return; }
    if (!video_mode_is(0x13)) { skip(r, "stage1_missing"); return; }
    uint64_t t0 = now();
    if (press_enter(until)) { fail(r, "typing"); return; }
    bool set = wait_mode(0x101, until);
    selftest_figures(r, "vbe_ms=%llu", (unsigned long long)(now() - t0));
    if (!set) { fail(r, "vbe_mode_not_set"); return; }
    task_sleep_ms(300);
    fullscreen_round(0x101, until, r);
}
static bool vm_gone(void) { return !vm_live(); }
/* 1: the overlay file still exists, 0: absent (or none was created), <0:
 * the read-only lookup failed. Opens nothing writable. */
static int overlay_left(void)
{
    if (!overlay_path[0]) return 0;
    struct storage *s = storage_get();
    if (!s || !s->ready || !s->vfs.volumes[2]) return -FS_ENOENT;
    int e = vfs_table_init(&s->vfs, &overlay_table, 2);
    if (e) return e;
    int fd = vfs_open(&overlay_table, overlay_path, VFS_READ, VFS_DENY_NONE, 0);
    if (fd >= 0) (void)vfs_close(&overlay_table, fd);
    vfs_table_destroy(&overlay_table);
    return fd >= 0 ? 1 : fd == -FS_ENOENT ? 0 : fd;
}
/* FSTEST waits for one key per stage; a stage that never ran leaves it one
 * key further back, so Enter is repeated (at most three times). */
#define RETURN_KEYS 3u
void selftest_dos_return(void *ctx, const struct selftest_check *c, uint64_t until, struct selftest_result *r)
{
    (void)ctx; (void)c;
    if (!vm) { selftest_device_running(); skip(r, "no_dos_window"); return; }
    uint64_t t0 = now();
    bool prompt = false, exited = false, forced = false;
    unsigned keys = 0;
    while (vm_live() && !(prompt = at_prompt()) && now() + 8000 < until) {
        if (keys < RETURN_KEYS && (!keys || now() - t0 >= keys * 1500u)) {
            if (press_enter(until)) break;
            keys++;
        }
        task_sleep_ms(20);
    }
    if (prompt && !type_text("EXIT", until))
        while (!(exited = vm_gone()) && now() + 3000 < until) task_sleep_ms(20);
    uint64_t exit_ms = now() - t0;
    struct process *p = client ? proc_find(client) : 0;
    if (!exited && vm_live() && p && p->state != PROC_ZOMBIE) {
        /* The bounded fallback the f3 probes use for a stuck window. */
        forced = true;
        proc_stop(p, 0, 0);
        uint64_t stop = now();
        while (vm_live() && now() - stop < 2000) task_sleep_ms(10);
    }
    uint64_t settle = now();
    struct dosvm_ledger after = {0};
    int left = 0;
    do {
        dosvm_client_poll(); dosvm_snapshot(&after);
        if (after.vms == baseline.vms && !(left = overlay_left())) break;
        task_sleep_ms(20);
    } while (now() - settle < 5000 && now() + 200 < until);
    selftest_figures(r, "prompt=%u keys=%u exited=%u forced=%u exit_ms=%llu vms_before=%u vms_after=%u pages_before=%u pages_after=%u overlay_left=%d",
                     prompt, keys, exited, forced, (unsigned long long)exit_ms, baseline_taken ? baseline.vms : 0,
                     after.vms, baseline.pages, after.pages, left);
    selftest_device_running();
    vm = 0;
    if (exited && after.vms == baseline.vms && !left) selftest_set(r, SELFTEST_PASS, 0);
    else fail(r, forced ? "exit_forced" : !prompt ? "no_prompt" : after.vms != baseline.vms ? "vm_not_released" :
                 "overlay_left");
}
