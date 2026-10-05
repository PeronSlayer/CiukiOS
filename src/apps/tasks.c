/* Task Manager for the CiukiOS desktop, in the manner of Windows XP's:
 * Applications (desktop windows: switch to, end task, new task), Processes
 * (the DOS memory arena by owner: name, PID, memory, blocks), Virtual
 * Machines (each DOS VM of the VM manager: state, its DOS window session and
 * devices, focus; end one) and Performance (CPU usage history from the
 * desktop's idle time, conventional/XMS/EMS memory, disk). */
#include "app.h"

#define MAX_PROC 40
#define HIST 60
#define VMM_MAX 4

static int tab;                         /* 0 apps, 1 processes, 2 VMs, 3 performance */
static int sel_row[4], top_row[4];
static int speed = 1;                   /* 0 high, 1 normal, 2 low, 3 paused */
static unsigned last_tick;
static u8 cpu_hist[HIST];
static int hist_n;
static int sort_col, sort_desc;
static char status_line[96];
static unsigned click_tick;

/* ---- Applications ---- */
static char winbuf[32 * 25];
static int nwin;
static int app_ids[32];
static void load_windows(void)
{
    int n = app_windows(winbuf), i;
    nwin = 0;
    for (i = 0; i < n; i++) {
        u8 st = (u8)winbuf[i * 25];
        if (i == WIN_TASKS || i == 7) continue;           /* itself, the launch menu */
        if ((st & 0x7F) == 1 || (st & 0x7F) == 2) app_ids[nwin++] = i;
    }
}

/* ---- Processes: owners of the DOS memory arena ---- */
struct proc { char name[13]; u16 psp; u32 bytes; int blocks; };
static struct proc procs[MAX_PROC];
static int nproc;
static u32 arena_free, arena_total;
static void psp_name(u16 psp, char *out)
{
    u16 env, off;
    int n;
    const char *p;
    char path[80];
    if (psp == 8) { str_copy(out, "DOS (system)"); return; }
    if (psp < 0x40 || peek16(psp, 0) != 0x20CD) {       /* not a program (PSP) */
        str_copy(out, "Data ");
        fmt_hex4(out + 5, psp);
        return;
    }
    /* The program path after the environment, else the MCB name (DOS 4+). */
    env = peek16(psp, 0x2C);
    if (env && env < 0xA000) {
        off = 0;
        while (off < 4000 && (peek8(env, off) || peek8(env, off + 1))) off++;
        if (off < 4000 && peek16(env, off + 2) >= 1) {
            off += 4;
            for (n = 0; n < 79; n++) {
                char c = (char)peek8(env, off + n);
                if (!c) break;
                path[n] = c;
            }
            path[n] = 0;
            p = path;
            { const char *q = path; while (*q) { if (*q == '\\' || *q == ':') p = q + 1; q++; } }
            if (*p) { str_ncopy(out, p, 13); return; }
        }
    }
    n = 0;
    while (n < 8) {
        char c = (char)peek8(psp - 1, 8 + n);
        if (c < 33 || c > 126) break;
        out[n++] = c;
    }
    out[n] = 0;
    if (!n) { str_copy(out, "PSP "); fmt_hex4(out + 4, psp); }
}
static int proc_less(struct proc *a, struct proc *b)
{
    int r = sort_col == 0 ? str_icmp(a->name, b->name) : sort_col == 1 ? (a->psp < b->psp ? -1 : a->psp > b->psp) :
            sort_col == 2 ? (a->bytes < b->bytes ? -1 : a->bytes > b->bytes) : (a->blocks - b->blocks);
    return sort_desc ? r > 0 : r < 0;
}
static void load_processes(void)
{
    struct regs r;
    u16 mcb;
    int guard = 0, i, j;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x5200;
    r.ds = r.es = app_seg();
    intr(0x21, &r);
    nproc = 0;
    arena_free = arena_total = 0;
    mcb = peek16(r.es, r.bx - 2);
    while (guard++ < 400) {
        u8 kind = peek8(mcb, 0);
        u16 owner = peek16(mcb, 1), paras = peek16(mcb, 3);
        if (kind != 'M' && kind != 'Z') break;
        arena_total += (u32)(paras + 1) * 16;
        if (!owner) arena_free += (u32)paras * 16;
        else {
            for (i = 0; i < nproc && procs[i].psp != owner; i++) ;
            if (i == nproc && nproc < MAX_PROC) {
                procs[i].psp = owner;
                procs[i].bytes = 0;
                procs[i].blocks = 0;
                psp_name(owner, procs[i].name);
                nproc++;
            }
            if (i < nproc) { procs[i].bytes += (u32)paras * 16; procs[i].blocks++; }
        }
        if (kind == 'Z') break;
        mcb += paras + 1;
    }
    for (i = 1; i < nproc; i++)
        for (j = i; j > 0 && proc_less(&procs[j], &procs[j - 1]); j--) {
            struct proc t;
            mem_copy(&t, &procs[j], sizeof t);
            mem_copy(&procs[j], &procs[j - 1], sizeof t);
            mem_copy(&procs[j - 1], &t, sizeof t);
        }
}

