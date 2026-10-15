/* F3 video row (directive f3-02): per-VM virtual VGA and its presenter.
 * VMVIDEO.COM draws six deterministic stages (text 80x25/80x50, mode 13h,
 * Mode X, mode 12h, text restored); the controller hashes the complete
 * 640x480 presenter frame of each stage at a fixed blink/cursor clock. The
 * suite compares the digests with goldens computed by tests/host/dosvm_test.c
 * from the same patterns. Guest text never supplies the verdict.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include <ciuki/dosvm.h>
#include <ciuki/task.h>
#include <ciuki/storage.h>
#include <ciuki/sha256.h>
#include <ciuki/sync.h>
#include <ciuki/process.h>
#include <ciuki/desktop.h>
#include <ciuki/supervisor.h>

#define VIDEO_PROBE "vm-video"
#define VIDEO_STAGES 5u       /* framed stages; stage 6 restores text and exits */

static bool video_prompt(struct dosvm *v)
{
    const uint16_t *text = dosvm_text(v, 0);
    for (unsigned row = 0; row < 25; row++) {
        bool drive = false;
        for (unsigned col = 0; col < 80; col++) {
            unsigned i = row * 80 + col;
            if (col < 78 && (uint8_t)text[i] == 'C' && (uint8_t)text[i + 1] == ':' &&
                (uint8_t)text[i + 2] == '\\') drive = true;
            if (drive && (uint8_t)text[i] == '>') return true;
        }
    }
    return false;
}
static int video_wait(struct dosvm *v, uint32_t ms, bool (*done)(struct dosvm *, void *), void *arg)
{
    uint64_t until = deadline_after_ms(ms);
    for (;;) {
        struct dosvm_info info;
        dosvm_info(v, &info);
        if (info.state >= DOSVM_STOPPING) return -V86_EIO;
        if (done(v, arg)) return 0;
        if (deadline_passed(until)) return -V86_ETIMEDOUT;
        task_sleep_ms(1);
    }
}
static bool ready_prompt(struct dosvm *v, void *arg)
{
    (void)arg;
    struct dosvm_info info;
    dosvm_info(v, &info);
    return info.dos_ready && video_prompt(v);
}
static bool stage_reached(struct dosvm *v, void *arg)
{
    struct dosvm_info info;
    dosvm_info(v, &info);
    return info.video_stage >= *(const uint32_t *)arg;
}
static bool blocked_for_key(struct dosvm *v, void *arg)
{
    (void)arg;
    struct dosvm_info info;
    dosvm_info(v, &info);
    return info.state == DOSVM_BLOCKED;
}
static int type_line(struct dosvm *v, const char *text)
{
    struct dosvm_key key = dosvm_key(v);
    for (unsigned i = 0; text[i]; i++)
        if (!dosvm_keyboard(key, (uint8_t)text[i])) return -ENOSPC;
    return dosvm_keyboard(key, 0x1C0D) ? 0 : -ENOSPC;
}
static bool ledger_same(const struct dosvm_ledger *a, const struct dosvm_ledger *b)
{
    return a->vms == b->vms && a->tasks == b->tasks && a->pages == b->pages &&
           a->handles == b->handles && a->devices == b->devices &&
           a->reserved == b->reserved && a->resident == b->resident;
}
/* Lead diagnostic: the f3-01 device view when a stage does not arrive
 * (a lost keyboard IRQ or a masked timer shows up here first). */
static void video_devices(const char *probe, struct dosvm *v)
{
    struct dosvm_device_info d = {0};
    if (!dosvm_device_info(dosvm_key(v), &d)) return;
    rec_emit(probe, "DATA", "group=vm_devices irr0=%02x isr0=%02x imr0=%02x queued=%u held=%u irq0=%u irq1=%u last_vector=%u base0=%02x base1=%02x",
             d.irr[0], d.isr[0], d.imr[0], d.queued_bytes, d.held_keys, d.delivered[0], d.delivered[1],
             d.last_vector, d.pic_base[0], d.pic_base[1]);
}

