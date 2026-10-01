/* Recycle Bin: the items deleted in Files and on the desktop (C:\RECYCLED,
 * see sys.c), with Restore, Delete (for good), Empty Recycle Bin and
 * Properties, as the Windows 9x Recycle Bin window. */
#include "app.h"

#define MAX_ITEMS 120
struct bitem { int rec; u8 sel; char name[LFN_NAME]; };
static struct bitem items[MAX_ITEMS];
static struct binrec rec;                    /* the record being looked at */
static int nitems, cur = -1, anchor = -1, top;
static u32 total_bytes, bin_sig;
static unsigned poll_tick, last_click;
static u16 seen_changes;
static int last_i = -1, sort_col, sort_desc;
static char status[96], msg[320];

static const char *base_of(const char *p)
{
    const char *b = p;
    while (*p) { if (*p == '\\' || *p == ':') b = p + 1; p++; }
    return b;
}
static void dir_of(const char *p, char *out, int capacity)
{
    int n;
    str_ncopy(out, p, capacity);
    n = str_len(out);
    while (n > 0 && out[n - 1] != '\\') n--;
    if (n > 3) n--;
    out[n] = 0;
}
static int cmp(int a, int b)
{
    struct binrec ra, rb;
    char da[SYS_PATH], db[SYS_PATH];
    int r = 0;
    bin_get(items[a].rec, &ra);
    bin_get(items[b].rec, &rb);
    switch (sort_col) {
    case 1: dir_of(ra.orig, da, sizeof da); dir_of(rb.orig, db, sizeof db); r = str_icmp(da, db); break;
    case 2: r = ra.ddate != rb.ddate ? (ra.ddate < rb.ddate ? -1 : 1) : ra.dtime != rb.dtime ? (ra.dtime < rb.dtime ? -1 : 1) : 0; break;
    case 3: r = ra.size < rb.size ? -1 : ra.size > rb.size ? 1 : 0; break;
    }
    if (!r) r = str_icmp(items[a].name, items[b].name);
    return sort_desc ? -r : r;
}
static void sort_items(void)
{
    int i, j;
    struct bitem t;
    for (i = 1; i < nitems; i++) {
        for (j = i; j > 0 && cmp(j, j - 1) < 0; j--) {
            mem_copy(&t, &items[j], sizeof t);
            mem_copy(&items[j], &items[j - 1], sizeof t);
            mem_copy(&items[j - 1], &t, sizeof t);
        }
    }
}
static void load(void)
{
    int i, n = bin_count();
    char t[16];
    nitems = 0;
    total_bytes = 0;
    bin_sig = 0;
    for (i = 0; i < n && nitems < MAX_ITEMS; i++) {
        if (!bin_item(i, &rec)) continue;
        items[nitems].rec = i;
        items[nitems].sel = 0;
        str_ncopy(items[nitems].name, base_of(rec.orig), LFN_NAME);
        total_bytes += rec.size;
        bin_sig = bin_sig * 31 + i + rec.ddate + rec.dtime;
        nitems++;
    }
    bin_sig += n;
    sort_items();
    if (cur >= nitems) cur = nitems - 1;
    if (cur < 0 && nitems) cur = 0;
    anchor = cur;
    fmt_u32(t, (u32)nitems);
    app_log("[BIN] list", t);
}
static u32 signature(void)
{
    int i, n = bin_count();
    u32 s = 0;
    for (i = 0; i < n; i++) if (bin_item(i, &rec)) s = s * 31 + i + rec.ddate + rec.dtime;
    return s + n;
}
static int count_sel(void) { int i, n = 0; for (i = 0; i < nitems; i++) n += items[i].sel; return n; }
static void select_only(int i)
{
    int k;
    for (k = 0; k < nitems; k++) items[k].sel = 0;
    if (i >= 0 && i < nitems) items[i].sel = 1;
    cur = anchor = i;
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
#define ROW_H 18
static int X0, Y0, W, H, tb_y, lx, ly, lw, lh;
static void layout(void)
{
    X0 = HOST.x + 3; Y0 = HOST.y + TITLE_H; W = HOST.w - 6; H = HOST.h - TITLE_H - 4;
    tb_y = Y0 + MENUBAR_H + 2;
    lx = X0 + 2; ly = tb_y + 34; lw = W - 4 - 16; lh = Y0 + H - 22 - ly;
}
static int rows_visible(void) { int r = (lh - 24) / ROW_H; return r < 1 ? 1 : r; }
static int col(int percent) { return lx + (int)((long)lw * percent / 100); }
static void ensure_visible(void)
{
    if (cur < top) top = cur;
    if (cur >= top + rows_visible()) top = cur - rows_visible() + 1;
    if (top < 0) top = 0;
}

/* ------------------------------------------------------------------ */
/* Menus, toolbar                                                      */
enum { C_RESTORE = 1, C_DELETE, C_EMPTY, C_PROPS, C_CLOSE, C_SELALL, C_INVERT, C_REFRESH,
       C_ABOUT, C_SORT0, C_SORT1, C_SORT2, C_SORT3 };
static struct menu_item m_file[] = {
    { "R&estore", 0, C_RESTORE, 0 }, { "&Delete", "Del", C_DELETE, 0 }, { "", 0, 0, MI_SEP },
    { "Empty Recycle &Bin", 0, C_EMPTY, 0 }, { "", 0, 0, MI_SEP },
    { "P&roperties", "Alt+Enter", C_PROPS, 0 }, { "", 0, 0, MI_SEP }, { "&Close", "Alt+F4", C_CLOSE, 0 } };
static struct menu_item m_edit[] = {
    { "Select &All", "Ctrl+A", C_SELALL, 0 }, { "&Invert Selection", 0, C_INVERT, 0 } };
static struct menu_item m_view[] = {
    { "Sort by &Name", 0, C_SORT0, 0 }, { "Sort by &Original Location", 0, C_SORT1, 0 },
    { "Sort by Date &Deleted", 0, C_SORT2, 0 }, { "Sort by &Size", 0, C_SORT3, 0 },
    { "", 0, 0, MI_SEP }, { "&Refresh", "F5", C_REFRESH, 0 } };
static struct menu_item m_help[] = { { "&About Recycle Bin", 0, C_ABOUT, 0 } };
static struct menu menus[] = {
    { "&File", m_file, 8 }, { "&Edit", m_edit, 2 }, { "&View", m_view, 6 }, { "&Help", m_help, 1 } };
static struct menubar bar = { menus, 4, 0, 0, 0, -1, 0 };
static struct menu_item m_item[] = {
    { "R&estore", 0, C_RESTORE, 0 }, { "", 0, 0, MI_SEP }, { "&Delete", 0, C_DELETE, 0 },
    { "", 0, 0, MI_SEP }, { "P&roperties", 0, C_PROPS, 0 } };
static struct menu_item m_back[] = {
    { "Empty Recycle &Bin", 0, C_EMPTY, 0 }, { "", 0, 0, MI_SEP }, { "&Refresh", 0, C_REFRESH, 0 },
    { "Select &All", 0, C_SELALL, 0 } };
static struct popup ctx;
struct tbdef { const char *label; int w, cmd; };
static struct tbdef tb[] = {
    { "Restore", 70, C_RESTORE }, { "Delete", 60, C_DELETE }, { "Empty Recycle Bin", 132, C_EMPTY },
    { "Properties", 80, C_PROPS }, { "Refresh", 64, C_REFRESH } };
#define TB_COUNT 5
static int tb_hot = -1;
static void flag(struct menu_item *it, int f, int on) { if (on) it->flags |= f; else it->flags &= ~f; }
static int enabled(int cmd)
{
    if (cmd == C_RESTORE || cmd == C_DELETE || cmd == C_PROPS) return cur >= 0 && nitems > 0;
    if (cmd == C_EMPTY) return nitems > 0;
    return 1;
}
static void update_menus(void)
{
    flag(&m_file[0], MI_DISABLED, !enabled(C_RESTORE)); flag(&m_file[1], MI_DISABLED, !enabled(C_DELETE));
    flag(&m_file[3], MI_DISABLED, !enabled(C_EMPTY)); flag(&m_file[5], MI_DISABLED, !enabled(C_PROPS));
    flag(&m_back[0], MI_DISABLED, !enabled(C_EMPTY));
    flag(&m_view[0], MI_CHECKED, sort_col == 0); flag(&m_view[1], MI_CHECKED, sort_col == 1);
    flag(&m_view[2], MI_CHECKED, sort_col == 2); flag(&m_view[3], MI_CHECKED, sort_col == 3);
}
static int tb_at(int sx, int sy)
{
    int x = X0 + 4, i;
    if (sy < tb_y || sy >= tb_y + 28) return -1;
    for (i = 0; i < TB_COUNT; i++) {
        if (sx >= x && sx < x + tb[i].w) return i;
        x += tb[i].w + 2;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Dialogs                                                             */
enum { D_NONE, D_MSG, D_DELETE, D_EMPTY, D_PROPS, D_ABOUT };
static struct dialog dlg;
static int dlg_kind;
static struct dctl dc[10];
static char dl[6][80];
static void ctl(int i, int type, int x, int y, int w, int h, const char *text, int id)
{
    mem_set(&dc[i], 0, sizeof dc[i]);
    dc[i].type = type; dc[i].x = x; dc[i].y = y; dc[i].w = w; dc[i].h = h;
    dc[i].text = text; dc[i].id = id;
}
static void message(const char *title, const char *text)
{
    msgbox(&dlg, title, text, "OK");
    dlg_kind = D_MSG;
    app_log("[BIN] message", text);
}
static void ask_delete(void)
{
    int n = count_sel();
    char t[12];
    if (!nitems || cur < 0) return;
    if (!n) { select_only(cur); n = 1; }
    if (n == 1) {
        int i;
        for (i = 0; i < nitems; i++) if (items[i].sel) break;
        str_copy(msg, "Are you sure you want to delete '");
        str_cat(msg, items[i].name);
        str_cat(msg, "'?\nIt will be deleted permanently.");
    } else {
        fmt_u32(t, (u32)n);
        str_copy(msg, "Are you sure you want to delete these ");
        str_cat(msg, t);
        str_cat(msg, " items?\nThey will be deleted permanently.");
    }
    msgbox(&dlg, n == 1 ? "Confirm File Delete" : "Confirm Multiple File Delete", msg, "Yes|No");
    dlg_kind = D_DELETE;
    app_log("[BIN] confirm", msg);
}
static void ask_empty(void)
{
    char t[12];
    if (!nitems) return;
    fmt_u32(t, (u32)nitems);
    str_copy(msg, "Are you sure you want to delete all of the ");
    str_cat(msg, t);
    str_cat(msg, " items\nin the Recycle Bin?");
    msgbox(&dlg, "Confirm Multiple File Delete", msg, "Yes|No");
    dlg_kind = D_EMPTY;
    app_log("[BIN] confirm", msg);
}
static void properties(void)
{
    char t[24];
    if (cur < 0 || !bin_get(items[cur].rec, &rec)) return;
    str_ncopy(dl[0], items[cur].name, sizeof dl[0]);
    str_copy(dl[1], "Origin: "); dir_of(rec.orig, dl[1] + 8, sizeof dl[1] - 8);
    str_copy(dl[2], "Type: "); str_cat(dl[2], (rec.attr & A_DIR) ? "File Folder" : "File");
    str_copy(dl[3], "Size: "); fmt_size(t, rec.size); str_cat(dl[3], t);
    str_cat(dl[3], " ("); fmt_u32_group(t, rec.size); str_cat(dl[3], t); str_cat(dl[3], " bytes)");
    str_copy(dl[4], "Deleted: "); fmt_date(t, rec.ddate); str_cat(dl[4], t); str_cat(dl[4], "  ");
    fmt_time(t, rec.dtime); str_cat(dl[4], t);
    str_copy(dl[5], "Modified: ");
    if (rec.date) { fmt_date(t, rec.date); str_cat(dl[5], t); str_cat(dl[5], "  "); fmt_time(t, rec.time); str_cat(dl[5], t); }
    else str_cat(dl[5], "-");
    ctl(0, DC_LABEL, 52, 8, 0, 16, dl[0], 0);
    ctl(1, DC_LABEL, 0, 44, 0, 16, dl[1], 0);
    ctl(2, DC_LABEL, 0, 66, 0, 16, dl[2], 0);
    ctl(3, DC_LABEL, 0, 88, 0, 16, dl[3], 0);
    ctl(4, DC_LABEL, 0, 110, 0, 16, dl[4], 0);
    ctl(5, DC_LABEL, 0, 132, 0, 16, dl[5], 0);
    ctl(6, DC_BUTTON, 150, 164, 84, 26, "&Restore", 2);
    ctl(7, DC_BUTTON, 240, 164, 84, 26, "OK", 1);
    str_copy(msg, items[cur].name);
    str_cat(msg, " Properties");
    dialog_show(&dlg, msg, dc, 8, 340, 196, 1, 1);
    dlg_kind = D_PROPS;
}
static void restore(void)
{
    int i, done = 0, r = 0;
    char t[8];
    if (!nitems || cur < 0) return;
    if (!count_sel()) select_only(cur);
    for (i = 0; i < nitems; i++) {
        if (!items[i].sel) continue;
        bin_get(items[i].rec, &rec);
        r = bin_restore(items[i].rec);
        if (r < 0) {
            str_copy(msg, "Cannot restore '");
            str_cat(msg, items[i].name);
            str_cat(msg, r == -80 ? "':\nan item of that name is already in\n" : "':\n");
            if (r == -80) dir_of(rec.orig, msg + str_len(msg), sizeof msg - str_len(msg));
            else str_cat(msg, dos_error_text(r));
            break;
        }
        app_log("[BIN] restored", rec.orig);
        done++;
    }
    fmt_u32(t, (u32)done);
    str_copy(status, t);
    str_cat(status, " item(s) restored.");
    load();
    ui_repaint_win(WIN_DESKTOP);
    if (r < 0) { app_sound(5); message("Restore", msg); }
}
static void purge(void)
{
    int i, done = 0, r = 0;
    char t[8];
    for (i = 0; i < nitems; i++) {
        if (!items[i].sel) continue;
        r = bin_purge(items[i].rec);
        if (r < 0) break;
        done++;
    }
    fmt_u32(t, (u32)done);
    app_log("[BIN] deleted", t);
    str_copy(status, t);
    str_cat(status, " item(s) deleted.");
    load();
    ui_repaint_win(WIN_DESKTOP);
    if (r < 0) message("Delete", dos_error_text(r));
}
static void dialog_result(int r)
{
    int kind = dlg_kind;
    if (r < 0) return;
    dlg_kind = D_NONE;
    if (kind == D_DELETE && r == MB_YES) purge();
    if (kind == D_EMPTY && r == MB_YES) {
        bin_empty();
        app_log("[BIN] emptied", 0);
        str_copy(status, "The Recycle Bin is empty.");
        load();
        ui_repaint_win(WIN_DESKTOP);
    }
    if (kind == D_PROPS && r == 2) restore();
}
static void command(int id)
{
    int i;
    status[0] = 0;
    switch (id) {
    case C_RESTORE: restore(); break;
    case C_DELETE: ask_delete(); break;
    case C_EMPTY: ask_empty(); break;
    case C_PROPS: properties(); break;
    case C_CLOSE: app_close(); break;
    case C_SELALL: for (i = 0; i < nitems; i++) items[i].sel = 1; break;
    case C_INVERT: for (i = 0; i < nitems; i++) items[i].sel = !items[i].sel; break;
    case C_REFRESH: load(); break;
    case C_ABOUT:
        ctl(0, DC_LABEL, 52, 4, 0, 16, "CiukiOS Recycle Bin", 0);
        ctl(1, DC_LABEL, 52, 24, 0, 16, "Deleted items stay here until it is emptied.", 0);
        ctl(2, DC_LABEL, 52, 44, 0, 16, "A modern Retro OS", 0);
        ctl(3, DC_BUTTON, 170, 76, 84, 26, "OK", 1);
        dialog_show(&dlg, "About Recycle Bin", dc, 4, 424, 108, 1, 1);
        dlg_kind = D_ABOUT;
        break;
    case C_SORT0: case C_SORT1: case C_SORT2: case C_SORT3:
        sort_col = id - C_SORT0; sort_desc = 0; sort_items(); break;
    }
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
static void paint(void)
{
    static const char *names[] = { "Name", "Original Location", "Date Deleted", "Size" };
    int i, x, cx[5];
    char t[80];
    layout();
    update_menus();
    bar.x = X0; bar.y = Y0; bar.w = W;
    ui_rect(X0, tb_y - 1, W, ly - tb_y + 1, C_FACE);
    x = X0 + 4;
    for (i = 0; i < TB_COUNT; i++) {
        int on = enabled(tb[i].cmd);
        ui_bevel(x, tb_y, tb[i].w, 28, i == tb_hot && on ? C_LIGHT : C_FACE);
        ui_text(x + (tb[i].w - ui_measure(tb[i].label)) / 2, tb_y + 6, tb[i].label, on ? C_INK : C_SHADOW);
        x += tb[i].w + 2;
    }
    ui_inset(lx, ly, lw, lh);
    ui_rect(lx + 2, ly + 2, lw - 4, lh - 4, C_PAPER);
    cx[0] = lx + 2; cx[1] = col(30); cx[2] = col(62); cx[3] = col(84); cx[4] = lx + lw - 2;
    for (i = 0; i < 4; i++) {
        ui_bevel(cx[i], ly + 2, cx[i + 1] - cx[i], 20, C_FACE);
        ui_text(cx[i] + 6, ly + 4, names[i], C_INK);
        if (sort_col == i) {
            int ax = cx[i + 1] - 14, k;
            for (k = 0; k < 4; k++) ui_rect(ax + (sort_desc ? k : 3 - k), ly + 8 + k, 1 + 2 * (sort_desc ? 3 - k : k), 1, C_SHADOW);
        }
    }
    ensure_visible();
    for (i = top; i < nitems && i < top + rows_visible(); i++) {
        int y = ly + 24 + (i - top) * ROW_H, fg = C_INK;
        bin_get(items[i].rec, &rec);
        if (items[i].sel) { ui_rect(cx[0] + 22, y, cx[1] - cx[0] - 26, ROW_H, C_TITLE); fg = C_PAPER; }
        if (rec.attr & A_DIR) { ui_rect(cx[0] + 4, y + 5, 14, 9, C_YELLOW); ui_rect(cx[0] + 4, y + 3, 6, 2, C_BROWN); }
        else { ui_rect(cx[0] + 6, y + 2, 10, 14, C_PAPER); ui_rect(cx[0] + 6, y + 2, 10, 1, C_SHADOW);
               ui_rect(cx[0] + 6, y + 15, 10, 1, C_SHADOW); ui_rect(cx[0] + 6, y + 2, 1, 14, C_SHADOW);
               ui_rect(cx[0] + 15, y + 2, 1, 14, C_SHADOW); }
        draw_frame_text(cx[0] + 26, y + 1, cx[1] - cx[0] - 30, items[i].name, fg);
        dir_of(rec.orig, t, sizeof t);
        draw_frame_text(cx[1] + 6, y + 1, cx[2] - cx[1] - 10, t, C_INK);
        fmt_date(t, rec.ddate); str_cat(t, " "); fmt_time(t + str_len(t), rec.dtime);
        draw_frame_text(cx[2] + 6, y + 1, cx[3] - cx[2] - 10, t, C_INK);
        fmt_size(t, rec.size);
        ui_text(cx[4] - 8 - ui_measure(t), y + 1, t, C_INK);
        if (i == cur && HOST.active) draw_focus(cx[0] + 22, y, cx[1] - cx[0] - 26, ROW_H);
    }
    if (!nitems) ui_text(lx + 20, ly + 32, "The Recycle Bin is empty.", C_SHADOW);
    draw_scroll(lx + lw, ly, lh, top, nitems, rows_visible());
    {
        int sy = Y0 + H - 20, sel = count_sel();
        ui_rect(X0, sy - 1, W, 21, C_FACE);
        if (status[0]) str_copy(t, status);
        else {
            fmt_u32(t, (u32)(sel ? sel : nitems));
            str_cat(t, sel ? " object(s) selected" : " object(s)");
            str_cat(t, "    ");
            fmt_size(t + str_len(t), total_bytes);
        }
        ui_inset(X0 + 2, sy, W - 4, 19);
        ui_text(X0 + 8, sy + 1, t, C_INK);
    }
    menubar_draw(&bar);
    if (ctx.open) popup_draw(&ctx);
}
static void paint_dialog(void)
{
    dialog_draw(&dlg);
    if (dlg_kind == D_PROPS || dlg_kind == D_ABOUT)
        ui_icon(dlg.x + 6, dlg.y + DIALOG_TITLE_H + 2, dlg_kind == D_ABOUT ? ICON_BIN_FULL : ICON_TEXT);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
static int on_mouse(int kind, int sx, int sy)
{
    int r, i;
    layout();
    if (dlg.open) { r = dialog_mouse(&dlg, kind, sx, sy); if (r >= 0) dialog_result(r); return 1; }
    if (ctx.open) { r = popup_mouse(&ctx, kind, sx, sy); if (r >= 0) command(r); return 1; }
    r = menubar_mouse(&bar, kind, sx, sy);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    if (kind == MOUSE_DOWN) {
        i = tb_at(sx, sy);
        if (i >= 0) { if (enabled(tb[i].cmd)) command(tb[i].cmd); return 1; }
        if (sx >= lx + lw && sx < lx + lw + 16 && sy >= ly && sy < ly + lh) {
            long p = scroll_hit(lx + lw, ly, lh, sy, nitems, rows_visible());
            if (p == -1 && top > 0) top--;
            else if (p == -2 && top + rows_visible() < nitems) top++;
            else if (p >= 16) top = (int)(p - 16);
            return 1;
        }
        if (sy >= ly + 2 && sy < ly + 22 && sx >= lx && sx < lx + lw) {
            int c = sx < col(30) ? 0 : sx < col(62) ? 1 : sx < col(84) ? 2 : 3;
            if (c == sort_col) sort_desc = !sort_desc; else { sort_col = c; sort_desc = c >= 2; }
            sort_items();
            return 1;
        }
    }
    if (sx >= lx && sx < lx + lw && sy >= ly + 24 && sy < ly + lh) {
        i = top + (sy - ly - 24) / ROW_H;
        if (i >= nitems) i = -1;
        if (kind == MOUSE_DOWN) {
            status[0] = 0;
            if (i >= 0 && i == last_i && (unsigned)(HOST.ticks - last_click) < (unsigned)HOST.dblclick) {
                last_i = -1; select_only(i); properties(); return 1;
            }
            last_i = i; last_click = HOST.ticks;
            if (i < 0) { select_only(-1); cur = nitems ? 0 : -1; return 1; }
            if (HOST.shift & SH_CTRL) { items[i].sel = !items[i].sel; cur = anchor = i; }
            else if (HOST.shift & SH_SHIFT) {
                int k, a = anchor < 0 ? i : anchor;
                for (k = 0; k < nitems; k++) items[k].sel = k >= (a < i ? a : i) && k <= (a < i ? i : a);
                cur = i;
            } else select_only(i);
            return 1;
        }
        if (kind == MOUSE_RIGHT) {
            update_menus();
            if (i >= 0) {
                if (!items[i].sel) select_only(i);
                cur = i;
                update_menus();
                popup_open(&ctx, m_item, 5, sx, sy, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
                app_log("[BIN] menu", "item");
            } else {
                popup_open(&ctx, m_back, 4, sx, sy, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
                app_log("[BIN] menu", "background");
            }
            return 1;
        }
    }
    return kind == MOUSE_DOWN || kind == MOUSE_RIGHT;
}
static int on_key(int key)
{
    int s = KEY_SCAN(key), ch = KEY_CHAR(key), r, letter;
    layout();
    if (dlg.open) { r = dialog_key(&dlg, key, HOST.shift); if (r >= 0) dialog_result(r); return 1; }
    if (ctx.open) { r = popup_key(&ctx, key); if (r >= 0) command(r); return 1; }
    r = menubar_key(&bar, key, HOST.shift);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    letter = (HOST.shift & SH_CTRL) ? key_ctrl_letter(key) : 0;
    if (letter == 'A') { command(C_SELALL); return 1; }
    if (key == 0x5D00) {
        update_menus();
        popup_open(&ctx, cur >= 0 ? m_item : m_back, cur >= 0 ? 5 : 4, lx + 40, ly + 40, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
        return 1;
    }
    if (s == 0xA6 || (s == 0x1C && (HOST.shift & SH_ALT))) { command(C_PROPS); return 1; }
    if (ch == 13) { command(C_PROPS); return 1; }
    if (!ch || ch == 0xE0) {
        switch (s) {
        case K_DEL: command(C_DELETE); return 1;
        case K_F5: command(C_REFRESH); return 1;
        case K_UP: if (cur > 0) select_only(cur - 1); return 1;
        case K_DOWN: if (cur + 1 < nitems) select_only(cur + 1); return 1;
        case K_HOME: if (nitems) select_only(0); return 1;
        case K_END: if (nitems) select_only(nitems - 1); return 1;
        }
    }
    return 0;
}
static int bin_event(int ev, int a, int b, int c)
{
    switch (ev) {
    case EV_OPEN:
        str_copy(app_title, "Recycle Bin");
        if (!HDR_WIDTH) {
            HDR_WIDTH = HOST.screen_w - 80 > 640 ? 640 : HOST.screen_w - 80;
            HDR_HEIGHT = HOST.screen_h - 140 > 400 ? 400 : HOST.screen_h - 140;
        }
        APP_ARG[0] = 0;
        load();
        return 1;
    case EV_PAINT: paint(); return 0;
    case EV_KEY: return on_key(a);
    case EV_MOUSE: return on_mouse(a, HOST.x + b, HOST.y + TITLE_H + c);
    case EV_POLL:
        if ((unsigned)(HOST.ticks - poll_tick) >= 18 && !dlg.open && fs_changes() != seen_changes) {
            poll_tick = HOST.ticks;
            seen_changes = fs_changes();
            if (signature() != bin_sig) { load(); return 1; }
        }
        return 0;
    }
    return 0;
}
int app_event(int ev, int a, int b, int c)
{
    int r, orig = ev;
    if (ev == EV_OPEN && a == 2) { dlg.win = 0; dialog_sync(&dlg); return 1; }
    r = dialog_pre(&dlg, WIN_RECYCLE, &ev, &a);
    if (r >= 0) return r;
    if (dialog_mine(&dlg) && ev == EV_PAINT) { paint_dialog(); return 0; }
    if (ev == EV_MOUSE && a == MOUSE_HOVER) {
        int sx = HOST.x + b, sy = HOST.y + TITLE_H + c, i;
        layout();
        ui_dirty = 0;
        if (dialog_mine(&dlg)) dialog_hover(&dlg, sx, sy);
        else if (dlg.open) return 0;
        else if (ctx.open) popup_mouse(&ctx, MOUSE_HOVER, sx, sy);
        else if (menubar_open(&bar)) menubar_mouse(&bar, MOUSE_HOVER, sx, sy);
        else {
            i = tb_at(sx, sy);
            if (i >= 0 && !enabled(tb[i].cmd)) i = -1;
            if (i != tb_hot) { tb_hot = i; ui_dirty = 1; }
        }
        return ui_dirty;
    }
    r = bin_event(ev, a, b, c);
    r = dialog_post(&dlg, WIN_RECYCLE, ev, r);
    return orig == EV_CLOSE && ev != EV_CLOSE ? 0 : r;
}
