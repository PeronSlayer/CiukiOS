/* A DOS window is a forked VM. The desktop module stays in VM 0 and paints
 * the guest's virtual VGA through CVSESSION's bounded video-band presenter.
 * Its event loop never runs from a guest DOS or timer callback. */
#include "app.h"

#define VM_COUNT 4
#define WIN_COUNT 3
#define VM_FREE 0
#define VM_READY 1
#define SNAP_MAGIC 0x53564443UL /* CDVS */
#define SNAP_VERSION 0x0100
#define SNAP_BYTES 448
#define SNAP_VIDEO 1
#define SNAP_DEVICES 2
#define SNAP_TIMEOUT 1
#define SNAP_EXIT 2
#define SNAP_CLOSE 3
#define SNAP_ABI_ERROR 0xFFFE
#define SNAP_UNAVAILABLE 0xFFFF
#define SNAP_INTERVAL 19        /* at least one second at the BIOS tick rate */
#define SNAP_DEADLINE 219       /* approximately twelve seconds */

static const char fork_path[] = "\\VM\\VMFORK.COM";
static char tail[128], pending[96];
static char raw_path[SYS_PATH], short_path[SYS_PATH];
static u8 fcb[16];
#pragma pack(push, 1)
struct exec_params {
    u16 env, tail_off, tail_seg, fcb1_off, fcb1_seg, fcb2_off, fcb2_seg;
};
struct band_info {
    u16 seg, top, bottom, stride, width;
    u8 bytes, red_size, red_pos, green_size, green_pos, blue_size, blue_pos;
    u16 left;
};
struct dosvm_snapshot {
    u32 magic;
    u16 version, bytes, vm, generation, host_ticks, valid_mask;
    u16 reason, state, exit_code, video_error, device_error;
    u16 sample_ticks, video_ticks, device_ticks;
    char title[32];
    u8 video[256], devices[128];
};
#pragma pack(pop)
typedef char snapshot_size_check[sizeof(struct dosvm_snapshot) == SNAP_BYTES ? 1 : -1];
static struct exec_params params;
static u8 frame[256];
static u8 snapshot_video[256], snapshot_devices[128];
struct guest_window {
    u8 win, vm, live, finished, presented, present_error, windows_exe;
    u16 generation, exit_code;
    u16 client_x, client_y, client_w, client_h;
    u16 snapshot_started, snapshot_polled;
    u8 snapshot_sampled, snapshot_saved;
    char title[32];
    struct dosvm_snapshot snapshot;
};
static struct guest_window guest[WIN_COUNT];
static u16 vmm_seg, vmm_off;
static int pending_open, pending_slot = -1, pending_windows_exe;
static int active_before = -1, buttons_before;
static int mouse_x, mouse_y;
static unsigned last_damage_tick;
static char log_line[220];
static int presenter_log_pending;

/* Foreground transitions only: persist evidence on machines without COM1. */
static void disk_log(const char *event, const char *detail, u16 code)
{
    char number[8];
    int h = dos_open("\\SYSTEM\\DOSVM.LOG", 2);
    long end;
    if (h < 0) h = dos_create("\\SYSTEM\\DOSVM.LOG");
    if (h < 0) return;
    end = dos_seek(h, 0, 2);
    if (end < 0) { dos_close(h); return; }
    if (end >= 4096L) {
        dos_close(h); h = dos_create("\\SYSTEM\\DOSVM.LOG");
        if (h < 0) return;
    }
    str_ncopy(log_line, event, 40);
    str_cat(log_line, " ");
    if (detail) str_ncopy(log_line + str_len(log_line), detail, 150);
    str_cat(log_line, " code="); fmt_hex4(number, code); str_cat(log_line, number);
    str_cat(log_line, "\r\n");
    if (end + str_len(log_line) > 4096L) {
        dos_close(h); h = dos_create("\\SYSTEM\\DOSVM.LOG");
        if (h < 0) return;
    }
    dos_write(h, log_line, str_len(log_line));
    dos_close(h);
}