static int probe_f3_vm_video(void)
{
    rec_emit(VIDEO_PROBE, "BEGIN", "version=1");
    struct dosvm *v = 0;
    int error = 0;
    unsigned framed = 0;
    struct dosvm_ledger before, after;
    dosvm_snapshot(&before);
    const struct dosvm_config config = {.commitment = DOSVM_LOW_CAP, .probe = VIDEO_PROBE};
    if ((error = storage_enable_write(storage_get(), 2))) goto cleanup;
    if ((error = dosvm_create(&config, &v)) || (error = dosvm_boot(v)) ||
        (error = video_wait(v, 30000, ready_prompt, 0))) goto cleanup;
    struct dosvm_info info;
    dosvm_info(v, &info);
    rec_emit(VIDEO_PROBE, "DATA", "case=boot ready=%u prompt=1 free_paras=%u", info.dos_ready, info.free_paragraphs);
    /* The DOS prompt itself, through the BIOS teletype and the 8x16 ROM font. */
    struct dosvm_frame frame;
    if ((error = dosvm_frame_digest(v, 0, &frame))) goto cleanup;
    char hex[65];
    sha256_hex(frame.digest, hex);
    rec_emit(VIDEO_PROBE, "DATA", "case=prompt mode=%u width=%u height=%u digest=%s",
             frame.mode, frame.width, frame.height, hex);
    if ((error = type_line(v, "\\VMVIDEO.COM"))) goto cleanup;
    for (uint32_t stage = 1; stage <= VIDEO_STAGES; stage++) {
        if ((error = video_wait(v, 60000, stage_reached, &stage))) break;
        /* The stage is drawn before its line; INT 16h then blocks the VM. */
        if ((error = video_wait(v, 5000, blocked_for_key, 0))) break;
        dosvm_info(v, &info);
        if ((error = dosvm_frame_digest(v, 0, &frame))) break;
        sha256_hex(frame.digest, hex);
        rec_emit(VIDEO_PROBE, "DATA", "case=frame stage=%u mode=%u width=%u height=%u scale=%u text=%u flags=%u digest=%s",
                 stage, frame.mode, frame.width, frame.height, frame.scale, frame.text, info.video_flags, hex);
        framed++;
        if (!dosvm_keyboard(dosvm_key(v), 0x1C0D)) { error = -ENOSPC; break; }
    }
    if (!error) {
        uint32_t last = VIDEO_STAGES + 1;
        if ((error = video_wait(v, 30000, stage_reached, &last)) ||
            (error = video_wait(v, 30000, ready_prompt, 0))) goto cleanup;
        dosvm_info(v, &info);
        rec_emit(VIDEO_PROBE, "DATA", "case=return stage=%u prompt=1 records=%u dropped=%u",
                 info.video_stage, info.guest_records, info.dropped_records);
        if ((error = type_line(v, "EXIT"))) goto cleanup;
        uint64_t until = deadline_after_ms(5000);
        for (;;) {
            dosvm_info(v, &info);
            if (info.state >= DOSVM_STOPPING) break;
            if (deadline_passed(until)) { error = -V86_ETIMEDOUT; goto cleanup; }
            task_sleep_ms(1);
        }
        if (info.reason != DOSVM_EXIT) { error = -EFAULT; goto cleanup; }
        error = dosvm_destroy(dosvm_key(v), false);
        v = 0;
    }
cleanup:
    if (error && v) {
        dosvm_info(v, &info);
        rec_emit(VIDEO_PROBE, "DATA", "group=boot_state ready=%u state=%u reason=%u stage=%u",
                 info.dos_ready, info.state, info.reason, info.video_stage);
        video_devices(VIDEO_PROBE, v);
        probe_dos_screen(VIDEO_PROBE, v);
    }
    if (v) { int e = dosvm_destroy(dosvm_key(v), true); if (!error) error = e; }
    dosvm_snapshot(&after);
    bool cleaned = ledger_same(&before, &after);
    if (!cleaned && !error) error = -ENOMEM;
    rec_emit(VIDEO_PROBE, "DATA", "case=cleanup frames=%u vms=%u tasks=%u pages=%u handles=%u devices=%u restored=%u",
             framed, after.vms, after.tasks, after.pages, after.handles, after.devices, cleaned);
    rec_emit(VIDEO_PROBE, "END", "status=%s error=%d", error ? "FAIL" : "PASS", error);
    return !!error;
}
CIUKI_F3_PROBE("vm-video", probe_f3_vm_video);