/* ---- Virtual machines (CVSESSION) ---- */
static u16 vm_seg, vm_off;
static u8 vm_list[VMM_MAX * 4];
static int vm_count, vm_focus, vm_current, vm_ok;
static u32 vm_switches;
static u16 vm_last_exit;
static void vm_find(void)
{
    struct regs r;
    if (vm_seg) return;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x1684; r.bx = 0x4349;
    r.ds = r.es = 0;
    intr(0x2F, &r);
    if (r.es || r.di) { vm_seg = r.es; vm_off = r.di; }
}
static void load_vms(void)
{
    struct regs r;
    vm_ok = 0;
    vm_find();
    if (!vm_seg) return;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x43;                                       /* VMM_STATE */
    r.ds = r.es = app_seg();
    if (far_regs(vm_seg, vm_off, &r)) return;
    vm_count = r.bx;
    vm_current = r.cx;
    vm_focus = r.cxh;
    vm_switches = ((u32)r.dxh << 16) | r.dx;
    vm_last_exit = r.di;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x48;                                       /* VMM_LIST */
    r.ds = r.es = app_seg();
    r.di = (u16)vm_list;
    if (far_regs(vm_seg, vm_off, &r)) return;
    vm_ok = 1;
}
static void vm_kill(int vm)
{
    struct regs r;
    mem_set(&r, 0, sizeof r);
    r.ax = 0x45; r.bx = vm;
    r.ds = r.es = app_seg();
    if (far_regs(vm_seg, vm_off, &r)) app_sound(5);
}

/* ---- Performance ---- */
static u16 xms_seg, xms_off;
static int xms_checked;
static long xms_free_kb, xms_largest_kb, ems_free_kb, ems_total_kb, conv_total_kb, conv_largest_kb;
static void load_performance(void)
{
    struct regs r;
    mem_set(&r, 0, sizeof r);
    r.ds = r.es = app_seg();
    intr(0x12, &r);
    conv_total_kb = r.ax;
    conv_largest_kb = (long)dos_largest() * 16 / 1024;
    if (!xms_checked) {
        xms_checked = 1;
        mem_set(&r, 0, sizeof r);
        r.ax = 0x4300; r.ds = r.es = app_seg();
        intr(0x2F, &r);
        if ((r.ax & 0xFF) == 0x80) {
            mem_set(&r, 0, sizeof r);
            r.ax = 0x4310; r.ds = r.es = app_seg();
            intr(0x2F, &r);
            xms_seg = r.es; xms_off = r.bx;
        }
    }
    xms_free_kb = xms_largest_kb = -1;
    if (xms_seg) {
        mem_set(&r, 0, sizeof r);
        /* XMS 2.x AH=08h clips a 128/256 MiB machine at 64 MiB. */
        r.ax = 0x8800; r.ds = r.es = app_seg();
        far_regs(xms_seg, xms_off, &r);
        if ((r.bx & 0xff) == 0) {
            xms_largest_kb = ((u32)r.axh << 16) | r.ax;
            xms_free_kb = ((u32)r.dxh << 16) | r.dx;
        }
    }
    ems_free_kb = ems_total_kb = -1;
    if (peek16(0, 0x67 * 4 + 2)) {
        u16 seg = peek16(0, 0x67 * 4 + 2);
        if (peek8(seg, 10) == 'E' && peek8(seg, 11) == 'M' && peek8(seg, 12) == 'M') {   /* EMMXXXX0 */
            mem_set(&r, 0, sizeof r);
            r.ax = 0x4200; r.ds = r.es = app_seg();
            intr(0x67, &r);
            if (!(r.ax & 0xFF00)) { ems_free_kb = (long)r.bx * 16; ems_total_kb = (long)r.dx * 16; }
        }
    }
}
static void sample(void)
{
    int cpu = 100 - app_idle();
    if (cpu < 0) cpu = 0;
    if (hist_n < HIST) cpu_hist[hist_n++] = (u8)cpu;
    else { mem_move(cpu_hist, cpu_hist + 1, HIST - 1); cpu_hist[HIST - 1] = (u8)cpu; }
}
static void refresh_all(void)
{
    load_windows();
    load_processes();
    load_vms();
    load_performance();
}

