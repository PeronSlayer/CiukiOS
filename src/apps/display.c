/* Display properties: firmware modes, EDID identity and active CiukiOS driver.
 * The monitor and adapter are separate objects. Report only detected facts.
 * Protocol/design: docs/validation/2026-10-03-display-settings.md. */
#include "app.h"
#include "display_probe.h"

static struct disp_probe probe;
static struct dialog notice;
static u8 native[192];
static u8 cache_diagnostics[624];
static u8 session_info[64];
static u16 vm_seg, vm_off;
static int native_ok, cache_log_attempted, protected_lfb_ok, page, selected, top, focus, pending_probe = 1;
static int probe_require_banked;
static int X, Y, W, H;
static unsigned last_poll, preview_tick;
static u16 previous_mode, preview_mode;
static int preview;
static char status[128];
static const char *tabs[6] = { "Screen", "Adapter", "Monitor", "Advanced", "Background", "Appearance" };
#define WP_SOLID 0
#define WALL_MAX 99
static struct app_wallpaper_info wall_info;
static char wall_title[WALL_MAX + 1][31];
static int wall_count, wall_selected, wall_top, wall_style, wall_saved_index, wall_saved_style;
static int wall_changed;
static u16 wall_width, wall_height;
static int close_after_preview;
static struct deskcfg theme_cfg, theme_saved;
static u8 theme_palette[48];
static u16 theme_icons;
static int theme_scheme, theme_dirty;
static const char *scheme_names[8] = {
    "CiukiOS Classic", "Ocean", "Rose", "Slate", "Forest", "Desert", "Lilac", "High Contrast"
};
static const u8 scheme_colors[8][8][3] = {
    { {9,10,12}, {13,18,30}, {13,23,24}, {51,52,53}, {28,30,34}, {39,49,48}, {61,61,60}, {27,36,48} },
    { {5,8,14}, {6,22,40}, {8,26,38}, {48,52,56}, {26,31,38}, {36,48,56}, {60,62,63}, {12,30,52} },
    { {14,8,10}, {38,14,24}, {40,24,30}, {54,50,51}, {33,28,30}, {56,44,48}, {63,61,61}, {44,20,34} },
    { {8,9,11}, {22,26,32}, {20,23,28}, {46,47,49}, {26,27,30}, {38,40,43}, {60,60,60}, {30,34,42} },
    { {7,11,8}, {12,28,16}, {14,26,18}, {49,52,47}, {27,31,27}, {40,48,38}, {60,62,58}, {18,36,26} },
    { {14,10,6}, {40,24,10}, {44,34,22}, {56,52,44}, {36,31,24}, {58,50,36}, {62,62,57}, {42,30,16} },
    { {10,8,14}, {28,18,42}, {30,26,40}, {52,50,55}, {30,28,34}, {46,42,54}, {62,61,63}, {34,26,50} },
    { {0,0,0}, {0,0,40}, {0,0,0}, {48,48,48}, {16,16,16}, {56,56,56}, {63,63,63}, {0,0,52} }
};
static const u8 theme_slots[8] = { 0, 1, 3, 7, 8, 11, 15, 9 };
static const u8 theme_swatches[8] = { 3, 9, 2, 12, 8, 13, 6, 1 };
static const char *icon_names[8] = { "Computer", "Programs", "Recycle Bin", "Control Panel", "DOS Prompt", "Floppy", "USB drive", "CD-ROM" };
#define DISPLAY_LOG_PATH "\\SYSTEM\\DISPLAY.LOG"
#define DISPLAY_LOG_LIMIT 4096L
#define CACHE_LOG_PATH "\\SYSTEM\\VIDEO\\CACHE.LOG"

static void cache_log_persist(void)
{
    struct regs r;
    int h;
    if (cache_log_attempted || !vm_seg) return;
    mem_set(cache_diagnostics, 0, sizeof cache_diagnostics);
    mem_set(&r, 0, sizeof r);
    r.ax = 0x0117;                 /* VM_OP_FB_DIAGNOSTICS | NO_SWITCH */
    r.cx = sizeof cache_diagnostics;
    r.di = (u16)cache_diagnostics;
    r.ds = r.es = app_seg();
    if (far_regs(vm_seg, vm_off, &r) || (r.flags & 1)) return;
    if (*(u32 *)(cache_diagnostics + 0) != 0x44465643UL ||
        *(u16 *)(cache_diagnostics + 4) != 0x0100 ||
        *(u16 *)(cache_diagnostics + 6) != sizeof cache_diagnostics ||
        *(u32 *)(cache_diagnostics + 8) != 576 ||
        *(u32 *)(cache_diagnostics + 12) != 32 ||
        mem_cmp(cache_diagnostics + 16, "CVFBCACH", 8) ||
        mem_cmp(cache_diagnostics + 592, "CVFBTIME", 8)) return;
    /* A full or read-only disk must not turn diagnostics into periodic I/O. */
    cache_log_attempted = 1;
    h = dos_create(CACHE_LOG_PATH);
    if (h < 0) return;
    dos_write(h, cache_diagnostics, sizeof cache_diagnostics);
    dos_close(h);
}

static u16 display_msw(void);
#pragma aux display_msw = "smsw ax" value [ax];
static int display_in_v86(void) { return (display_msw() & 1) != 0; }