static void vm_find(void)
{
    struct regs r;
    if (vmm_seg) return;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x1684; r.bx = 0x4349;
    intr(0x2F, &r);
    vmm_seg = r.es; vmm_off = r.di;
}
static int vm_call(struct regs *r)
{
    vm_find();
    if (!vmm_seg) return -1;
    r->ds = r->es = app_seg();
    return far_regs(vmm_seg, vmm_off, r);
}
static int vm_list(u8 *list)
{
    struct regs r;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x48; r.di = (u16)list;
    return vm_call(&r);
}
static int vm_target(int vm)
{
    struct regs r;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x47; r.bx = vm;
    return vm_call(&r);
}
static void vm_focus(int vm)
{
    struct regs r;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x44; r.bx = vm; r.cx = 1; /* mouse stays with the desktop */
    vm_call(&r);
}
static void vm_kill(int vm)
{
    struct regs r;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x45; r.bx = vm;
    vm_call(&r);
}
static int vm_status(int vm, u16 *code, u16 *generation)
{
    struct regs r;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x4A; r.bx = vm;
    *code = 0xFFFF; *generation = 0;
    if (vm_call(&r)) return VM_FREE;
    *code = r.dx; *generation = r.cx;
    return r.bx;
}
/* Samples use fresh state packets, not the separately refreshed shared live[]
 * header. Keep the last validated packet on failure, with its original tick. */
static u16 snapshot_query(struct guest_window *g, u16 op, u8 *data,
                          u16 bytes, u32 magic)
{
    struct regs r;
    mem_set(data, 0, bytes);
    if (vm_target(g->vm)) return SNAP_UNAVAILABLE;
    mem_set(&r, 0, sizeof r);
    r.ax = op; r.cx = bytes; r.di = (u16)data;
    if (vm_call(&r)) return r.ax ? r.ax : SNAP_UNAVAILABLE;
    if (*(u32 *)data != magic || *(u16 *)(data + 4) != SNAP_VERSION ||
        *(u16 *)(data + 6) != bytes) return SNAP_ABI_ERROR;
    return 0;
}
static void snapshot_begin(struct guest_window *g)
{
    struct dosvm_snapshot *s = &g->snapshot;
    mem_set(s, 0, sizeof *s);
    s->magic = SNAP_MAGIC; s->version = SNAP_VERSION; s->bytes = SNAP_BYTES;
    s->vm = g->vm; s->generation = g->generation;
    s->exit_code = SNAP_UNAVAILABLE;
    s->video_error = s->device_error = SNAP_UNAVAILABLE;
    str_ncopy(s->title, g->title, sizeof s->title);
    g->snapshot_started = g->snapshot_polled = HOST.ticks;
    g->snapshot_sampled = g->snapshot_saved = 0;
}
static void snapshot_sample(struct guest_window *g)
{
    struct dosvm_snapshot *s = &g->snapshot;
    u16 code, generation, video_error, device_error;
    if (!g->live || (g->snapshot_sampled &&
        (u16)(HOST.ticks - g->snapshot_polled) < SNAP_INTERVAL)) return;
    g->snapshot_sampled = 1; g->snapshot_polled = HOST.ticks;
    s->sample_ticks = HOST.ticks;
    s->video_error = s->device_error = SNAP_UNAVAILABLE;
    if (vm_status(g->vm, &code, &generation) != VM_READY ||
        generation != g->generation) return;
    video_error = snapshot_query(g, 0x21, snapshot_video, 256, 0x53565643UL);
    device_error = snapshot_query(g, 0x33, snapshot_devices, 128, 0x56445643UL);
    /* A freed target falls back to the caller's session. Neither teardown
     * nor slot reuse may replace this window's last validated packets. */
    if (vm_status(g->vm, &code, &generation) != VM_READY ||
        generation != g->generation) return;
    s->video_error = video_error; s->device_error = device_error;
    if (!video_error) {
        mem_copy(s->video, snapshot_video, sizeof s->video);
        s->valid_mask |= SNAP_VIDEO; s->video_ticks = HOST.ticks;
    }
    if (!device_error) {
        mem_copy(s->devices, snapshot_devices, sizeof s->devices);
        s->valid_mask |= SNAP_DEVICES; s->device_ticks = HOST.ticks;
    }
}
/* Called only by foreground poll/close handling, never by the painter or an
 * IRQ callback. One fixed file per VM; the live timeout is attempted once. */