/* ---- Menus ---- */
enum { T_NEW = 1, T_EXIT, T_REFRESH, T_HIGH, T_NORMAL, T_LOW, T_PAUSED, T_ABOUT,
       T_END, T_SWITCH, T_KILLVM, T_FOCUSVM, T_MINIMIZE, T_MAXIMIZE };
static struct menu_item m_file[] = { { "&New Task (Run...)", 0, T_NEW, 0 }, { "", 0, 0, MI_SEP },
                                     { "E&xit Task Manager", 0, T_EXIT, 0 } };
static struct menu_item m_view[] = { { "&Refresh Now", "F5", T_REFRESH, 0 }, { "", 0, 0, MI_SEP },
                                     { "&High (twice a second)", 0, T_HIGH, 0 }, { "&Normal (every second)", 0, T_NORMAL, 0 },
                                     { "&Low (every 4 seconds)", 0, T_LOW, 0 }, { "&Paused", 0, T_PAUSED, 0 } };
static struct menu_item m_help[] = { { "&About Task Manager", 0, T_ABOUT, 0 } };
static struct menu menus[] = { { "&File", m_file, 3 }, { "&View", m_view, 6 }, { "&Help", m_help, 1 } };
static struct menubar bar = { menus, 3, 0, 0, 0, -1, 0 };
/* Right-click menus, as in Windows' Task Manager. */
static struct menu_item m_app[] = { { "&Switch To", 0, T_SWITCH, 0 }, { "Mi&nimize", 0, T_MINIMIZE, 0 },
                                    { "Ma&ximize", 0, T_MAXIMIZE, 0 }, { "", 0, 0, MI_SEP },
                                    { "&End Task", 0, T_END, 0 } };
static struct menu_item m_vm[] = { { "&Give Focus", 0, T_FOCUSVM, 0 }, { "", 0, 0, MI_SEP }, { "&End VM", 0, T_KILLVM, 0 } };
static struct menu_item m_rest[] = { { "&Refresh Now", 0, T_REFRESH, 0 }, { "&New Task (Run...)", 0, T_NEW, 0 } };
static struct popup ctx;
static struct dialog dlg;
static struct dctl dc[6];

/* ---- Layout ---- */
static int X0, Y0, W, H, tab_y, body_x, body_y, body_w, body_h, btn_y;
static void layout(void)
{
    X0 = HOST.x + 3; Y0 = HOST.y + TITLE_H; W = HOST.w - 6; H = HOST.h - TITLE_H - 4;
    tab_y = Y0 + MENUBAR_H + 4;
    body_x = X0 + 6; body_y = tab_y + 24; body_w = W - 12;
    btn_y = Y0 + H - 22 - 32;
    body_h = btn_y - body_y - 6;
}
static const char *tab_names[] = { "Applications", "Processes", "Virtual Machines", "Performance" };
static int tab_x(int t, int *w)
{
    int i, x = body_x;
    for (i = 0; i < 4; i++) {
        int tw = ui_measure(tab_names[i]) + 20;
        if (i == t) { *w = tw; return x; }
        x += tw;
    }
    *w = 0;
    return x;
}
static int rows_count(void) { return tab == 0 ? nwin : tab == 1 ? nproc : tab == 2 ? VMM_MAX : 0; }