/* F3 desktop row (directive f3-02 item 4): the desktop launches /Programs/dosbox
 * through its menu path (--launch=dosbox); the window VM boots to the prompt;
 * the runner types \VMVIDEO.COM through QMP (desktop -> dosbox -> syscall 72
 * -> virtual 8042 -> IRQ1 -> guest INT 9 -> BIOS buffer); stage 1 is held for
 * the runner's window_screen screendump of the composited window; the
 * controller then finishes the stages and types EXIT: the VM stops,
 * /Programs/dosbox closes its window and exits, and every VM and desktop object
 * returns to its baseline.
 * Stimulus: a runner (fw_cfg) case declares the typed keys, so the
 * controller never types for it: it waits WINDOW_INPUT_MS (31 QMP batches
 * take about 10 s under TCG) and fails with the device diagnostics when the
 * keys do not arrive. Only a BOOT.CFG sweep (probes_operator_mode, no runner)
 * falls back to kernel typing after WINDOW_OPERATOR_MS and says typed=kernel. */
#define WINDOW_PROBE "vm-window"
#define WINDOW_HOLD_MS 6000u
#define WINDOW_INPUT_MS 120000u
#define WINDOW_OPERATOR_MS 30000u
extern bool probes_operator_mode;
static bool process_live(uint32_t pid)
{
    struct process *p = proc_find(pid);
    return p && p->state == PROC_LIVE;
}
static void process_remove(struct process *parent, uint32_t pid)
{
    struct process *p = proc_find(pid);
    if (!p) return;
    if (p->state != PROC_ZOMBIE) proc_stop(p, 0, 0);
    uint64_t until = deadline_after_ms(2000);
    while ((p = proc_find(pid)) && p->state != PROC_ZOMBIE && !deadline_passed(until)) { proc_collect(); task_sleep_ms(1); }
    if (p && p->state == PROC_ZOMBIE && parent && p->ppid == parent->pid) proc_reap(parent, p);
}
static bool objects_same(const struct desktop_ledger *a, const struct desktop_ledger *b)
{
    return a->descriptions == b->descriptions && a->surfaces == b->surfaces && a->pages == b->pages &&
           a->channels == b->channels && a->messages == b->messages && a->grants == b->grants;
}
static int finish_stages(struct dosvm *v, uint32_t from)
{
    int error = 0;
    for (uint32_t stage = from; stage <= VIDEO_STAGES && !error; stage++) {
        if ((error = video_wait(v, 60000, stage_reached, &stage)) ||
            (error = video_wait(v, 5000, blocked_for_key, 0))) break;
        if (!dosvm_keyboard(dosvm_key(v), 0x1C0D)) error = -ENOSPC;
    }
    return error;
}
static int probe_f3_vm_window(void)
{
    rec_emit(WINDOW_PROBE, "BEGIN", "version=1");
    int error = 0;
    struct process *owner = 0, *desktop = 0;
    struct dosvm *v = 0;
    uint32_t client = 0, desktop_pid = 0;
    bool closed = false;
    struct dosvm_info info;
    struct dosvm_ledger before, after;
    struct desktop_ledger objects_before, objects_after;
    dosvm_snapshot(&before);
    desktop_snapshot(&objects_before);
    if ((error = storage_enable_write(storage_get(), 2))) goto cleanup;
    if ((error = proc_prepare(proc_supervisor(), &owner))) { owner = 0; goto cleanup; }
    proc_publish(owner, 0);
    const char *argv[] = {"desktop", "--launch=dosbox", 0};
    int made = supervisor_spawn("/Programs/desktop", argv, 0, true, &desktop);
    if (made < 0) { error = made; desktop = 0; goto cleanup; }
    desktop->ppid = owner->pid;
    desktop_pid = desktop->pid;
    uint64_t until = deadline_after_ms(20000);
    while (!(v = dosvm_client_first(&client))) {
        if (!process_live(desktop_pid)) { error = -ESRCH; goto cleanup; }
        if (deadline_passed(until)) { error = -V86_ETIMEDOUT; goto cleanup; }
        task_sleep_ms(1);
    }
    if ((error = video_wait(v, 30000, ready_prompt, 0))) goto cleanup;
    dosvm_info(v, &info);
    rec_emit(WINDOW_PROBE, "DATA", "case=window vm=%u generation=%u client=%u desktop=%u ready=%u prompt=1",
             info.key.id, info.key.generation, client, desktop_pid, info.dos_ready);
    rec_emit(WINDOW_PROBE, "ARM", "action=window_input vm=%u", info.key.id);
    uint32_t first = 1;
    const char *typed = "window";
    error = video_wait(v, probes_operator_mode ? WINDOW_OPERATOR_MS : WINDOW_INPUT_MS, stage_reached, &first);
    if (error == -V86_ETIMEDOUT && probes_operator_mode) {
        typed = "kernel";
        if ((error = type_line(v, "\\VMVIDEO.COM"))) goto cleanup;
        error = video_wait(v, 60000, stage_reached, &first);
    }
    if (error || (error = video_wait(v, 5000, blocked_for_key, 0))) goto cleanup;
    /* The sample is the stage-1 text frame (mode 3, 640x400, flags 3): the
     * typed line ends with Enter, so no key may have advanced VMVIDEO yet. */
    struct dosvm_frame frame;
    if ((error = dosvm_frame_digest(v, 0, &frame))) goto cleanup;
    char hex[65];
    sha256_hex(frame.digest, hex);
    dosvm_info(v, &info);
    uint32_t sampled = info.video_stage;
    rec_emit(WINDOW_PROBE, "DATA", "case=frame stage=%u typed=%s mode=%u width=%u height=%u scale=%u text=%u flags=%u digest=%s",
             sampled, typed, frame.mode, frame.width, frame.height, frame.scale, frame.text, info.video_flags, hex);
    if (sampled != 1) { error = -EFAULT; goto cleanup; }
    rec_emit(WINDOW_PROBE, "ARM", "action=window_screen digest=%s", hex);
    /* Stage 1 stays in place while the runner composites and captures it. */
    until = deadline_after_ms(WINDOW_HOLD_MS);
    while (!deadline_passed(until)) {
        dosvm_info(v, &info);
        if (info.state >= DOSVM_STOPPING || !process_live(client)) { error = -V86_EIO; goto cleanup; }
        if (info.video_stage != 1) { error = -EFAULT; goto cleanup; }
        task_sleep_ms(10);
    }
    if (!dosvm_keyboard(dosvm_key(v), 0x1C0D)) { error = -ENOSPC; goto cleanup; }
    uint32_t last = VIDEO_STAGES + 1;
    if ((error = finish_stages(v, 2)) || (error = video_wait(v, 30000, stage_reached, &last)) ||
        (error = video_wait(v, 30000, ready_prompt, 0)) || (error = type_line(v, "EXIT"))) goto cleanup;
    /* VM exit closes the window: dosbox sees STOPPED, closes the VM and exits. */
    struct dosvm_key key = dosvm_key(v);
    until = deadline_after_ms(10000);
    while (dosvm_client_first(0) || process_live(client)) {
        if (deadline_passed(until)) { error = -V86_ETIMEDOUT; goto cleanup; }
        task_sleep_ms(1);
    }
    dosvm_info(v, &info); /* the released slot keeps its final record */
    closed = true;
    rec_emit(WINDOW_PROBE, "DATA", "case=closed reason=%u dead=%u window_released=1 client_live=0",
             info.reason, !dosvm_valid(key) && info.state == DOSVM_DEAD);
    if (info.reason != DOSVM_EXIT) error = -EFAULT;
    v = 0;
cleanup:
    if (error && v && dosvm_client_first(0) == v) {
        dosvm_info(v, &info);
        rec_emit(WINDOW_PROBE, "DATA", "group=boot_state ready=%u state=%u reason=%u stage=%u",
                 info.dos_ready, info.state, info.reason, info.video_stage);
        video_devices(WINDOW_PROBE, v);
        probe_dos_screen(WINDOW_PROBE, v);
    }
    if (client) process_remove(0, client);
    if (desktop_pid) process_remove(owner, desktop_pid);
    if (owner) { proc_stop(owner, 0, 0); proc_collect(); }
    /* Owner death reaches the supervisor's collection pass; wait for it. */
    until = deadline_after_ms(10000);
    do {
        dosvm_client_poll();
        dosvm_snapshot(&after);
        desktop_snapshot(&objects_after);
        if (ledger_same(&before, &after) && objects_same(&objects_before, &objects_after)) break;
        task_sleep_ms(10);
    } while (!deadline_passed(until));
    bool restored = ledger_same(&before, &after), objects = objects_same(&objects_before, &objects_after);
    if ((!restored || !objects) && !error) error = -ENOMEM;
    if (!closed && !error) error = -EFAULT;
    rec_emit(WINDOW_PROBE, "DATA", "case=cleanup vms=%u tasks=%u pages=%u handles=%u restored=%u objects=%u surfaces=%u grants=%u channels=%u",
             after.vms, after.tasks, after.pages, after.handles, restored, objects,
             objects_after.surfaces, objects_after.grants, objects_after.channels);
    rec_emit(WINDOW_PROBE, "END", "status=%s error=%d", error ? "FAIL" : "PASS", error);
    return !!error;
}
CIUKI_F3_PROBE("vm-window", probe_f3_vm_window);