static void snapshot_save(struct guest_window *g, u16 reason, u16 state, u16 code)
{
    char path[] = "\\SYSTEM\\DOSVM1.BIN";
    struct dosvm_snapshot *s = &g->snapshot;
    int h, written;
    if (g->vm < 1 || g->vm >= VM_COUNT || s->magic != SNAP_MAGIC) return;
    path[13] = '0' + g->vm;
    s->host_ticks = HOST.ticks; s->reason = reason;
    s->state = state; s->exit_code = code;
    h = dos_create(path);
    if (h < 0) { disk_log("Snapshot failed", path, SNAP_UNAVAILABLE); return; }
    written = dos_write(h, s, sizeof *s);
    dos_close(h);
    if (written != sizeof *s) disk_log("Snapshot failed", path, SNAP_UNAVAILABLE);
}
static void snapshot_close(struct guest_window *g)
{
    u16 code, generation;
    int state;
    snapshot_sample(g);
    state = vm_status(g->vm, &code, &generation);
    if (generation != g->generation) code = SNAP_UNAVAILABLE;
    snapshot_save(g, SNAP_CLOSE, (u16)state, code);
}
static struct guest_window *by_window(int win)
{
    int i;
    if (!win) return 0;
    for (i = 0; i < WIN_COUNT; i++) if (guest[i].win == win) return &guest[i];
    return 0;
}
/* A PE/NE file is a Windows program, even though it starts with an MZ DOS
 * stub. Passing it to COMMAND.COM can leave a guest in the DOS stub forever.
 * The Win32 runtime is still under construction; report this before forking. */
static int is_windows_exe(const char *command)
{
    char path[SYS_PATH];
    u8 header[64], signature[4];
    u32 offset;
    int h, i = 0, result = 0;
    if (!(*command == '\\' || (command[0] && command[1] == ':'))) return 0;
    while (command[i] && command[i] != ' ' && command[i] != '\t' &&
           i < SYS_PATH - 1) { path[i] = command[i]; i++; }
    path[i] = 0;
    h = dos_open(path, 0);
    if (h < 0) return 0;
    if (dos_read(h, header, sizeof header) != sizeof header ||
        header[0] != 'M' || header[1] != 'Z') goto done;
    offset = (u32)header[60] | ((u32)header[61] << 8) |
             ((u32)header[62] << 16) | ((u32)header[63] << 24);
    if (offset < 64 || offset > 65535UL || dos_seek(h, offset, 0) != offset ||
        dos_read(h, signature, sizeof signature) != sizeof signature) goto done;
    if ((signature[0] == 'P' && signature[1] == 'E' &&
         signature[2] == 0 && signature[3] == 0) ||
        (signature[0] == 'N' && signature[1] == 'E')) result = 1;
done:
    dos_close(h);
    return result;
}
static int spawn(const char *command)
{
    u8 before[VM_COUNT * 4], after[VM_COUNT * 4];
    struct regs r;
    int i, k = 0;
    const char *prefix = " \\VM\\DPMIRUN.COM /V";
    const char *args = 0;
    /* COMMAND.COM uses classic DOS EXEC. Convert a long pathname through the
     * resident LFN service before handing it to the forked VM. This also
     * makes programs inside C:\DESKTOP\TestGames launchable by double-click. */
    if (*command == '\\' || (command[0] && command[1] == ':')) {
        i = 0;
        while (command[i] && command[i] != ' ' && command[i] != '\t' && i < SYS_PATH - 1) {
            raw_path[i] = command[i]; i++;
        }
        raw_path[i] = 0;
        args = command + i;
        if (dos_short_path(raw_path, short_path) == 0 && short_path[0]) {
            command = short_path;
        } else args = 0;
    }
    if (vm_list(before)) { disk_log("VM list failed", 0, 0); return -1; }
    while (*prefix && k < 125) tail[1 + k++] = *prefix++;
    if (*command) {
        const char *extra = " /C ";
        while (*extra && k < 125) tail[1 + k++] = *extra++;
        while (*command && k < 125) tail[1 + k++] = *command++;
        if (*command) return -1;
        if (args) {
            while (*args && k < 125) tail[1 + k++] = *args++;
            if (*args) return -1;
        }
    }
    tail[0] = k; tail[1 + k] = 13;
    tail[2 + k] = 0;
    disk_log("EXEC", tail + 1, 0);
    params.env = 0;
    params.tail_off = (u16)tail; params.tail_seg = app_seg();
    params.fcb1_off = params.fcb2_off = (u16)fcb;
    params.fcb1_seg = params.fcb2_seg = app_seg();
    mem_set(&r, 0, sizeof r);
    r.ax = 0x4B00; r.dx = (u16)fork_path; r.bx = (u16)&params;
    r.ds = r.es = app_seg();
    if (intr(0x21, &r)) { disk_log("EXEC failed", fork_path, r.ax); return -1; }
    if (vm_list(after)) return -1;
    for (i = 1; i < VM_COUNT; i++)
        if (before[i * 4] == VM_FREE && after[i * 4] == VM_READY) return i;
    return -1;
}
static void begin_window(int slot, int window)
{
    int vm;
    struct guest_window *g = &guest[slot];
    g->win = window;
    g->finished = 0; g->presented = 0; g->present_error = 0;
    g->windows_exe = 0;
    g->exit_code = 0;
    if (pending_windows_exe) {
        g->live = 0; g->finished = 1; g->windows_exe = 1;
        pending_windows_exe = 0;
        pending[0] = 0;
        app_log("[DOSVM] Windows executable", g->title);
        ui_repaint_win(window);
        return;
    }
    vm = spawn(pending);
    pending[0] = 0;
    if (vm < 0) {
        g->live = 0; g->finished = 1;
        g->exit_code = 0xFFFF;
        app_log("[DOSVM] fork failed", 0);
        disk_log("Fork failed", g->title, 0);
        app_sound(5);
    } else {
        g->vm = vm; g->live = 1;
        vm_status(vm, &g->exit_code, &g->generation);
        snapshot_begin(g);
        app_log("[DOSVM] fork", g->title);
        disk_log("VM ready", g->title, g->generation);
    }
    ui_repaint_win(window);
}

