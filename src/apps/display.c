/* Display properties: firmware modes, EDID identity and active CiukiOS driver.
 * The monitor and adapter are separate objects. Report only detected facts.
 * Protocol/design: docs/validation/2026-10-03-display-settings.md. */
#include "app.h"
#include "display_probe.h"

static struct disp_probe probe;
static struct dialog notice;
static u8 native[192];
static u16 vm_seg, vm_off;
static int native_ok, page, selected, top, focus, pending_probe = 1;
static int X, Y, W, H;
static unsigned last_poll, preview_tick;
static u16 previous_mode, preview_mode;
static int preview;
static char status[128];
static const char *tabs[4] = { "Screen", "Adapter", "Monitor", "Advanced" };

static u32 nfield(int off) { return *(u32 *)(native + off); }
static void native_read(void)
{
    struct regs r;
    native_ok = 0;
    if (!vm_seg) {
        mem_set(&r, 0, sizeof r);
        r.ax = 0x1684; r.bx = 0x4349;
        intr(0x2F, &r); vm_seg = r.es; vm_off = r.di;
    }
    if (!vm_seg) return;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x0114; r.cx = sizeof native; r.di = (u16)native;
    r.ds = r.es = app_seg();
    if (far_regs(vm_seg, vm_off, &r)) return;
    if (*(u32 *)native != 0x44475643UL || *(u16 *)(native + 4) != 0x0100 ||
        *(u16 *)(native + 6) != sizeof native) return;
    native_ok = 1;
    if (nfield(52) == 128 && disp_probe_parse_edid(native + 64, 128, &probe.monitor))
        probe.flags |= DISP_F_EDID;
}
static const char *adapter_name(void)
{
    if (!(probe.flags & DISP_F_PCI_MATCH)) return "VGA-compatible display adapter";
    switch (probe.adapter_kind) {
    case DISP_ADAPTER_QEMU: return "QEMU Standard VGA (Bochs VBE)";
    case DISP_ADAPTER_VIRTIO: return "QEMU VirtIO GPU";
    case DISP_ADAPTER_ATI: return "ATI / AMD display adapter";
    case DISP_ADAPTER_NVIDIA: return "NVIDIA display adapter";
    case DISP_ADAPTER_INTEL: return "Intel display adapter";
    }
    return "PCI display adapter";
}
static const char *driver_name(void)
{
    if (native_ok && nfield(8) == 1) return "CiukiOS VirtIO GPU 2D";
    if (native_ok && nfield(8) == 2) {
        if (probe.adapter_kind == DISP_ADAPTER_ATI) return "CiukiOS ATI base display";
        if (probe.adapter_kind == DISP_ADAPTER_NVIDIA) return "CiukiOS NVIDIA base display";
        return "CiukiOS native base display";
    }
    return probe.flags & DISP_F_VBE ? "CiukiOS VBE framebuffer" : "CiukiOS VGA compatibility";
}
static const char *driver_state(void)
{
    if (!native_ok || !nfield(8)) return "Firmware display path";
    if (nfield(12) == 1) return "Active";
    if (nfield(12) == 2) return "Initializing";
    if (nfield(12) == 3 || nfield(32)) return "Driver reported an error";
    return "Inactive";
}
static int virtual_display(void)
{
    return probe.adapter_kind == DISP_ADAPTER_QEMU || probe.adapter_kind == DISP_ADAPTER_VIRTIO;
}
static void geometry(char *out, u16 width, u16 height, u16 depth)
{
    char t[12];
    fmt_u32(out, width); str_cat(out, " x "); fmt_u32(t, height); str_cat(out, t);
    if (depth) { str_cat(out, "   "); fmt_u32(t, depth); str_cat(out, t); str_cat(out, "-bit"); }
}
static void detect(void)
{
    int i, j;
    struct disp_mode temp;
    disp_probe_init(&probe);
    native_read();
    /* Stable geometry/depth order; mode number breaks duplicate ties. */
    for (i = 1; i < probe.mode_count; i++) {
        temp = probe.modes[i]; j = i;
        while (j > 0) {
            struct disp_mode *p = &probe.modes[j - 1];
            if (p->width < temp.width || (p->width == temp.width && p->height < temp.height) ||
                (p->width == temp.width && p->height == temp.height && p->bpp <= temp.bpp)) break;
            probe.modes[j] = *p; j--;
        }
        probe.modes[j] = temp;
    }
    selected = 0;
    for (i = 0; i < probe.mode_count; i++)
        if (probe.modes[i].id == probe.current_mode) selected = i;
    top = selected > 6 ? selected - 6 : 0;
    str_copy(status, probe.mode_count ? "Select a mode, then Apply to preview it." : "No supported desktop modes were reported by the video BIOS.");
    app_log("[DISPLAY] adapter", adapter_name());
    app_log("[DISPLAY] driver", driver_name());
    app_log("[DISPLAY] monitor", probe.monitor.valid ? probe.monitor.name : "identification unavailable");
}
static void layout(void)
{
    X = HOST.x + 6; Y = HOST.y + TITLE_H; W = HOST.w - 12; H = HOST.h - TITLE_H - 6;
}
static void line(int row, const char *label, const char *value)
{
    int y = Y + 48 + row * 25;
    ui_text(X + 18, y, label, C_INK);
    draw_frame_text(X + 164, y, W - 182, value, C_INK | BOLD);
}
static void button(int x, int y, int w, const char *text, int id)
{
    ui_button(X + x, Y + y, w, 25, text, id);
}
static void screen_paint(void)
{
    int i, at, y, fg, rows = 7;
    char text[96], t[16];
    const char *name = probe.monitor.valid && probe.monitor.name[0] ? probe.monitor.name : "Generic display";
    ui_icon(X + 18, Y + 47, ICON_DISPLAY);
    draw_frame_text(X + 62, Y + 46, W - 86, name, C_INK | BOLD);
    draw_frame_text(X + 62, Y + 66, W - 86, adapter_name(), C_INK);
    geometry(text, (u16)HOST.screen_w, (u16)HOST.screen_h,
        native_ok && nfield(8) ? (u16)nfield(28) : probe.current_bpp);
    str_copy(t, "Current: ");
    ui_text(X + 18, Y + 95, t, C_INK);
    ui_text(X + 83, Y + 95, text, C_INK | BOLD);
    ui_text(X + 18, Y + 121, "Resolution and colour depth", C_INK);
    ui_inset(X + 18, Y + 143, 302, rows * 21 + 4);
    ui_rect(X + 20, Y + 145, 298, rows * 21, C_PAPER);
    for (i = 0; i < rows; i++) {
        at = top + i;
        if (at >= probe.mode_count) break;
        y = Y + 145 + i * 21; fg = at == selected ? C_PAPER : C_INK;
        if (at == selected) ui_rect(X + 20, y, 280, 21, C_TITLE);
        geometry(text, probe.modes[at].width, probe.modes[at].height, probe.modes[at].bpp);
        ui_text(X + 27, y + 2, text, fg);
        ui_hit(X + 20, y, 280, 21, 20 + i);
    }
    if (probe.mode_count > rows) draw_scroll(X + 300, Y + 145, rows * 21, top, probe.mode_count, rows);
    ui_bevel(X + 349, Y + 141, W - 384, 105, C_SHADOW);
    ui_rect(X + 356, Y + 148, W - 398, 88, C_TEAL);
    ui_text(X + 423, Y + 180, "1", C_PAPER | BOLD);
    ui_rect(X + 423, Y + 246, 28, 9, C_SHADOW);
    ui_bevel(X + 397, Y + 255, 80, 7, C_FACE);
    draw_frame_text(X + 337, Y + 276, W - 354, virtual_display() ? "Virtual display" : "Primary display", C_INK);
    if (probe.mode_count) {
        geometry(text, probe.modes[selected].width, probe.modes[selected].height, 0);
        draw_frame_text(X + 337, Y + 296, W - 354, text, C_INK | BOLD);
    }
    if (focus == 1) draw_focus(X + 16, Y + 141, 306, rows * 21 + 8);
}
static void adapter_paint(void)
{
    char t[96], n[16];
    line(0, "Adapter", adapter_name());
    str_copy(t, "Unavailable");
    if (probe.flags & DISP_F_PCI_MATCH) {
        fmt_hex4(t, probe.pci_vendor); str_cat(t, ":"); fmt_hex4(n, probe.pci_device); str_cat(t, n);
        str_cat(t, "   Bus "); fmt_u32(n, probe.pci_bus); str_cat(t, n);
        str_cat(t, "  Device "); fmt_u32(n, probe.pci_devfn >> 3); str_cat(t, n);
        str_cat(t, "  Function "); fmt_u32(n, probe.pci_devfn & 7); str_cat(t, n);
    }
    line(1, "Hardware ID", t);
    line(2, "Active driver", driver_name());
    line(3, "Status", driver_state());
    if (probe.video_memory_64k) { fmt_u32(t, (u32)probe.video_memory_64k * 64UL); str_cat(t, " KB (video BIOS)"); }
    else str_copy(t, "Not reported");
    line(4, "Video memory", t);
    str_copy(t, "Not available");
    if (probe.flags & DISP_F_VBE) {
        fmt_u32(t, probe.vbe_version >> 8); str_cat(t, "."); fmt_u32(n, probe.vbe_version & 255); str_cat(t, n);
    }
    line(5, "VBE interface", t);
    line(6, "Connection", probe.flags & DISP_F_PCI_MATCH ? "Matched to active framebuffer" : "Firmware primary display");
    button(18, 244, 158, "Device Manager...", 12);
    button(189, 244, 152, "Driver details", 3);
}
static void monitor_paint(void)
{
    struct disp_monitor *m = &probe.monitor;
    char t[96], n[20];
    line(0, "Display", m->valid && m->name[0] ? m->name : "Generic display");
    line(1, "Identification", m->valid ? "EDID verified" : "Not supplied by firmware / driver");
    line(2, "Connection", virtual_display() ? "Virtual display" : !m->valid ? "Unknown" : m->input_digital ? "Digital (EDID)" : "Analog RGB (EDID)");
    line(3, "Manufacturer", m->valid ? m->manufacturer : "Unknown");
    if (m->valid) { fmt_hex4(t, m->product); str_cat(t, "   Serial "); fmt_u32(n, m->serial); str_cat(t, n); }
    else str_copy(t, "Unknown");
    line(4, "Product", t);
    if (m->valid && m->width_cm && m->height_cm) {
        fmt_u32(t, m->width_cm); str_cat(t, " x "); fmt_u32(n, m->height_cm); str_cat(t, n); str_cat(t, " cm");
    } else str_copy(t, "Not reported");
    line(5, "Image size", t);
    str_copy(t, "Not reported");
    if (m->valid && m->preferred_width && m->preferred_height) {
        geometry(t, m->preferred_width, m->preferred_height, 0);
        if (m->preferred_millihz) {
            str_cat(t, "  "); fmt_u32(n, m->preferred_millihz / 1000UL); str_cat(t, n);
            str_cat(t, "."); fmt_2(n, (u16)((m->preferred_millihz % 1000UL) / 10UL)); str_cat(t, n); str_cat(t, " Hz");
        }
    }
    line(6, "Preferred timing", t);
    line(7, "Active refresh", virtual_display() ? "Managed by the host display" : "Default video BIOS timing");
    draw_frame_text(X + 18, Y + 272, W - 36,
        virtual_display() ? "This is the virtual screen exposed by QEMU." : "Preferred timing comes from EDID; active timing may differ.", C_INK);
}
static void advanced_paint(void)
{
    char t[96], n[16];
    line(0, "Driver", driver_name());
    line(1, "Provider / version", "CiukiOS / 0.8.0");
    line(2, "Component", native_ok && nfield(8) ? "C:\\VM\\CVSESS.DLL" : "Built-in VGA / VBE framebuffer");
    line(3, "Presentation", native_ok && nfield(8) == 1 ? "Native texture transfer and presentation" : native_ok && nfield(8) == 2 ? "Native scanout; CPU rendering" : "Firmware scanout; CPU rendering");
    line(4, "3D interface", "Not implemented by this display driver");
    line(5, "Refresh control", virtual_display() ? "Host compositor / QEMU" : "Video BIOS default; custom timing unavailable");
    if (native_ok) {
        fmt_u32(t, nfield(48) / 1024UL); str_cat(t, " KB   Error "); fmt_u32(n, nfield(32)); str_cat(t, n);
    } else str_copy(t, "Resident display interface unavailable");
    line(6, "Framebuffer", t);
    str_copy(t, "Not applicable");
    if (native_ok && nfield(8) == 1) {
        fmt_u32(t, nfield(40)); str_cat(t, " / "); fmt_u32(n, nfield(36)); str_cat(t, n); str_cat(t, " commands completed / submitted");
    }
    line(7, "GPU status", t);
    button(18, 264, 162, "Driver manager...", 13);
    button(193, 264, 150, "Detect displays", 10);
}
static void paint(void)
{
    int i;
    layout();
    ui_rect(X, Y, W, H, C_FACE);
    for (i = 0; i < 4; i++) {
        ui_bevel(X + 8 + i * 116, Y + 5 + (i == page ? 0 : 3), 116, i == page ? 27 : 24, C_FACE);
        ui_text(X + 20 + i * 116, Y + 10, tabs[i], C_INK | (i == page ? BOLD : 0));
        ui_hit(X + 8 + i * 116, Y + 5, 116, 27, i);
    }
    ui_rect(X + 8, Y + 31, W - 16, 1, C_PAPER);
    if (page == 0) screen_paint();
    else if (page == 1) adapter_paint();
    else if (page == 2) monitor_paint();
    else advanced_paint();
    draw_frame_text(X + 18, Y + H - 62, W - 36,
        page == 0 ? status : page == 1 ? "The active driver is selected for the detected hardware." :
        page == 2 ? "Press F5 to detect the display again." :
        "Built-in video drivers are updated with the system image.", C_INK);
    if (page == 0) {
        button(18, H - 36, 129, "Detect displays", 10);
        button(158, H - 36, 141, "Advanced...", 3);
        button(W - 222, H - 36, 98, "Apply...", 11);
    }
    button(W - 111, H - 36, 93, "Close", 14);
    if (focus >= 2) draw_focus(X + (focus == 2 ? W - 224 : W - 113), Y + H - 38, focus == 2 ? 102 : 97, 29);
}
static int profile_writable(const char *path)
{
    int attr = dos_get_attr(path);
    return attr == -2 || (attr >= 0 && !(attr & 0x11));
}
static int save_mode(u16 mode)
{
    const char *cfg = "\\SYSTEM\\VIDEO\\DISPLAY.CFG";
    const char *tmp = "\\SYSTEM\\VIDEO\\DISPLAY.NEW";
    const char *bak = "\\SYSTEM\\VIDEO\\DISPLAY.BAK";
    char text[6];
    int h, had, ok;
    if (!profile_writable(cfg) || !profile_writable(tmp) || !profile_writable(bak)) return 0;
    fmt_hex4(text + 1, mode); text[1] = 'M';
    h = dos_create(tmp);
    if (h < 0) return 0;
    ok = dos_write(h, text + 1, 4) == 4;
    if (dos_close(h) < 0) ok = 0;
    if (!ok) { dos_delete(tmp); return 0; }
    if (dos_get_attr(bak) >= 0 && dos_delete(bak) < 0) return 0;
    had = dos_get_attr(cfg) >= 0;
    if (had && dos_rename(cfg, bak) < 0) return 0;
    if (dos_rename(tmp, cfg) < 0) {
        if (had) dos_rename(bak, cfg);
        return 0;
    }
    return 1;
}
static void finish_preview(int keep)
{
    if (!preview) return;
    notice.open = 0;
    dialog_sync(&notice);
    if (keep && save_mode(preview_mode)) {
        str_copy(status, "Display mode saved.");
        app_log("[DISPLAY] kept", "graphics");
    } else {
        if (app_display_mode(previous_mode) < 0) {
            str_copy(status, "Could not restore the previous mode. Open Display again.");
            app_log("[DISPLAY] restore failed", "graphics");
            preview = 0; pending_probe = 1;
            ui_repaint_win(WIN_DISPLAY);
            return;
        }
        str_copy(status, keep ? "Profile could not be saved. Previous mode restored." : "Previous display mode restored.");
        app_log("[DISPLAY] reverted", "graphics");
    }
    preview = 0; pending_probe = 1;
    ui_repaint_win(WIN_DISPLAY);
}
static void apply(void)
{
    if (!probe.mode_count) return;
    native_read();
    if (vm_seg && !native_ok) {
        msgbox(&notice, "Display settings", "The session manager did not report its state.\nDisplay settings could not be changed safely.", "OK");
        return;
    }
    if (native_ok && nfield(56)) {
        msgbox(&notice, "Display settings", "Close DOS windows before changing the display mode.\nThe current game will keep running.", "OK");
        app_log("[DISPLAY] blocked", "DOS windows open");
        return;
    }
    if (probe.modes[selected].id == probe.current_mode) {
        str_copy(status, "The selected display mode is already active."); return;
    }
    previous_mode = probe.current_mode;
    preview_mode = probe.modes[selected].id;
    if (app_display_mode(preview_mode) < 0) {
        str_copy(status, "The display change could not be queued."); return;
    }
    preview = 1; preview_tick = HOST.ticks;
    str_copy(status, "Applying display mode...");
    app_log("[DISPLAY] preview", "graphics");
}
static int action(int id)
{
    if (id >= 0 && id < 4) { page = id; focus = 0; return 1; }
    if (id >= 20 && id < 27 && top + id - 20 < probe.mode_count) {
        selected = top + id - 20; focus = 1; return 1;
    }
    if (id == 10) { pending_probe = 1; return 1; }
    if (id == 11) { apply(); return 1; }
    if (id == 12) app_open(WIN_DEVICES, "");
    if (id == 13) app_open(WIN_DEVICES, "drivers");
    if (id == 14) app_close();
    return 1;
}
int app_event(int ev, int a, int b, int c)
{
    int r, orig = ev, scan, ch, sx, sy, had_monitor, old_backend, old_phase;
    if (ev == EV_OPEN) {
        str_copy(app_title, "Display Properties");
        if (!HDR_WIDTH) { HDR_WIDTH = 606; HDR_HEIGHT = 414; }
        if (!str_icmp(APP_ARG, "advanced")) page = 3;
        else if (!str_icmp(APP_ARG, "monitor")) page = 2;
        APP_ARG[0] = 0;
        pending_probe = 1;
        app_log("[DISPLAY] open", "Display Properties");
        return 1;
    }
    if (ev == EV_POLL && preview) {
        if ((unsigned)(HOST.ticks - preview_tick) >= 219) {
            finish_preview(0); return 1;
        }
        if (preview == 1 && app_display_mode(0) != 0) {
            if (app_display_mode(0) != preview_mode) { finish_preview(0); return 1; }
            preview = 2;
            msgbox(&notice, "Keep display settings?", "Keep this display mode?\nThe previous mode returns automatically in 12 seconds.", "Yes|No");
            dialog_sync(&notice); return 1;
        }
        return 0;
    }
    if (dialog_mine(&notice)) {
        if (ev == EV_PAINT) { dialog_draw(&notice); return 0; }
        if (ev == EV_CLOSE) {
            notice.win = 0; notice.open = 0;
            finish_preview(0); return 0;
        }
        r = -1;
        if (ev == EV_KEY) r = dialog_key(&notice, a, HOST.shift);
        if (ev == EV_MOUSE) {
            sx = HOST.x + b; sy = HOST.y + TITLE_H + c;
            if (a == MOUSE_HOVER) {
                ui_dirty = 0; dialog_hover(&notice, sx, sy); return ui_dirty;
            }
            r = dialog_mouse(&notice, a, sx, sy);
        }
        if (preview && r >= 0) finish_preview(r == MB_YES);
        dialog_sync(&notice);
        return 1;
    }
    r = dialog_pre(&notice, WIN_DISPLAY, &ev, &a);
    if (r >= 0) return r;
    if (notice.open && (ev == EV_KEY || ev == EV_ACTION ||
        (ev == EV_MOUSE && a == MOUSE_DOWN))) {
        app_window_cmd(notice.win, 1); return 1;
    }
    switch (ev) {
    case EV_PAINT: paint(); return 0;
    case EV_ACTION: r = action(a); break;
    case EV_KEY:
        scan = KEY_SCAN(a); ch = KEY_CHAR(a); r = 1;
        if (ch == 27) { app_close(); break; }
        if (ch >= '1' && ch <= '4') { page = ch - '1'; focus = 0; break; }
        if (ch == 9) { focus = (focus + 1) % 4; break; }
        if (scan == K_F5) { pending_probe = 1; break; }
        if (ch == 13) { if (focus == 3) app_close(); else if (page == 0) apply(); break; }
        if (!ch || ch == 0xE0) {
            if (scan == K_LEFT) { page = (page + 3) % 4; break; }
            if (scan == K_RIGHT) { page = (page + 1) % 4; break; }
            if (page == 0 && probe.mode_count) {
                if (scan == K_UP && selected > 0) selected--;
                if (scan == K_DOWN && selected + 1 < probe.mode_count) selected++;
                if (scan == K_HOME) selected = 0;
                if (scan == K_END) selected = probe.mode_count - 1;
                if (scan == K_PGUP) selected = selected > 7 ? selected - 7 : 0;
                if (scan == K_PGDN) selected = selected + 7 < probe.mode_count ? selected + 7 : probe.mode_count - 1;
                if (selected < top) top = selected;
                if (selected >= top + 7) top = selected - 6;
                focus = 1;
            }
        }
        break;
    case EV_WHEEL:
        if (page != 0 || !probe.mode_count || notice.open) return 0;
        top += a * 3;
        if (top > probe.mode_count - 7) top = probe.mode_count - 7;
        if (top < 0) top = 0;
        return 1;
    case EV_MOUSE:
        r = 0; layout();
        sx = HOST.x + b; sy = HOST.y + TITLE_H + c;
        if (page == 0 && a == MOUSE_DOWN && sx >= X + 300 && sx < X + 316 && sy >= Y + 145 && sy < Y + 292 && probe.mode_count > 7) {
            long hit = scroll_hit(X + 300, Y + 145, 147, sy, probe.mode_count, 7);
            if (hit == -1 && top > 0) top--;
            else if (hit == -2 && top + 7 < probe.mode_count) top++;
            else if (hit >= 16) top = (int)(hit - 16);
            r = 1;
        }
        break;
    case EV_POLL:
        if (pending_probe) { pending_probe = 0; detect(); r = 1; break; }
        if ((unsigned)(HOST.ticks - last_poll) >= 18) {
            had_monitor = probe.monitor.valid;
            old_backend = native_ok ? (int)nfield(8) : 0;
            old_phase = native_ok ? (int)nfield(12) : 0;
            last_poll = HOST.ticks; native_read();
            r = page == 3 || had_monitor != probe.monitor.valid ||
                old_backend != (native_ok ? (int)nfield(8) : 0) ||
                old_phase != (native_ok ? (int)nfield(12) : 0);
            if (!had_monitor && probe.monitor.valid)
                app_log("[DISPLAY] monitor", probe.monitor.name);
            break;
        }
        r = 0; break;
    case EV_SUSPEND: return 1;
    case EV_CLOSE: finish_preview(0); return 0;
    default: r = 0; break;
    }
    r = dialog_post(&notice, WIN_DISPLAY, ev, r);
    return orig == EV_CLOSE && ev != EV_CLOSE ? 0 : r;
}