/* f3-20: exercise the real mode/lease transaction, then a paused second
 * round trip for byte-identical complete screendumps (no masked regions).
 * Internal state is used only to park the guest for that observation. */
#include "../vm/dosvm_internal.h"
#include <ciuki/display.h>
#define FS_PROBE "vm-fullscreen"
static int fullscreen_type(struct dosvm *v,const char *text)
{
    for (unsigned i=0;text[i];i++) {
        uint64_t until=deadline_after_ms(10000);
        while (!dosvm_keyboard(dosvm_key(v),(uint8_t)text[i])) {
            if (!dosvm_valid(dosvm_key(v)) || deadline_passed(until)) return -EIO;
            task_sleep_ms(10);
        }
        task_sleep_ms(10);
    }
    return dosvm_keyboard(dosvm_key(v),0x1C0D) ? 0 : -EIO;
}
static int fullscreen_wait(struct dosvm *v,bool active)
{
    uint64_t until=deadline_after_ms(10000);
    while (dosvm_fullscreen_active(dosvm_key(v))!=active) {
        if (!dosvm_valid(dosvm_key(v)) || deadline_passed(until)) return -EIO;
        task_sleep_ms(10);
    }
    return 0;
}
static int probe_f3_vm_fullscreen(void)
{
    rec_emit(FS_PROBE,"BEGIN","version=1");
    int error=0; struct process *desktop=0;
    struct dosvm *v=0; uint32_t client=0,desktop_pid=0;
    struct dosvm_ledger before,after; dosvm_snapshot(&before);
    if ((error=storage_enable_write(storage_get(),2))) goto cleanup_fs;
    const char *argv[]={"desktop","--test=vm-fullscreen","--launch=dosbox",0};
    int made=supervisor_spawn("/Programs/desktop",argv,0,true,&desktop);
    if (made<0) { error=made; goto cleanup_fs; }
    desktop_pid=desktop->pid;
    uint64_t until=deadline_after_ms(30000);
    while (!(v=dosvm_client_first(&client))) {
        if (!process_live(desktop_pid) || deadline_passed(until)) { error=-EIO; goto cleanup_fs; }
        task_sleep_ms(10);
    }
    v->probe=FS_PROBE;
    if ((error=video_wait(v,30000,ready_prompt,0))) goto cleanup_fs;
    if ((error=fullscreen_type(v,"CD \\DOOM"))) goto cleanup_fs;
    task_sleep_ms(1000);
    if ((error=fullscreen_type(v,"DOOM.EXE -config CIUKI.CFG -nosound -demo demo1"))) goto cleanup_fs;
    uint64_t launch_started=g_ticks;
    until=deadline_after_ms(300000);
    struct dosvm_video_info video;
    do {
        if (!dosvm_valid(dosvm_key(v)) || deadline_passed(until)) { error=-EIO; goto cleanup_fs; }
        dosvm_video_info(dosvm_key(v),&video); task_sleep_ms(10);
    } while (video.mode!=0x13 || !video.unchained);
    rec_emit(FS_PROBE,"DATA","case=launch ms=%llu",g_ticks-launch_started);
    rec_emit(FS_PROBE,"ARM","action=fullscreen_enter");
    if ((error=fullscreen_wait(v,true))) goto cleanup_fs;
    uint64_t started=g_ticks;
    task_sleep_ms(1000);
    rec_emit(FS_PROBE,"ARM","action=fullscreen_demo_start");
    task_sleep_ms(4000);
    rec_emit(FS_PROBE,"ARM","action=fullscreen_demo_end");
    while (g_ticks-started<10000) {
        if (!dosvm_fullscreen_active(dosvm_key(v))) { error=-EIO; goto cleanup_fs; }
        task_sleep_ms(10);
    }
    rec_emit(FS_PROBE,"DATA","case=demo active=1 ms=%u mode=%u",(uint32_t)(g_ticks-started),video.mode);
    rec_emit(FS_PROBE,"ARM","action=fullscreen_leave");
    if ((error=fullscreen_wait(v,false))) goto cleanup_fs;
    /* Pause at the existing worker checkpoint. The first switch ran DOOM;
     * only this equality observation is frozen, with a fixed desktop clock. */
    v->fullscreen_suspended=true; dosvm_idle_wake(v);
    until=deadline_after_ms(3000);
    while (!v->fullscreen_parked) {
        if (deadline_passed(until)) { error=-EBUSY; goto cleanup_fs; }
        task_sleep_ms(1);
    }
    task_sleep_ms(2000);
    rec_emit(FS_PROBE,"ARM","action=fullscreen_before");
    task_sleep_ms(4000);
    v->fullscreen_pause=true;
    if ((error=dosvm_fullscreen_enter(dosvm_key(v)))) goto cleanup_fs;
    task_sleep_ms(500);
    if ((error=dosvm_fullscreen_leave(dosvm_key(v),true))) goto cleanup_fs;
    task_sleep_ms(2000);
    rec_emit(FS_PROBE,"ARM","action=fullscreen_after");
    task_sleep_ms(4000);
    rec_emit(FS_PROBE,"DATA","case=restore active=%u display=%u width=%u height=%u",
        dosvm_fullscreen_active(dosvm_key(v)),fbdev_get()->present,fbdev_get()->width,fbdev_get()->height);
cleanup_fs:
    if (v && dosvm_fullscreen_active(dosvm_key(v))) dosvm_fullscreen_leave(dosvm_key(v),false);
    if (client) process_remove(0,client);
    if (desktop_pid) process_remove(proc_supervisor(),desktop_pid);
    until=deadline_after_ms(10000);
    do {
        dosvm_client_poll(); dosvm_snapshot(&after);
        if (ledger_same(&before,&after)) break;
        task_sleep_ms(10);
    } while (!deadline_passed(until));
    bool restored=ledger_same(&before,&after);
    if (!restored && !error) error=-ENOMEM;
    rec_emit(FS_PROBE,"DATA","case=cleanup restored=%u vms=%u tasks=%u pages=%u handles=%u",
        restored,after.vms,after.tasks,after.pages,after.handles);
    rec_emit(FS_PROBE,"END","status=%s error=%d",error ? "FAIL" : "PASS",error);
    return !!error;
}
CIUKI_F3_PROBE(FS_PROBE,probe_f3_vm_fullscreen);