static u32 nfield(int off) { return *(u32 *)(native + off); }
static int session_query_lfb(void)
{
    struct regs r;
    u32 caps;
    if (!vm_seg) {
        mem_set(&r, 0, sizeof r);
        r.ax = 0x1684; r.bx = 0x4349;
        intr(0x2F, &r); vm_seg = r.es; vm_off = r.di;
    }
    if (!vm_seg) return 0;
    mem_set(session_info, 0, sizeof session_info);
    mem_set(&r, 0, sizeof r);
    r.ax = 0x0100;                 /* VM_OP_QUERY | VM_OP_NO_SWITCH */
    r.cx = sizeof session_info; r.di = (u16)session_info;
    r.ds = r.es = app_seg();
    if (far_regs(vm_seg, vm_off, &r) || (r.flags & 1)) return 0;
    if (*(u32 *)session_info != 0x534D5643UL ||
        *(u16 *)(session_info + 4) < 0x0100 ||
        *(u16 *)(session_info + 6) < sizeof session_info ||
        *(u32 *)(session_info + 12) != 0) return 0;
    caps = *(u32 *)(session_info + 8);
    return (caps & (0x00010000UL | 0x00080000UL)) ==
        (0x00010000UL | 0x00080000UL);
}
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
    cache_log_persist();
}
static void diag_hex4(char **dst, u16 value)
{
    fmt_hex4(*dst, value);
    *dst += 4;
}
static void diag_hex8(char **dst, u32 value)
{
    diag_hex4(dst, (u16)(value >> 16));
    diag_hex4(dst, (u16)value);
}
static void diag_append(const char *record)
{
    const char *path = DISPLAY_LOG_PATH;
    int attr, h, bytes = str_len(record);
    long end;
    char line[192];
    if (!bytes || bytes > 180) return;
    mem_copy(line, record, bytes);
    line[bytes++] = '\r'; line[bytes++] = '\n';
    attr = dos_get_attr(path);
    if (attr == -2) h = dos_create(path);
    else if (attr >= 0 && !(attr & 1)) h = dos_open(path, 2);
    else return;
    if (h < 0) return;
    end = dos_seek(h, 0, 2);
    if (end < 0) { dos_close(h); return; }
    if (end + bytes > DISPLAY_LOG_LIMIT) {
        if (dos_close(h) < 0) return;
        h = dos_create(path);
        if (h < 0) return;
        if (dos_write(h, "RESET\r\n", 7) != 7) { dos_close(h); return; }
    }
    if (dos_write(h, line, bytes) != bytes) { dos_close(h); return; }
    dos_close(h);
}
static int read_active_vbe(u16 *status_ax, u16 *mode_bx, u16 *pitch_bx,
                           u16 *pixels_cx, u16 *lines_dx, u16 *scan_status)
{
    struct regs r;
    mem_set(&r, 0, sizeof r); r.ax = 0x4F03; r.ds = app_seg();
    intr(0x10, &r);
    *status_ax = r.ax; *mode_bx = r.bx;
    mem_set(&r, 0, sizeof r); r.ax = 0x4F06; r.bx = 1; r.ds = app_seg();
    intr(0x10, &r);
    *scan_status = r.ax; *pitch_bx = r.bx; *pixels_cx = r.cx; *lines_dx = r.dx;
    return *status_ax == 0x004F;
}
static void diag_active(const char *event, int app_mode)
{
    char line[192], *p = line;
    u16 ax, bx, pitch, pixels, lines, scan;
    read_active_vbe(&ax, &bx, &pitch, &pixels, &lines, &scan);
    str_copy(p, "ACTIVE event="); p += str_len(p); str_copy(p, event); p += str_len(p);
    str_copy(p, " app="); p += str_len(p); diag_hex4(&p, (u16)app_mode);
    str_copy(p, " 4F03="); p += str_len(p); diag_hex4(&p, ax); *p++ = '/'; diag_hex4(&p, bx);
    str_copy(p, " 4F06="); p += str_len(p); diag_hex4(&p, scan); *p++ = '/'; diag_hex4(&p, pitch);
    *p++ = '/'; diag_hex4(&p, pixels); *p++ = '/'; diag_hex4(&p, lines); *p = 0;
    diag_append(line);
}
static void diag_mode(const char *event, u16 id)
{
    struct disp_mode_diag d;
    char line[192], *p = line, h[5];
    const u8 *masks;
    u16 i, active_ax = 0, active_bx = 0, active_pitch, active_pixels, active_lines, active_scan;
    int use_linear;
    if (!disp_probe_get_mode_diag(id, &d)) return;
    use_linear = !probe_require_banked && (d.attributes & 0x80);
    if (str_cmp(event, "current") == 0 || str_cmp(event, "preview") == 0) {
        read_active_vbe(&active_ax, &active_bx, &active_pitch, &active_pixels,
                        &active_lines, &active_scan);
        if (active_ax == 0x004F && (active_bx & 0x3FFF) == id)
            use_linear = !!(active_bx & 0x4000);
    }
    str_copy(p, "MODE event="); p += str_len(p); str_copy(p, event); p += str_len(p);
    str_copy(p, " id="); p += str_len(p); diag_hex4(&p, id);
    str_copy(p, " attr="); p += str_len(p); diag_hex4(&p, d.attributes);
    str_copy(p, " A="); p += str_len(p); diag_hex4(&p, d.window_a_attributes); *p++ = ':'; diag_hex4(&p, d.window_a_segment);
    str_copy(p, " B="); p += str_len(p); diag_hex4(&p, d.window_b_attributes); *p++ = ':'; diag_hex4(&p, d.window_b_segment);
    str_copy(p, " g/s="); p += str_len(p); diag_hex4(&p, d.granularity_kb); *p++ = '/'; diag_hex4(&p, d.window_kb);
    str_copy(p, " pitch="); p += str_len(p); diag_hex4(&p, d.banked_pitch); *p++ = '/'; diag_hex4(&p, d.linear_pitch);
    str_copy(p, " st/pl="); p += str_len(p); diag_hex4(&p, d.status); *p++ = '/'; diag_hex4(&p, d.planes);
    str_copy(p, " wh="); p += str_len(p); diag_hex4(&p, d.width); *p++ = 'x'; diag_hex4(&p, d.height);
    str_copy(p, " b/m="); p += str_len(p); diag_hex4(&p, d.bpp); *p++ = '/'; diag_hex4(&p, d.memory_model);
    str_copy(p, " path="); p += str_len(p); str_copy(p, use_linear ? "LFB" : "BANK"); p += str_len(p);
    str_copy(p, " phys="); p += str_len(p); diag_hex8(&p, d.framebuffer_phys);
    masks = use_linear ? d.linear_masks : d.bank_masks;
    if (use_linear && !masks[0]) masks = d.bank_masks;
    str_copy(p, " rgb="); p += str_len(p);
    for (i = 0; i < 6; ++i) { h[0] = "0123456789ABCDEF"[(masks[i] >> 4) & 15]; h[1] = "0123456789ABCDEF"[masks[i] & 15]; h[2] = 0; str_copy(p, h); p += 2; }
    *p = 0;
    diag_append(line);
}
static void diag_raw_probes(void)
{
    u16 i;
    struct disp_probe_diag d;
    for (i = 0; i < probe.diag_count; ++i) {
        char line[192], *p = line;
        if (!disp_probe_get_raw_diag(&probe, i, &d)) continue;
        str_copy(p, "PROBE id="); p += str_len(p); diag_hex4(&p, d.id);
        str_copy(p, " st="); p += str_len(p); diag_hex4(&p, d.status);
        str_copy(p, " attr="); p += str_len(p); diag_hex4(&p, d.attributes);
        str_copy(p, " pl="); p += str_len(p); diag_hex4(&p, d.planes);
        str_copy(p, " wh="); p += str_len(p); diag_hex4(&p, d.width); *p++ = 'x'; diag_hex4(&p, d.height);
        str_copy(p, " pitch="); p += str_len(p); diag_hex4(&p, d.banked_pitch); *p++ = '/'; diag_hex4(&p, d.linear_pitch);
        str_copy(p, " b/m="); p += str_len(p); diag_hex4(&p, d.bpp); *p++ = '/'; diag_hex4(&p, d.memory_model);
        str_copy(p, d.accepted ? " ok=1" : " ok=0"); p += str_len(p);
        *p = 0; diag_append(line);
    }
}
static void diag_event(const char *event, u16 id, u16 result)
{
    char line[192], *p = line;
    str_copy(p, "DISPLAY event="); p += str_len(p); str_copy(p, event); p += str_len(p);
    str_copy(p, " id="); p += str_len(p); diag_hex4(&p, id);
    str_copy(p, " result="); p += str_len(p); diag_hex4(&p, result);
    str_copy(p, " current="); p += str_len(p); diag_hex4(&p, probe.current_mode);
    str_copy(p, " access="); p += str_len(p); diag_hex4(&p, probe.current_mode_flags);
    str_copy(p, " native="); p += str_len(p);
    if (native_ok) {
        diag_hex4(&p, (u16)nfield(8)); *p++ = '/'; diag_hex4(&p, (u16)nfield(12));
        *p++ = '/'; diag_hex8(&p, nfield(56));
    } else str_copy(p, "NONE");
    p += str_len(p);
    str_copy(p, " path="); p += str_len(p);
    str_copy(p, probe_require_banked ? "BANK" : (protected_lfb_ok ? "LFB-OR-BANK" : "AUTO"));
    p += str_len(p);
    *p = 0;
    diag_append(line);
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
    case DISP_ADAPTER_S3:
        if (probe.pci_device == 0x8C2E || probe.pci_device == 0x8C2F)
            return "S3 SuperSavage/IXC 16 (IBM ThinkPad T23)";
        if (probe.pci_device == 0x8A25 || probe.pci_device == 0x8A26)
            return "S3 Savage/IX";
        if (probe.pci_device == 0x8A22)
            return "S3 Savage4";
        if (probe.pci_device == 0x8A20 || probe.pci_device == 0x8A21)
            return "S3 Savage3D";
        if ((probe.pci_device & 0xFF00) == 0x8C00 || (probe.pci_device & 0xFF00) == 0x8A00)
            return "S3 Savage series";
        if (probe.pci_device == 0x8811 || probe.pci_device == 0x8812 || probe.pci_device == 0x8814)
            return "S3 Trio32/64";
        if (probe.pci_device == 0x8901 || probe.pci_device == 0x8902)
            return "S3 Trio64V2";
        if (probe.pci_device == 0x8815)
            return "S3 Aurora64V+";
        if (probe.pci_device == 0x883D || probe.pci_device == 0x8A01)
            return "S3 ViRGE series";
        return "S3 Graphics display adapter";
    case DISP_ADAPTER_MATROX: return "Matrox MGA display adapter";
    case DISP_ADAPTER_SIS: return "SiS display adapter";
    case DISP_ADAPTER_3DFX: return "3dfx Voodoo display adapter";
    case DISP_ADAPTER_CIRRUS: return "Cirrus Logic display adapter";
    case DISP_ADAPTER_TRIDENT: return "Trident display adapter";
    case DISP_ADAPTER_NEOMAGIC: return "NeoMagic MagicGraph display adapter";
    }
    return "PCI display adapter";
}
static const char *driver_name(void)
{
    if (native_ok && nfield(8) == 3 && (nfield(60) & 0x80000000UL))
        return "CiukiOS SuperSavage BCI";
    if (native_ok && nfield(8) == 1) return "CiukiOS VirtIO GPU 2D";
    if (native_ok && nfield(8) == 2) {
        if (probe.adapter_kind == DISP_ADAPTER_ATI) return "CiukiOS ATI base display";
        if (probe.adapter_kind == DISP_ADAPTER_NVIDIA) return "CiukiOS NVIDIA base display";
        if (probe.adapter_kind == DISP_ADAPTER_S3) return "CiukiOS S3 Savage display";
        return "CiukiOS native base display";
    }
    if (probe.flags & DISP_F_VBE) {
        if (probe.adapter_kind == DISP_ADAPTER_S3) return "CiukiOS VBE (S3 SuperSavage)";
        return "CiukiOS VBE framebuffer";
    }
    return "CiukiOS VGA compatibility";
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
    int i;
    protected_lfb_ok = display_in_v86() && session_query_lfb();
    probe_require_banked = display_in_v86() && !protected_lfb_ok;
    disp_probe_init(&probe, probe_require_banked);
    disp_probe_sort_modes(&probe);
    native_read();
    diag_raw_probes();
    selected = 0;
    for (i = 0; i < probe.mode_count; i++)
        if (probe.modes[i].id == probe.current_mode) selected = i;
    top = selected > 6 ? selected - 6 : 0;
    str_copy(status, probe.mode_count ? "Select a mode, then Apply to preview it." : "No supported desktop modes were reported by the video BIOS.");
    diag_event("probe", probe.current_mode, probe.flags);
    if (probe.current_mode >= 0x100) diag_mode("current", probe.current_mode);
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
static void wallpaper_dimensions(void);
static void wallpaper_catalog_load(void)
{
    static const char path[] = "\\SYSTEM\\UI\\WALLS.DAT";
    u8 header[8], record[44];
    char fallback[13];
    int h, i, count, listed = 0;
    count = app_wallpaper_count();
    if (count < 0 || count > WALL_MAX) count = 0;
    wall_count = 1;
    str_copy(wall_title[0], "Solid colour");
    mem_set(wall_title + 1, 0, sizeof wall_title - sizeof wall_title[0]);
    h = dos_open(path, 0);
    if (h >= 0) {
        if (dos_read(h, header, sizeof header) == sizeof header &&
            !mem_cmp(header, "CWC1", 4) && !header[5] && !header[6] && !header[7] &&
            header[4] <= WALL_MAX) {
            listed = header[4];
            /* If the service cannot discover a catalog, retain its packaged entries. */
            if (!count) count = listed;
            if (listed > count) listed = count;
            for (i = 0; i < listed; i++) {
                if (dos_read(h, record, sizeof record) != sizeof record ||
                    mem_cmp(record, "WALL", 4)) break;
                mem_copy(wall_title[i + 1], record + 13, 30);
                wall_title[i + 1][30] = 0;
                wall_count++;
            }
        }
        dos_close(h);
    }
    while (wall_count <= count && wall_count <= WALL_MAX) {
        mem_copy(fallback, "WALL", 4);
        fmt_2(fallback + 4, wall_count);
        mem_copy(fallback + 6, ".CWP", 5);
        str_copy(wall_title[wall_count], fallback);
        wall_count++;
    }
}
static void wallpaper_refresh(void)
{
    int selected = wall_selected, style = wall_style;
    wallpaper_catalog_load();
    if (selected >= wall_count) selected = wall_count - 1;
    wall_selected = selected; wall_style = style;
    if (wall_selected < wall_top) wall_top = wall_selected;
    if (wall_selected >= wall_top + 9) wall_top = wall_selected - 8;
    if (wall_top < 0) wall_top = 0;
    if (wall_top > wall_count - 9) wall_top = wall_count > 9 ? wall_count - 9 : 0;
    wall_changed = wall_selected != wall_saved_index || wall_style != wall_saved_style;
    wallpaper_dimensions();
    str_copy(status, "Wallpaper list refreshed.");
}
static void wallpaper_dimensions(void)
{
    char path[32], file[13];
    u8 header[8];
    int h;
    wall_width = wall_height = 0;
    if (!wall_selected) return;
    mem_copy(file, "WALL", 4); fmt_2(file + 4, wall_selected); mem_copy(file + 6, ".CWP", 5);
    str_copy(path, "\\SYSTEM\\UI\\"); str_cat(path, file);
    h = dos_open(path, 0);
    if (h < 0) return;
    if (dos_read(h, header, sizeof header) == sizeof header &&
        (!mem_cmp(header, "CWP1", 4) || !mem_cmp(header, "CWP2", 4))) {
        wall_width = (u16)(header[4] | ((u16)header[5] << 8));
        wall_height = (u16)(header[6] | ((u16)header[7] << 8));
    }
    dos_close(h);
}
static void wallpaper_geometry(char *out)
{
    u32 dw = (u32)HOST.screen_w, dh = HOST.screen_h > 61 ? (u32)HOST.screen_h - 61 : (u32)HOST.screen_h;
    u32 iw = wall_width, ih = wall_height, rw, rh, x;
    char n[16];
    if (!iw || !ih) {
        str_copy(out, "Solid colour across the desktop."); return;
    }
    str_copy(out, "Image: "); fmt_u32(n, iw); str_cat(out, n); str_cat(out, " x "); fmt_u32(n, ih); str_cat(out, n); str_cat(out, "; ");
    if (wall_style == WP_STRETCH) {
        fmt_u32(n, dw); str_cat(out, n); str_cat(out, " x "); fmt_u32(n, dh); str_cat(out, n); str_cat(out, " (stretched to fit).");
    } else if (wall_style == WP_CENTER) {
        if (iw > dw) { x = (iw - dw) / 2; str_cat(out, "centered; crops "); fmt_u32(n, x); str_cat(out, n); str_cat(out, " px each side."); }
        else if (ih > dh) { x = (ih - dh) / 2; str_cat(out, "centered; crops "); fmt_u32(n, x); str_cat(out, n); str_cat(out, " px top and bottom."); }
        else str_cat(out, "centered at original size.");
    } else if (wall_style == WP_TILE) {
        str_cat(out, "tiles "); fmt_u32(n, (dw + iw - 1) / iw); str_cat(out, n); str_cat(out, " x ");
        fmt_u32(n, (dh + ih - 1) / ih); str_cat(out, n); str_cat(out, " from top left.");
    } else if (wall_style == WP_FIT) {
        if (iw * dh > ih * dw) { rw = dw; rh = ih * dw / iw; }
        else { rh = dh; rw = iw * dh / ih; }
        str_cat(out, "fits "); fmt_u32(n, rw); str_cat(out, n); str_cat(out, " x "); fmt_u32(n, rh); str_cat(out, n); str_cat(out, "; keeps aspect ratio.");
    } else {
        if (iw * dh > ih * dw) { rh = dh; rw = (iw * dh + ih - 1) / ih; }
        else { rw = dw; rh = (ih * dw + iw - 1) / iw; }
        str_cat(out, "fills "); fmt_u32(n, dw); str_cat(out, n); str_cat(out, " x "); fmt_u32(n, dh); str_cat(out, n); str_cat(out, "; centered crop ");
        fmt_u32(n, (rw - dw) / 2); str_cat(out, n); str_cat(out, " x "); fmt_u32(n, (rh - dh) / 2); str_cat(out, n); str_cat(out, " px.");
    }
}
static void theme_scheme_select(int i)
{
    int s;
    if (i < 0 || i >= 8) return;
    for (s = 0; s < 8; s++) {
        if (s == 2) continue; /* Preserve the user's selected desktop colour. */
        theme_palette[theme_slots[s] * 3] = scheme_colors[i][s][0];
        theme_palette[theme_slots[s] * 3 + 1] = scheme_colors[i][s][1];
        theme_palette[theme_slots[s] * 3 + 2] = scheme_colors[i][s][2];
    }
    if (i == 7) theme_palette[9] = theme_palette[10] = theme_palette[11] = 0;
    theme_scheme = i;
    theme_dirty = 1;
    ui_palette(theme_palette);
}
static void theme_scheme_detect(void)
{
    int i, s, match;
    theme_scheme = -1;
    for (i = 0; i < 8; i++) {
        match = 1;
        for (s = 0; s < 8 && match; s++) if (s != 2 &&
            (theme_palette[theme_slots[s] * 3] != scheme_colors[i][s][0] ||
             theme_palette[theme_slots[s] * 3 + 1] != scheme_colors[i][s][1] ||
             theme_palette[theme_slots[s] * 3 + 2] != scheme_colors[i][s][2])) match = 0;
        if (match) { theme_scheme = i; break; }
    }
}
static void properties_load(void)
{
    cfg_load(&theme_cfg);
    mem_copy(&theme_saved, &theme_cfg, sizeof theme_cfg);
    mem_copy(theme_palette, theme_cfg.palette, sizeof theme_palette);
    theme_icons = theme_cfg.icons_hidden;
    theme_scheme_detect();
    theme_dirty = 0;
    wallpaper_catalog_load();
    mem_set(&wall_info, 0, sizeof wall_info);
    app_wallpaper(&wall_info);
    wall_selected = wall_info.index;
    wall_style = wall_info.style;
    if (wall_style > 4) wall_style = 0;
    if (wall_selected >= wall_count) wall_selected = 0;
    wallpaper_dimensions();
    wall_top = 0;
    wall_saved_index = wall_selected;
    wall_saved_style = wall_style;
    wall_changed = 0;
}
static void theme_restore(void)
{
    if (theme_dirty) ui_palette(theme_saved.palette);
    mem_copy(theme_palette, theme_saved.palette, sizeof theme_palette);
    theme_icons = theme_saved.icons_hidden;
    theme_scheme_detect();
    theme_dirty = 0;
}
static int properties_apply(void)
{
    int r;
    if (wall_changed) {
        r = app_wallpaper_apply((unsigned)wall_selected, (unsigned)wall_style);
        if (!r) { str_copy(status, "The background could not be saved."); return 0; }
        wall_saved_index = wall_selected; wall_saved_style = wall_style; wall_changed = 0;
    }
    if (theme_dirty || theme_icons != theme_cfg.icons_hidden) {
        mem_copy(theme_cfg.palette, theme_palette, sizeof theme_palette);
        theme_cfg.icons_hidden = theme_icons;
        if (cfg_save(&theme_cfg)) { str_copy(status, "Appearance settings could not be saved."); return 0; }
        mem_copy(&theme_saved, &theme_cfg, sizeof theme_cfg);
        theme_dirty = 0;
    }
    str_copy(status, "Properties saved.");
    return 1;
}
static void wallpaper_paint(void)
{
    int i, visible = 9, y, at;
    char geometry[96];
    static const char *styles[5] = { "Fill", "Fit", "Stretch", "Center", "Tile" };
    ui_text(X + 18, Y + 43, "Background picture", C_INK | BOLD);
    ui_inset(X + 18, Y + 62, 224, visible * 19 + 4);
    ui_rect(X + 20, Y + 64, 220, visible * 19, C_PAPER);
    for (i = 0; i < visible; i++) {
        at = wall_top + i;
        if (at >= wall_count) break;
        y = Y + 64 + i * 19;
        if (at == wall_selected) ui_rect(X + 20, y, 220, 19, C_TITLE);
        draw_frame_text(X + 27, y + 2, 204, wall_title[at], at == wall_selected ? C_PAPER : C_INK);
        /* Hit IDs are stored in one byte; encode the visible row, not the
         * catalog index, then add wall_top when the action is dispatched. */
        ui_hit(X + 20, y, 220, 19, 100 + i);
    }
    if (wall_count > visible) draw_scroll(X + 222, Y + 64, visible * 19, wall_top, wall_count, visible);
    ui_text(X + 268, Y + 43, "Preview", C_INK | BOLD);
    ui_bevel(X + 267, Y + 62, 286, 142, C_SHADOW);
    ui_rect(X + 275, Y + 70, 270, 126, C_TEAL);
    ui_rect(X + 289, Y + 82, 76, 91, C_FACE);
    ui_rect(X + 293, Y + 87, 68, 15, C_TITLE);
    ui_text(X + 298, Y + 88, "Desktop", C_PAPER | BOLD);
    ui_rect(X + 297, Y + 111, 38, 45, C_BLUE);
    ui_rect(X + 341, Y + 111, 38, 45, C_ROSE);
    ui_rect(X + 379, Y + 82, 150, 90, C_FACE);
    draw_frame_text(X + 386, Y + 88, 138, wall_title[wall_selected], C_INK | BOLD);
    ui_text(X + 386, Y + 112, wall_selected ? "Photo or tile" : "Plain desktop colour", C_INK);
    wallpaper_geometry(geometry);
    draw_frame_text(X + 267, Y + 212, 286, geometry, C_INK);
    ui_text(X + 18, Y + 240, "Picture position:", C_INK);
    for (i = 0; i < 5; i++) {
        int x = X + 18 + (i % 3) * 112, yy = Y + 260 + (i / 3) * 28;
        ui_button(x, yy, 104, 24, styles[i], 50 + i);
        if (wall_style == i) draw_focus(x + 1, yy + 1, 102, 22);
    }
}
static void appearance_paint(void)
{
    int i, y, x;
    ui_text(X + 18, Y + 43, "Colour scheme", C_INK | BOLD);
    ui_inset(X + 18, Y + 62, 204, 8 * 22 + 4);
    ui_rect(X + 20, Y + 64, 200, 8 * 22, C_PAPER);
    for (i = 0; i < 8; i++) {
        y = Y + 64 + i * 22;
        if (i == theme_scheme) ui_rect(X + 20, y, 200, 22, C_TITLE);
        draw_frame_text(X + 28, y + 3, 184, scheme_names[i], i == theme_scheme ? C_PAPER : C_INK);
        ui_hit(X + 20, y, 200, 22, 60 + i);
    }
    ui_text(X + 244, Y + 43, "Desktop colour", C_INK | BOLD);
    for (i = 0; i < 8; i++) {
        x = X + 244 + (i % 4) * 64; y = Y + 64 + (i / 4) * 34;
        ui_inset(x, y, 56, 28);
        ui_rect(x + 4, y + 4, 48, 20, i ? theme_swatches[i] : C_FACE);
        if (i && !mem_cmp(theme_palette + 9, theme_palette + theme_swatches[i] * 3, 3)) draw_focus(x + 1, y + 1, 54, 26);
        ui_hit(x, y, 56, 28, 70 + i);
    }
    ui_rect(X + 244, Y + 145, 276, 54, C_TEAL);
    ui_bevel(X + 260, Y + 153, 244, 38, C_FACE);
    ui_rect(X + 263, Y + 156, 238, 14, C_TITLE);
    ui_text(X + 269, Y + 157, "Active window", C_PAPER | BOLD);
    ui_text(X + 270, Y + 174, "Window text     [   OK   ]", C_INK);
    ui_text(X + 18, Y + 248, "Show these desktop icons:", C_INK | BOLD);
    for (i = 0; i < 8; i++) {
        x = X + 20 + (i % 4) * 137; y = Y + 268 + (i / 4) * 24;
        draw_check(x, y, !(theme_icons & (1 << i)));
        draw_frame_text(x + 19, y, 116, icon_names[i], C_INK);
        ui_hit(x, y, 130, 18, 80 + i);
    }
}
static void screen_paint(void)
{
    int i, at, y, fg, rows = 7;
    char text[96], t[16];
    const char *name = probe.monitor.valid && probe.monitor.name[0] ? probe.monitor.name :
        (probe.adapter_kind == DISP_ADAPTER_S3 && (probe.pci_device == 0x8C2E || probe.pci_device == 0x8C2F)) ?
        "IBM ThinkPad 14.1\" TFT" : "Generic display";
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
    int panel_valid = native_ok && (nfield(60) & 0x80000000UL) &&
        nfield(76) == 0x8C2E5333UL && nfield(184) == 1;
    const char *disp_name = m->valid && m->name[0] ? m->name :
        panel_valid ? "Internal LCD panel (S3 firmware)" : "Generic display";
    line(0, "Display", disp_name);
    line(1, "Identification", m->valid ? "EDID verified" :
        panel_valid ? "Native panel size verified" : "Not supplied by firmware / driver");
    line(2, "Connection", virtual_display() ? "Virtual display" :
        panel_valid ? "Internal LCD active" :
        !m->valid ? "VGA / Internal display" : m->input_digital ? "Digital (EDID)" : "Analog RGB (EDID)");
    line(3, "Manufacturer", m->valid ? m->manufacturer : "Not reported");
    if (m->valid) { fmt_hex4(t, m->product); str_cat(t, "   Serial "); fmt_u32(n, m->serial); str_cat(t, n); }
    else str_copy(t, "Not reported");
    line(4, "Product", t);
    if (m->valid && m->width_cm && m->height_cm) {
        fmt_u32(t, m->width_cm); str_cat(t, " x "); fmt_u32(n, m->height_cm); str_cat(t, n); str_cat(t, " cm");
    }
    else str_copy(t, "Not reported");
    line(5, "Image size", t);
    str_copy(t, "Not reported");
    if (m->valid && m->preferred_width && m->preferred_height) {
        geometry(t, m->preferred_width, m->preferred_height, 0);
        if (m->preferred_millihz) {
            str_cat(t, "  "); fmt_u32(n, m->preferred_millihz / 1000UL); str_cat(t, n);
            str_cat(t, "."); fmt_2(n, (u16)((m->preferred_millihz % 1000UL) / 10UL)); str_cat(t, n); str_cat(t, " Hz");
        }
    } else if (panel_valid) {
        geometry(t, (u16)nfield(176), (u16)nfield(180), 0);
    }
    line(6, "Preferred timing", t);
    line(7, "Active refresh", virtual_display() ? "Managed by the host display" : "Video BIOS timing; rate not reported");
    draw_frame_text(X + 18, Y + 272, W - 36,
        virtual_display() ? "This is the virtual screen exposed by QEMU." :
        panel_valid ? "Native LCD size comes from the active S3 panel registers." :
        "Preferred timing comes from EDID; active timing may differ.", C_INK);
}
static void advanced_paint(void)
{
    char t[96], n[16];
    line(0, "Driver", driver_name());
    line(1, "Provider / version", "CiukiOS / 0.8.3");
    line(2, "Component", native_ok && nfield(8) ? "C:\\VM\\CVSESS.DLL" : "Built-in VGA / VBE framebuffer");
    line(3, "Presentation", native_ok && nfield(8) == 1 ? "Native texture transfer and presentation" : native_ok && nfield(8) == 3 ? "GPU 2D commands; protected framebuffer" : native_ok && nfield(8) == 2 ? "Native scanout; CPU rendering" : "Firmware scanout; CPU rendering");
    line(4, "3D interface", native_ok && nfield(8) == 3 && (nfield(60) & 2) ? "Hardware Gouraud triangles (PIO)" : "Native triangles unavailable");
    line(5, "Refresh control", virtual_display() ? "Host compositor / QEMU" : "Video BIOS default; custom timing unavailable");
    if (native_ok) {
        fmt_u32(t, nfield(48) / 1024UL); str_cat(t, " KB   Error "); fmt_u32(n, nfield(32)); str_cat(t, n);
    } else str_copy(t, "Resident display interface unavailable");
    line(6, "Framebuffer", t);
    str_copy(t, "Not applicable");
    if (native_ok && nfield(8) == 1) {
        fmt_u32(t, nfield(40)); str_cat(t, " / "); fmt_u32(n, nfield(36)); str_cat(t, n); str_cat(t, " commands completed / submitted");
    } else if (native_ok && nfield(8) == 3 && (nfield(60) & 0x80000000UL)) {
        str_copy(t, "2D "); fmt_u32(n, nfield(112)); str_cat(t, n);
        str_cat(t, "   3D "); fmt_u32(n, nfield(120)); str_cat(t, n);
        str_cat(t, "   Tests "); fmt_u32(n, nfield(140)); str_cat(t, n);
        str_cat(t, "/"); fmt_u32(n, nfield(160)); str_cat(t, n);
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
    for (i = 0; i < 6; i++) {
        int x = X + 8 + i * 96;
        ui_bevel(x, Y + 5 + (i == page ? 0 : 3), 95, i == page ? 27 : 24, C_FACE);
        ui_text(x + 8, Y + 10, tabs[i], C_INK | (i == page ? BOLD : 0));
        ui_hit(x, Y + 5, 95, 27, i);
    }
    ui_rect(X + 8, Y + 31, W - 16, 1, C_PAPER);
    if (page == 0) screen_paint();
    else if (page == 1) adapter_paint();
    else if (page == 2) monitor_paint();
    else if (page == 3) advanced_paint();
    else if (page == 4) wallpaper_paint();
    else appearance_paint();
    draw_frame_text(X + 18, Y + H - 62, W - 36,
        page == 0 ? status : page == 1 ? "The active driver is selected for the detected hardware." :
        page == 2 ? "Press F5 to detect the display again." :
        page == 3 ? "Built-in video drivers are updated with the system image." : status, C_INK);
    if (page == 0) {
        button(18, H - 36, 129, "Detect displays", 10);
        button(158, H - 36, 141, "Advanced...", 3);
        button(W - 274, H - 36, 80, "Apply...", 11);
    } else {
        button(W - 274, H - 36, 80, "Apply", 11);
    }
    button(W - 184, H - 36, 80, "OK", 15);
    button(W - 94, H - 36, 80, "Cancel", 14);
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
    int kept = 0;
    if (!preview) return;
    diag_event(keep ? "keep" : "rollback", preview_mode, (u16)app_display_mode(0));
    diag_active(keep ? "keep" : "rollback", (int)app_display_mode(0));
    notice.open = 0;
    dialog_sync(&notice);
    if (keep && save_mode(preview_mode)) {
        kept = 1;
        str_copy(status, "Display mode saved.");
        app_log("[DISPLAY] kept", "graphics");
    } else {
        if (app_display_mode(previous_mode) < 0) {
            str_copy(status, "Could not restore the previous mode. Open Display again.");
            diag_event("restore-fail", previous_mode, (u16)app_display_mode(0));
            app_log("[DISPLAY] restore failed", "graphics");
            preview = 0; pending_probe = 1; close_after_preview = 0;
            ui_repaint_win(WIN_DISPLAY);
            return;
        }
        str_copy(status, keep ? "Profile could not be saved. Previous mode restored." : "Previous display mode restored.");
        diag_event(keep ? "save-fail" : "restored", previous_mode, (u16)app_display_mode(0));
        app_log("[DISPLAY] reverted", "graphics");
    }
    preview = 0; pending_probe = 1;
    if (!kept) close_after_preview = 0;
    else if (close_after_preview) { close_after_preview = 0; ui_repaint_win(WIN_DISPLAY); app_close(); return; }
    ui_repaint_win(WIN_DISPLAY);
}
static void apply(void)
{
    if (!probe.mode_count) { diag_event("no-modes", 0, probe.flags); return; }
    native_read();
    diag_event("apply", probe.modes[selected].id, probe.modes[selected].flags);
    diag_mode("selected", probe.modes[selected].id);
    if (vm_seg && !native_ok) {
        diag_event("native-unknown", probe.modes[selected].id, 0xFFFF);
        msgbox(&notice, "Display settings", "The session manager did not report its state.\nDisplay settings could not be changed safely.", "OK");
        return;
    }
    if (native_ok && nfield(56)) {
        diag_event("blocked-dos", probe.modes[selected].id, (u16)nfield(56));
        msgbox(&notice, "Display settings", "Close DOS windows before changing the display mode.\nThe current game will keep running.", "OK");
        app_log("[DISPLAY] blocked", "DOS windows open");
        return;
    }
    if (probe.modes[selected].id == probe.current_mode) {
        diag_event("already-active", probe.current_mode, probe.current_mode_flags);
        str_copy(status, "The selected display mode is already active."); return;
    }
    previous_mode = probe.current_mode;
    preview_mode = probe.modes[selected].id;
    if (app_display_mode(preview_mode) < 0) {
        diag_event("queue-fail", preview_mode, (u16)app_display_mode(0));
        str_copy(status, "The display change could not be queued."); return;
    }
    preview = 1; preview_tick = HOST.ticks;
    str_copy(status, "Applying display mode...");
    diag_event("queued", preview_mode, previous_mode);
    app_log("[DISPLAY] preview", "graphics");
}
static u16 mode_access_flags(u16 id)
{
    u16 i;
    for (i = 0; i < probe.mode_count; ++i)
        if (probe.modes[i].id == id) return probe.modes[i].flags;
    return 0;
}
static void accept_properties(void)
{
    if (!properties_apply()) return;
    if (probe.mode_count && probe.modes[selected].id != probe.current_mode) {
        close_after_preview = 1;
        apply();
        if (!preview) close_after_preview = 0;
        return;
    }
    app_close();
}
static int action(int id)
{
    int i;
    if (id >= 0 && id < 6) { page = id; focus = 0; return 1; }
    if (id >= 20 && id < 27 && top + id - 20 < probe.mode_count) {
        selected = top + id - 20; focus = 1; return 1;
    }
    if (id >= 100 && id < 109 && wall_top + id - 100 < wall_count) {
        wall_selected = wall_top + id - 100; wallpaper_dimensions();
        wall_changed = wall_selected != wall_saved_index || wall_style != wall_saved_style; return 1;
    }
    if (id >= 50 && id < 55) {
        wall_style = id - 50; wall_changed = wall_selected != wall_saved_index || wall_style != wall_saved_style; return 1;
    }
    if (id >= 60 && id < 68) { theme_scheme_select(id - 60); return 1; }
    if (id >= 70 && id < 78) {
        i = id - 70;
        mem_copy(theme_palette + 9, i ? theme_palette + theme_swatches[i] * 3 : cfg_default_palette + 9, 3);
        theme_dirty = 1; ui_palette(theme_palette); return 1;
    }
    if (id >= 80 && id < 88) {
        theme_icons ^= (u16)(1 << (id - 80)); return 1;
    }
    if (id == 10) { pending_probe = 1; return 1; }
    if (id == 11) {
        if (page == 0) { if (properties_apply()) apply(); }
        else properties_apply();
        return 1;
    }
    if (id == 12) app_open(WIN_DEVICES, "");
    if (id == 13) app_open(WIN_DEVICES, "drivers");
    if (id == 14) { theme_restore(); app_close(); }
    if (id == 15) { accept_properties(); return 1; }
    return 1;
}
int app_event(int ev, int a, int b, int c)
{
    int r, orig = ev, scan, ch, sx, sy, i, had_monitor, old_backend, old_phase;
    if (ev == EV_OPEN) {
        str_copy(app_title, "Display Properties");
        if (!HDR_WIDTH) { HDR_WIDTH = 606; HDR_HEIGHT = 414; }
        if (!str_icmp(APP_ARG, "background") || !str_icmp(APP_ARG, "wallpaper")) page = 4;
        else if (!str_icmp(APP_ARG, "appearance")) page = 5;
        else if (!str_icmp(APP_ARG, "advanced")) page = 3;
        else if (!str_icmp(APP_ARG, "monitor")) page = 2;
        else page = 0;
        APP_ARG[0] = 0;
        properties_load();
        pending_probe = 1;
        app_log("[DISPLAY] open", "Display Properties");
        return 1;
    }
    if (ev == EV_POLL && preview) {
        if ((unsigned)(HOST.ticks - preview_tick) >= 219) {
            diag_event("timeout", preview_mode, (u16)app_display_mode(0));
            diag_active("timeout", (int)app_display_mode(0));
            finish_preview(0); return 1;
        }
        if (preview == 1 && app_display_mode(0) != 0) {
            int app_mode = app_display_mode(0), vbe_ok;
            u16 vbe_ax, vbe_bx, pitch, pixels, lines, scan;
            int linear_active;
            vbe_ok = read_active_vbe(&vbe_ax, &vbe_bx, &pitch, &pixels, &lines, &scan);
            diag_active("preview", app_mode);
            linear_active = !!(vbe_bx & 0x4000);
            if (app_mode != preview_mode || !vbe_ok ||
                (vbe_bx & 0x3FFF) != preview_mode ||
                (linear_active && ((display_in_v86() && !protected_lfb_ok) ||
                    !(mode_access_flags(preview_mode) & DISP_MODE_LINEAR))) ||
                (!linear_active && !(mode_access_flags(preview_mode) & DISP_MODE_BANKED))) {
                diag_event("verify-fail", preview_mode, vbe_bx);
                finish_preview(0); return 1;
            }
            diag_event("preview-ok", preview_mode, vbe_bx);
            diag_mode("preview", preview_mode);
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
        if (ch == 27) { theme_restore(); app_close(); break; }
        if (ch >= '1' && ch <= '6') { page = ch - '1'; focus = 0; break; }
        if (ch == 9) { focus = (focus + 1) % 6; break; }
        if (scan == K_F5) {
            if (page == 4) wallpaper_refresh();
            else pending_probe = 1;
            break;
        }
        if (ch == 13) { accept_properties(); break; }
        if (!ch || ch == 0xE0) {
            if (scan == K_LEFT && page != 4) { page = (page + 5) % 6; break; }
            if (scan == K_RIGHT && page != 4) { page = (page + 1) % 6; break; }
            if (page == 4 && wall_count) {
                if (scan == K_HOME) wall_selected = 0;
                if (scan == K_END) wall_selected = wall_count - 1;
                if (scan == K_UP && wall_selected > 0) wall_selected--;
                if (scan == K_DOWN && wall_selected + 1 < wall_count) wall_selected++;
                if (scan == K_LEFT && wall_style > 0) wall_style--;
                if (scan == K_RIGHT && wall_style < 4) wall_style++;
                if (wall_selected < wall_top) wall_top = wall_selected;
                if (wall_selected >= wall_top + 9) wall_top = wall_selected - 8;
                wall_changed = wall_selected != wall_saved_index || wall_style != wall_saved_style;
                wallpaper_dimensions();
                break;
            }
            if (page == 5 && (scan == K_UP || scan == K_DOWN) && theme_scheme >= 0) {
                i = theme_scheme + (scan == K_UP ? -1 : 1);
                if (i >= 0 && i < 8) theme_scheme_select(i);
                break;
            }
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
        if (page == 4 && wall_count > 9 && !notice.open) {
            wall_top += a * 3;
            if (wall_top > wall_count - 9) wall_top = wall_count - 9;
            if (wall_top < 0) wall_top = 0;
            return 1;
        }
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
        if (page == 4 && a == MOUSE_DOWN && sx >= X + 222 && sx < X + 238 && sy >= Y + 64 && sy < Y + 235 && wall_count > 9) {
            long hit = scroll_hit(X + 222, Y + 64, 171, sy, wall_count, 9);
            if (hit == -1 && wall_top > 0) wall_top--;
            else if (hit == -2 && wall_top + 9 < wall_count) wall_top++;
            else if (hit >= 0) wall_top = (int)hit;
            if (wall_top > wall_count - 9) wall_top = wall_count - 9;
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
    case EV_CLOSE: theme_restore(); finish_preview(0); return 0;
    default: r = 0; break;
    }
    r = dialog_post(&notice, WIN_DISPLAY, ev, r);
    return orig == EV_CLOSE && ev != EV_CLOSE ? 0 : r;
}