static void header(int x, const char *s, int w)
{
    ui_bevel(x, body_y + 2, w, 20, C_FACE);
    ui_text(x + 6, body_y + 4, s, C_INK);
}
static void row_bg(int i, int y, int *fg)
{
    *fg = C_INK;
    if (i == sel_row[tab]) { ui_rect(body_x + 2, y, body_w - 4, 18, C_TITLE); *fg = C_PAPER; }
}
static void button(int x, int w, const char *label, int enabled)
{
    ui_bevel(x, btn_y, w, 26, C_FACE);
    ui_text(x + (w - ui_measure(label)) / 2, btn_y + 5, label, enabled ? C_INK : C_SHADOW);
}
static const char *sess_text(u8 s, char *out)
{
    out[0] = 0;
    if (!(s & 1)) return "-";
    str_copy(out, "DOS window: video");
    if (s & 8) str_cat(out, ", keyboard");
    if (s & 0x10) str_cat(out, ", mouse");
    if (s & 4) str_cat(out, ", sound");
    return out;
}
static void kb_text(char *out, long kb)
{
    char n[16];
    if (kb < 0) { str_copy(out, "not present"); return; }
    fmt_u32_group(n, (u32)kb);
    str_copy(out, n);
    str_cat(out, " KB");
}
static void draw_performance(void)
{
    int gx = body_x + 8, gy = body_y + 22, gw = body_w - 16, gh = 110, i, cpu = hist_n ? cpu_hist[hist_n - 1] : 0;
    char t[64], n[24];
    int y;
    ui_text(gx, body_y + 2, "CPU Usage History", C_INK | BOLD);
    ui_rect(gx, gy, gw, gh, C_INK);
    for (i = 1; i < 4; i++) ui_rect(gx, gy + i * gh / 4, gw, 1, C_GREEN);
    for (i = 0; i < hist_n; i++) {
        int bw = gw / HIST, bx = gx + gw - (hist_n - i) * bw, bh = cpu_hist[i] * (gh - 2) / 100;
        if (bh > 0) ui_rect(bx, gy + gh - 1 - bh, bw > 1 ? bw - 1 : 1, bh, C_CYAN);
    }
    str_copy(t, "CPU Usage: ");
    fmt_u32(n, (u32)cpu); str_cat(t, n); str_cat(t, "%   (the desktop's time not idle; DOS VMs included)");
    ui_text(gx, gy + gh + 4, t, C_INK);
    y = gy + gh + 30;
    ui_text(gx, y, "Memory", C_INK | BOLD);
    y += 20;
    str_copy(t, "Conventional: "); kb_text(n, conv_total_kb); str_cat(t, n);
    str_cat(t, " total, largest free block "); kb_text(n, conv_largest_kb); str_cat(t, n);
    ui_text(gx, y, t, C_INK); y += 18;
    str_copy(t, "DOS arena: "); fmt_size(n, arena_total); str_cat(t, n); str_cat(t, ", free "); fmt_size(n, arena_free); str_cat(t, n);
    ui_text(gx, y, t, C_INK); y += 18;
    str_copy(t, "Extended (XMS) free: "); kb_text(n, xms_free_kb); str_cat(t, n);
    if (xms_largest_kb >= 0) { str_cat(t, ", largest "); kb_text(n, xms_largest_kb); str_cat(t, n); }
    ui_text(gx, y, t, C_INK); y += 18;
    str_copy(t, "Expanded (EMS) free: "); kb_text(n, ems_free_kb); str_cat(t, n);
    if (ems_total_kb >= 0) { str_cat(t, " of "); kb_text(n, ems_total_kb); str_cat(t, n); }
    ui_text(gx, y, t, C_INK); y += 18;
    {
        long total, free = dos_disk_free(0, &total);
        str_copy(t, "Disk (current drive): ");
        if (free >= 0) { fmt_size(n, (u32)free); str_cat(t, n); str_cat(t, " free of "); fmt_size(n, (u32)total); str_cat(t, n); }
        else str_cat(t, "unknown");
        ui_text(gx, y, t, C_INK); y += 18;
    }
    if (vm_ok) {
        str_copy(t, "VM manager: ");
        fmt_u32(n, (u32)vm_count); str_cat(t, n); str_cat(t, " VM(s), ");
        fmt_u32_group(n, vm_switches); str_cat(t, n); str_cat(t, " switches");
        ui_text(gx, y, t, C_INK);
    }
}
static void paint(void)
{
    int i, y, x, w, fg, rows;
    char t[80], n[24];
    layout();
    bar.x = X0; bar.y = Y0; bar.w = W;
    ui_rect(X0, tab_y - 3, W, H - MENUBAR_H, C_FACE);
    for (i = 0; i < 4; i++) {
        x = tab_x(i, &w);
        ui_bevel(x, tab_y + (i == tab ? 0 : 3), w, i == tab ? 24 : 21, C_FACE);
        ui_text(x + 10, tab_y + (i == tab ? 3 : 5), tab_names[i], i == tab ? (C_INK | BOLD) : C_INK);
    }
    ui_inset(body_x, body_y, body_w, body_h);
    ui_rect(body_x + 2, body_y + 2, body_w - 4, body_h - 4, C_PAPER);
    rows = (body_h - 26) / 18;
    if (top_row[tab] > rows_count() - rows) top_row[tab] = rows_count() - rows;
    if (top_row[tab] < 0) top_row[tab] = 0;
    if (tab == 0) {
        header(body_x + 2, "Task", body_w * 2 / 3);
        header(body_x + 2 + body_w * 2 / 3, "Status", body_w - 4 - body_w * 2 / 3);
        for (i = top_row[tab]; i < nwin && i < top_row[tab] + rows; i++) {
            u8 st = (u8)winbuf[app_ids[i] * 25];
            y = body_y + 24 + (i - top_row[tab]) * 18;
            row_bg(i, y, &fg);
            ui_text(body_x + 8, y + 1, winbuf + app_ids[i] * 25 + 1, fg);
            ui_text(body_x + 8 + body_w * 2 / 3, y + 1, (st & 0x7F) == 2 ? "Minimized" : (st & 0x80) ? "Running (active)" : "Running", fg);
        }
        if (!nwin) ui_text(body_x + 10, body_y + 30, "No other windows are open.", C_SHADOW);
        button(body_x + body_w - 3 * 100, 94, "End Task", nwin > 0);
        button(body_x + body_w - 2 * 100, 94, "Switch To", nwin > 0);
        button(body_x + body_w - 100, 94, "New Task...", 1);
    } else if (tab == 1) {
        static const char *cols[] = { "Image Name", "PID", "Mem Usage", "Blocks" };
        int cx[5];
        cx[0] = body_x + 2; cx[1] = body_x + (int)((long)body_w * 45 / 100); cx[2] = body_x + (int)((long)body_w * 62 / 100);
        cx[3] = body_x + (int)((long)body_w * 82 / 100); cx[4] = body_x + body_w - 2;
        for (i = 0; i < 4; i++) header(cx[i], cols[i], cx[i + 1] - cx[i]);
        for (i = top_row[tab]; i < nproc && i < top_row[tab] + rows; i++) {
            y = body_y + 24 + (i - top_row[tab]) * 18;
            row_bg(i, y, &fg);
            ui_text(cx[0] + 6, y + 1, procs[i].name, fg);
            fmt_hex4(t, procs[i].psp); ui_text(cx[1] + 6, y + 1, t, fg);
            fmt_u32_group(n, (procs[i].bytes + 1023) / 1024); str_copy(t, n); str_cat(t, " K");
            ui_text(cx[3] - 10 - ui_measure(t), y + 1, t, fg);
            fmt_u32(t, (u32)procs[i].blocks); ui_text(cx[3] + 6, y + 1, t, fg);
        }
        str_copy(t, "Owners of the DOS memory arena (this desktop VM).");
        ui_text(body_x, btn_y + 5, t, C_SHADOW);
    } else if (tab == 2) {
        header(body_x + 2, "VM", 90);
        header(body_x + 92, "State", 110);
        header(body_x + 202, "Session", body_w - 4 - 200 - 80);
        header(body_x + body_w - 82, "Focus", 80);
        if (!vm_ok) ui_text(body_x + 10, body_y + 30, "The VM manager is not running (safe boot or no CVSESSION).", C_SHADOW);
        else for (i = 0; i < VMM_MAX; i++) {
            u8 st = vm_list[i * 4], s = vm_list[i * 4 + 1];
            y = body_y + 24 + (i - top_row[tab]) * 18;
            row_bg(i, y, &fg);
            str_copy(t, i ? "VM " : "System VM");
            if (i) { fmt_u32(n, (u32)i); str_cat(t, n); }
            ui_text(body_x + 8, y + 1, t, fg);
            ui_text(body_x + 98, y + 1, i == 0 ? "Running (desktop)" : st == 1 ? "Running" : st == 2 ? "Ending" : "-", fg);
            ui_text(body_x + 208, y + 1, sess_text(s, t), fg);
            ui_text(body_x + body_w - 76, y + 1, vm_list[i * 4 + 2] ? "Yes" : "", fg);
        }
        button(body_x + body_w - 2 * 110, 104, "Give Focus", vm_ok && sel_row[2] > 0 && vm_list[sel_row[2] * 4] == 1);
        button(body_x + body_w - 110, 104, "End VM", vm_ok && sel_row[2] > 0 && vm_list[sel_row[2] * 4] == 1);
    } else {
        draw_performance();
    }
    /* Status bar. */
    y = Y0 + H - 20;
    ui_rect(X0, y - 1, W, 21, C_FACE);
    str_copy(t, "Processes: "); fmt_u32(n, (u32)nproc); str_cat(t, n);
    str_cat(t, "    CPU Usage: "); fmt_u32(n, (u32)(hist_n ? cpu_hist[hist_n - 1] : 0)); str_cat(t, n); str_cat(t, "%");
    str_cat(t, "    DOS free block: "); kb_text(n, conv_largest_kb); str_cat(t, n);
    if (vm_ok) { str_cat(t, "    VMs: "); fmt_u32(n, (u32)vm_count); str_cat(t, n); }
    ui_inset(X0 + 2, y, W - 4, 19);
    ui_text(X0 + 8, y + 1, status_line[0] ? status_line : t, C_INK);
    menubar_draw(&bar);
    if (ctx.open) popup_draw(&ctx);
}
static void paint_dialog(void)
{
    dialog_draw(&dlg);
    ui_icon(dlg.x + 8, dlg.y + DIALOG_TITLE_H + 4, ICON_MONITOR);
}