static int band_present(struct guest_window *g, int x, int y, int w, int h)
{
    struct band_info info;
    struct regs r;
    mem_set(&info, 0, sizeof info);
    if (!g || !g->live) return 0;
    if (!app_band_info(&info)) return 0;
    if (!info.seg || info.bottom <= info.top || info.bytes < 2) return 0;
    if (vm_target(g->vm)) return 0;
    mem_set(frame, 0, sizeof frame);
    *(u32 *)(frame + 0) = 0x50565643UL; /* CVVP */
    *(u16 *)(frame + 4) = 0x0100;
    *(u16 *)(frame + 6) = 256;
    *(u16 *)(frame + 8) = info.seg;
    *(u16 *)(frame + 10) = info.top;
    *(u32 *)(frame + 12) = (u32)info.stride * (info.bottom - info.top);
    *(u32 *)(frame + 16) = info.stride;
    *(u16 *)(frame + 20) = info.width;
    *(u16 *)(frame + 22) = info.bottom - info.top;
    frame[24] = info.bytes;
    frame[25] = info.red_size; frame[26] = info.red_pos;
    frame[27] = info.green_size; frame[28] = info.green_pos;
    frame[29] = info.blue_size; frame[30] = info.blue_pos;
    *(u16 *)(frame + 32) = x; *(u16 *)(frame + 34) = y;
    *(u16 *)(frame + 36) = x + w; *(u16 *)(frame + 38) = y + h;
    *(u32 *)(frame + 172) = 8;       /* CVP_NO_DAMAGE: only this compositor band */
    *(u16 *)(frame + 232) = info.left;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x25; r.di = (u16)frame;
    if (vm_call(&r)) {
        if (r.ax != 4 && !g->present_error) {
            char number[8];
            fmt_hex4(number, r.ax);
            app_log("[DOSVM] video error", number);
            g->present_error = 1;
            presenter_log_pending = r.ax;
        }
        return 0;
    }
    if (!g->presented) app_log("[DOSVM] video ready", g->title);
    g->presented = 1;
    return 1;
}
static void paint(void)
{
    struct guest_window *g = by_window(HOST.window);
    int x = HOST.x + 3, y = HOST.y + TITLE_H;
    int w = HOST.w - 6, h = HOST.h - TITLE_H - 3;
    if (w < 4 || h < 4) return;
    if (g) {
        g->client_x = x; g->client_y = y;
        g->client_w = w; g->client_h = h;
    }
    if (g && band_present(g, x, y, w, h)) return;
    ui_rect(x, y, w, h, C_INK);
    if (g && g->windows_exe) {
        ui_text(x + 14, y + 14, "Windows executable detected.", C_PAPER);
        ui_text(x + 14, y + 38, "Win32 application support is under development.", C_PAPER);
    } else if (g && g->finished) {
        char number[8], message[40];
        ui_text(x + 14, y + 14, g->exit_code == 0xFFFF ?
                "Unable to run the DOS session." : "Program finished.", C_PAPER);
        str_copy(message, "Session return code: ");
        fmt_hex4(number, g->exit_code); str_cat(message, number);
        ui_text(x + 14, y + 38, message, C_PAPER);
        ui_text(x + 14, y + 62, "Press Alt+F4 to close this window.", C_PAPER);
    }
}
static void feed_mouse(struct guest_window *g, int x, int y, int buttons)
{
    struct regs r;
    if (!g || !g->live) return;
    if (vm_target(g->vm)) return;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x37;
    r.bx = x - mouse_x;
    r.cx = mouse_y - y;                /* PS/2 Y is positive upward */
    r.dx = buttons & 7;
    vm_call(&r);
    mouse_x = x; mouse_y = y;
}
static int poll(void)
{
    u8 windows[32 * 25];
    int active = -1, i, changed = 0;
    struct regs r;
    if (presenter_log_pending) {
        disk_log("Presenter failed", 0, (u16)presenter_log_pending);
        presenter_log_pending = 0;
    }
    if (pending_open) {
        int slot = pending_slot, win = WIN_DOS;
        pending_open = 0; pending_slot = -1;
        if (slot > 0) {
            win = win_open(-1, -1, HDR_WIDTH, HDR_HEIGHT, guest[slot].title, 0);
            if (!win) { app_sound(5); return 0; }
        }
        begin_window(slot, win);
        changed = 1;
    }
    if (app_windows((char *)windows) > 0)
        for (i = 0; i < 32; i++) if (windows[i * 25] & 0x80) { active = i; break; }
    if (app_desktop_focus()) active = -1;
    if (active != active_before || (HOST.buttons && !buttons_before)) {
        struct guest_window *g = by_window(active);
        if (active != active_before) {
            char number[8];
            fmt_u32(number, active < 0 ? 99 : active);
            app_log("[DOSVM] active", number);
        }
        vm_focus(g && g->live ? g->vm : 0);
        active_before = active;
    }
    buttons_before = HOST.buttons;
    for (i = 0; i < WIN_COUNT; i++) if (guest[i].live) {
        u16 code, generation;
        int state = vm_status(guest[i].vm, &code, &generation);
        if (state != VM_READY ||
            generation != guest[i].generation) {
            disk_log("VM state", guest[i].title, (u16)state);
            disk_log("VM generation", guest[i].title, generation);
            if (generation != guest[i].generation) code = 0xFFFF;
            snapshot_save(&guest[i], SNAP_EXIT, (u16)state, code);
            guest[i].live = 0; guest[i].finished = 1; guest[i].exit_code = code;
            app_log("[DOSVM] ended", guest[i].title);
            disk_log("VM exited", guest[i].title, code);
            ui_repaint_win(guest[i].win);
            changed = 1;
        } else {
            snapshot_sample(&guest[i]);
            if (!guest[i].snapshot_saved &&
                (u16)(HOST.ticks - guest[i].snapshot_started) >= SNAP_DEADLINE) {
                guest[i].snapshot_saved = 1;
                snapshot_save(&guest[i], SNAP_TIMEOUT, (u16)state, code);
            }
        }
    }
    /* Every desktop wake-up: the VM manager wakes the idle desktop every
     * 8 ms while a DOS VM runs, so a game's frames are presented as they
     * come rather than at a 2-tick (9 Hz) cadence. */
    {
        last_damage_tick = HOST.ticks;
        for (i = 0; i < WIN_COUNT; i++) if (guest[i].live) {
            u32 mask;
            int first, last, band_top, band_bottom;
            if (!guest[i].presented) {
                ui_repaint_win(guest[i].win);
                changed = 1;
                continue;
            }
            if (!guest[i].client_w || !guest[i].client_h) continue;
            if (vm_target(guest[i].vm)) continue;
            mem_set(&r, 0, sizeof r);
            r.ax = 0x2E; r.cx = 64; r.dx = guest[i].client_h;
            if (vm_call(&r)) continue;
            mask = ((u32)r.dxh << 16) | r.dx;
            if (!mask) continue;
            first = 0;
            while (!(mask & (1UL << first))) ++first;
            last = 31;
            while (!(mask & (1UL << last))) --last;
            band_top = first * 64;
            band_bottom = (last + 1) * 64;
            if (band_bottom > guest[i].client_h) band_bottom = guest[i].client_h;
            ui_damage(guest[i].client_x, guest[i].client_y + band_top,
                      guest[i].client_w, band_bottom - band_top);
            changed = 1;
        }
    }
    return changed ? 3 : 0; /* repaint the damage already queued, without expanding it */
}
int app_event(int ev, int a, int b, int c)
{
    struct guest_window *g = by_window(HOST.window);
    int i;
    switch (ev) {
    case EV_OPEN:
        if (a == 1 || a == 2) return 1;
        str_copy(app_title, "DOS Window");
        HDR_WIDTH = 680; HDR_HEIGHT = 460;
        for (i = 0; i < WIN_COUNT; i++) if (!guest[i].win) break;
        if (i >= WIN_COUNT) { app_sound(5); return 0; }
        str_copy(guest[i].title, "DOS Window");
        if (APP_ARG[0]) str_ncopy(guest[i].title, APP_ARG, sizeof guest[i].title);
        pending_windows_exe = is_windows_exe(APP_ARG);
        str_ncopy(pending, APP_ARG, sizeof pending);
        pending_slot = i; pending_open = 1;
        app_log("[DOSVM] open", guest[i].title);
        disk_log("Requested", APP_ARG, 0);
        return 1;
    case EV_PAINT: paint(); return 0;
    case EV_POLL: return poll();
    case EV_WHEEL:
        if (g && g->live && HOST.active && c >= 0 && !vm_target(g->vm)) {
            struct regs r;
            mem_set(&r, 0, sizeof r);
            r.ax = 0x37; r.dx = HOST.buttons & 7; r.si = a;
            vm_call(&r);
        }
        return 0;
    case EV_MOUSE:
        if (a == MOUSE_DOWN && g && g->live) vm_focus(g->vm);
        if (g && g->live && (a == MOUSE_MOVE || a == MOUSE_HOVER ||
            a == MOUSE_DOWN || a == MOUSE_UP || a == MOUSE_RIGHT)) {
            int x = HOST.x + b, y = HOST.y + TITLE_H + c;
            /* Only the focused guest owns input inside its actual client.
             * A title-bar move can otherwise dismiss DOS screen savers. */
            if ((active_before == g->win || a == MOUSE_DOWN) &&
                ((x >= g->client_x && y >= g->client_y &&
                  x < g->client_x + g->client_w &&
                  y < g->client_y + g->client_h) || a == MOUSE_UP))
                feed_mouse(g, x, y, HOST.buttons);
        }
        return 0;
    case EV_CLOSE:
        if (HOST.window == WIN_DOS) {
            if (guest[0].live) {
                snapshot_close(&guest[0]);
                vm_kill(guest[0].vm);
            }
            mem_set(&guest[0], 0, sizeof guest[0]);
            for (i = 1; i < WIN_COUNT; i++) if (guest[i].win) break;
            vm_focus(0);
            app_log("[DOSVM] closed", 0);
            return i < WIN_COUNT ? 2 : 0; /* keep the dynamic windows alive */
        } else if (g) {
            if (g->live) {
                snapshot_close(g);
                vm_kill(g->vm);
            }
            mem_set(g, 0, sizeof *g);
        }
        vm_focus(0);
        app_log("[DOSVM] closed", 0);
        for (i = 0; i < WIN_COUNT; i++) if (guest[i].win) break;
        return i == WIN_COUNT ? 3 : 0; /* manager had no main window */
    case EV_SUSPEND: return 1;          /* live VMs retain their manager */
    }
    return 0;
}