/* ---- Commands and input ---- */
static void log_tab(void)
{
    char t[40], n[8];
    str_copy(t, tab_names[tab]);
    str_cat(t, " ");
    fmt_u32(n, (u32)rows_count());
    str_cat(t, n);
    app_log("[TASKS] tab", t);
}
static void command(int id)
{
    int i = sel_row[tab];
    status_line[0] = 0;
    switch (id) {
    case T_NEW: app_window_cmd(WIN_RUN, 1); break;
    case T_EXIT: app_close(); break;
    case T_REFRESH: refresh_all(); sample(); break;
    case T_HIGH: case T_NORMAL: case T_LOW: case T_PAUSED: speed = id - T_HIGH; break;
    case T_ABOUT:
        dc[0].type = DC_LABEL; dc[0].x = 50; dc[0].y = 0; dc[0].text = "CiukiOS Task Manager"; dc[0].disabled = 0;
        dc[1].type = DC_LABEL; dc[1].x = 50; dc[1].y = 20; dc[1].text = "Version 0.8.3 - A modern Retro OS"; dc[1].disabled = 0;
        dc[2].type = DC_BUTTON; dc[2].x = 130; dc[2].y = 52; dc[2].w = 80; dc[2].h = 24; dc[2].text = "OK"; dc[2].id = 1; dc[2].disabled = 0;
        dialog_show(&dlg, "About Task Manager", dc, 3, 340, 82, 1, 1);
        break;
    case T_END:
        if (tab == 0 && i < nwin) { app_log("[TASKS] end", winbuf + app_ids[i] * 25 + 1); app_window_cmd(app_ids[i], 2); str_copy(status_line, "Task ended."); }
        break;
    case T_MINIMIZE:
        if (tab == 0 && i < nwin && app_ids[i]) app_window_cmd(app_ids[i], 3);
        break;
    case T_MAXIMIZE:
        if (tab == 0 && i < nwin) app_window_cmd(app_ids[i], 4);
        break;
    case T_SWITCH:
        if (tab == 0 && i < nwin) app_window_cmd(app_ids[i], 1);
        break;
    case T_KILLVM:
        if (tab == 2 && i > 0 && vm_list[i * 4] == 1) { vm_kill(i); str_copy(status_line, "The VM was ended."); }
        break;
    case T_FOCUSVM:
        if (tab == 2 && i > 0 && vm_list[i * 4] == 1) {
            struct regs r;
            mem_set(&r, 0, sizeof r);
            r.ax = 0x44; r.bx = i; r.ds = r.es = app_seg();
            far_regs(vm_seg, vm_off, &r);
        }
        break;
    }
    refresh_all();
    m_view[2].flags = speed == 0 ? MI_CHECKED : 0;
    m_view[3].flags = speed == 1 ? MI_CHECKED : 0;
    m_view[4].flags = speed == 2 ? MI_CHECKED : 0;
    m_view[5].flags = speed == 3 ? MI_CHECKED : 0;
}
static int on_mouse(int kind, int x, int y)
{
    int sx = HOST.x + x, sy = HOST.y + TITLE_H + y, r, i, w, tx;
    layout();
    if (dlg.open) { if (dialog_mouse(&dlg, kind, sx, sy) >= 0) dlg.open = 0; return 1; }
    if (ctx.open) { r = popup_mouse(&ctx, kind, sx, sy); if (r >= 0) command(r); return 1; }
    r = menubar_mouse(&bar, kind, sx, sy);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    if (kind != MOUSE_DOWN && kind != MOUSE_RIGHT) return 0;
    if (kind == MOUSE_DOWN && sy >= tab_y && sy < tab_y + 24) {
        for (i = 0; i < 4; i++) { tx = tab_x(i, &w); if (sx >= tx && sx < tx + w) { tab = i; refresh_all(); log_tab(); return 1; } }
    }
    if (kind == MOUSE_DOWN && sy >= body_y + 2 && sy < body_y + 22 && tab == 1) {
        int c = sx < body_x + (int)((long)body_w * 45 / 100) ? 0 : sx < body_x + (int)((long)body_w * 62 / 100) ? 1 :
                sx < body_x + (int)((long)body_w * 82 / 100) ? 2 : 3;
        if (c == sort_col) sort_desc = !sort_desc; else { sort_col = c; sort_desc = c >= 2; }
        load_processes();
        return 1;
    }
    if (kind == MOUSE_RIGHT) {
        if (sy >= body_y + 24 && sy < body_y + body_h && sx >= body_x && sx < body_x + body_w) {
            i = top_row[tab] + (sy - body_y - 24) / 18;
            if (i < rows_count()) sel_row[tab] = i;
            if (tab == 0 && i < nwin) {
                int w = app_ids[i], mod = w == 8 || w == 9 || (w >= 12 && w <= 15);
                if (mod) m_app[2].flags &= ~MI_DISABLED; else m_app[2].flags |= MI_DISABLED;
                if (w) m_app[1].flags &= ~MI_DISABLED; else m_app[1].flags |= MI_DISABLED;
                popup_open(&ctx, m_app, 5, sx, sy, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
                app_log("[TASKS] menu", "application");
                return 1;
            }
            if (tab == 2 && i < rows_count()) {
                popup_open(&ctx, m_vm, 3, sx, sy, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
                app_log("[TASKS] menu", "virtual machine");
                return 1;
            }
        }
        popup_open(&ctx, m_rest, 2, sx, sy, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
        app_log("[TASKS] menu", "window");
        return 1;
    }
    if (sy >= body_y + 24 && sy < body_y + body_h && sx >= body_x && sx < body_x + body_w) {
        i = top_row[tab] + (sy - body_y - 24) / 18;
        if (i < rows_count()) {
            if (i == sel_row[tab] && tab == 0 && (unsigned)(HOST.ticks - click_tick) < 9) command(T_SWITCH);
            sel_row[tab] = i;
            click_tick = HOST.ticks;
        }
        return 1;
    }
    if (sy >= btn_y && sy < btn_y + 26) {
        if (tab == 0) {
            if (sx >= body_x + body_w - 300 && sx < body_x + body_w - 206) command(T_END);
            else if (sx >= body_x + body_w - 200 && sx < body_x + body_w - 106) command(T_SWITCH);
            else if (sx >= body_x + body_w - 100) command(T_NEW);
        } else if (tab == 2) {
            if (sx >= body_x + body_w - 220 && sx < body_x + body_w - 116) command(T_FOCUSVM);
            else if (sx >= body_x + body_w - 110) command(T_KILLVM);
        }
        return 1;
    }
    return 1;
}
static int on_key(int key)
{
    int s = KEY_SCAN(key), ch = KEY_CHAR(key), r;
    layout();
    if (dlg.open) { if (dialog_key(&dlg, key, HOST.shift) >= 0) dlg.open = 0; return 1; }
    if (ctx.open) { r = popup_key(&ctx, key); if (r >= 0) command(r); return 1; }
    r = menubar_key(&bar, key, HOST.shift);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    if (s == 0x0F || (!ch && (s == K_LEFT || s == K_RIGHT) && (HOST.shift & SH_CTRL))) {
        tab = (tab + ((s == K_LEFT || (HOST.shift & SH_SHIFT)) ? 3 : 1)) % 4;
        log_tab();
        return 1;
    }
    if (!ch || ch == 0xE0) {
        if (s == K_F5) { command(T_REFRESH); return 1; }
        if (s == K_UP && sel_row[tab] > 0) { sel_row[tab]--; return 1; }
        if (s == K_DOWN && sel_row[tab] + 1 < rows_count()) { sel_row[tab]++; return 1; }
        if (s == K_DEL) { command(tab == 2 ? T_KILLVM : T_END); return 1; }
        if (s == K_LEFT) { tab = (tab + 3) % 4; log_tab(); return 1; }
        if (s == K_RIGHT) { tab = (tab + 1) % 4; log_tab(); return 1; }
        return 0;
    }
    if (ch == 13) { command(tab == 2 ? T_FOCUSVM : T_SWITCH); return 1; }
    if (key == K_ESC) return 1;
    return 0;
}

static int tasks_event(int ev, int a, int b, int c);
int app_event(int ev, int a, int b, int c)
{
    int r, orig = ev;
    if (ev == EV_OPEN && a == 2) { dlg.win = 0; dialog_sync(&dlg); return 1; }
    r = dialog_pre(&dlg, WIN_TASKS, &ev, &a);
    if (r >= 0) return r;
    if (dialog_mine(&dlg) && ev == EV_PAINT) { paint_dialog(); return 0; }
    if (ev == EV_MOUSE && a == MOUSE_HOVER) {
        int sx = HOST.x + b, sy = HOST.y + TITLE_H + c;
        ui_dirty = 0;
        layout();
        if (dialog_mine(&dlg)) dialog_hover(&dlg, sx, sy);
        else if (!dlg.open && ctx.open) popup_mouse(&ctx, MOUSE_HOVER, sx, sy);
        else if (!dlg.open && menubar_open(&bar)) menubar_mouse(&bar, MOUSE_HOVER, sx, sy);
        return ui_dirty;
    }
    r = tasks_event(ev, a, b, c);
    r = dialog_post(&dlg, WIN_TASKS, ev, r);
    return orig == EV_CLOSE && ev != EV_CLOSE ? 0 : r;   /* a dialog's close box */
}
static int tasks_event(int ev, int a, int b, int c)
{
    static const unsigned periods[] = { 9, 18, 72, 0 };
    switch (ev) {
    case EV_OPEN:
        str_copy(app_title, "Task Manager");
        if (!last_tick) {
            HDR_WIDTH = HOST.screen_w - 60 > 620 ? 620 : HOST.screen_w - 60;
            HDR_HEIGHT = HOST.screen_h - 100 > 470 ? 470 : HOST.screen_h - 100;
            m_view[3].flags = MI_CHECKED;
        }
        if (!str_icmp(APP_ARG, "performance")) tab = 3;
        APP_ARG[0] = 0;
        last_tick = HOST.ticks | 1;
        refresh_all();
        sample();
        log_tab();
        return 1;
    case EV_PAINT: paint(); return 0;
    case EV_WHEEL:
        if (dlg.open || tab >= 2 || c < 0) return 0;
        top_row[tab] += a * 3;
        return 1;
    case EV_KEY: {
        int r = on_key(a), rows;
        layout(); rows = (body_h - 26) / 18;
        if (sel_row[tab] < top_row[tab]) top_row[tab] = sel_row[tab];
        if (sel_row[tab] >= top_row[tab] + rows) top_row[tab] = sel_row[tab] - rows + 1;
        return r;
    }
    case EV_MOUSE: return on_mouse(a, b, c);
    case EV_POLL:
        if (!periods[speed]) return 0;
        if ((unsigned)(HOST.ticks - last_tick) >= periods[speed]) {
            last_tick = HOST.ticks;
            sample();
            refresh_all();
            return 1;
        }
        return 0;
    case EV_CLOSE: return 0;
    case EV_SUSPEND: APP_ARG[0] = 0; return 0;
    }
    return 0;
}
