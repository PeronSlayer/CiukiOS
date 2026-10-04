/* CiukNote for the CiukiOS desktop: a classic text editor (menus,
 * shortcuts, dialogs and behaviour) over DOS files. The text lives in its own
 * block of at most 60 KB (Windows 9x CiukNote stopped at 64 KB), with CR LF
 * line ends. Undo is one level and toggles, as in the original. */
#include "app.h"

#define TEXT_MAX  0xF000u
#define CELL      9
#define LINE_H    16
#define TAB       8
#define MAX_ROWS  64
#define PATH_MAX  80
#define NONE      0xFFFFu

typedef char __far *fptr;
static u16 tseg, useg, cseg;
static u16 tlen, ulen, clen;
static u16 caret, anchor, top, hcol, goal = NONE;
static u16 ucaret, uanchor;
static int undo_valid, typing_group;
static int wrap, status_bar = 1, bold;
static char np_font[13];                     /* Format > Font: "" the desktop font */
static int text_style(void) { return (bold ? BOLD : 0) | (np_font[0] ? ALTFONT : 0); }
static int dirty, closing, dragging;
static char path[PATH_MAX];
static char find_text[64], replace_text[64];
static int find_case, find_up;
static unsigned last_click_tick;
static u16 last_click_off = NONE;

/* Page setup: margins in characters (left, right) and lines (top, bottom). */
static char header_text[64] = "&f", footer_text[64] = "Page &p";
static char margin_l[6] = "5", margin_r[6] = "5", margin_t[6] = "3", margin_b[6] = "3";
static int landscape;

static fptr TP(u16 off) { return (fptr)(((u32)tseg << 16) | off); }
#define CH(o) (*TP(o))

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
static int ex, ey, ew, eh, rows, cols;
static void layout(void)
{
    ex = HOST.x + 4;
    ey = HOST.y + TITLE_H + MENUBAR_H + 1;
    ew = HOST.w - 8 - 16;
    eh = HOST.h - TITLE_H - MENUBAR_H - 6 - (wrap ? 0 : 16) - ((status_bar && !wrap) ? 20 : 0);
    rows = (eh - 4) / LINE_H;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    if (rows < 1) rows = 1;
    cols = (ew - 8) / CELL;
    if (cols < 8) cols = 8;
    if (cols > 150) cols = 150;
}

/* ------------------------------------------------------------------ */
/* Rows: a row is a line, or its wrapped part when Word Wrap is on.    */
static u16 line_start(u16 off)
{
    while (off > 0 && CH(off - 1) != '\n') off--;
    return off;
}
static u16 line_end(u16 off)
{
    while (off < tlen && CH(off) != '\r' && CH(off) != '\n') off++;
    return off;
}
static u16 after_eol(u16 off)
{
    if (off < tlen && CH(off) == '\r') off++;
    if (off < tlen && CH(off) == '\n') off++;
    return off;
}
static int char_w(u16 i, int c) { return CH(i) == '\t' ? TAB - c % TAB : 1; }

/* End of the row starting at s (before the line end). */
static u16 row_end(u16 s)
{
    u16 e = line_end(s), i, brk = NONE;
    int c = 0;
    if (!wrap) return e;
    for (i = s; i < e; i++) {
        c += char_w(i, c);
        if (c > cols) {
            if (brk != NONE && brk > s) return brk;
            return i > s ? i : s + 1;
        }
        if (CH(i) == ' ' || CH(i) == '\t') brk = i + 1;
    }
    return e;
}
static u16 row_next(u16 s)          /* NONE when s is the last row */
{
    u16 e = row_end(s);
    if (e < line_end(s)) return e;
    if (e >= tlen) return NONE;
    return after_eol(e);
}
static u16 row_of(u16 off)
{
    u16 s = line_start(off), n;
    for (;;) {
        n = row_next(s);
        if (n == NONE || n > off) return s;
        s = n;
    }
}
static u16 row_prev(u16 s)
{
    u16 p;
    if (s == 0) return NONE;
    if (s == line_start(s)) {
        p = s;
        if (p > 0 && CH(p - 1) == '\n') p--;
        if (p > 0 && CH(p - 1) == '\r') p--;
        return row_of(p);
    }
    return row_of(s - 1);
}
static int col_of(u16 s, u16 off)
{
    int c = 0;
    u16 i;
    for (i = s; i < off; i++) c += char_w(i, c);
    return c;
}
static u16 off_at_col(u16 s, int col, int round)
{
    u16 e = row_end(s), i = s;
    int c = 0, w;
    while (i < e) {
        w = char_w(i, c);
        if (c + w > col) {
            if (round && col - c >= (w + 1) / 2) i++;
            break;
        }
        c += w;
        i++;
    }
    return i;
}
static u16 row_index(u16 s)          /* rows before the row at s */
{
    u16 r = 0, p = 0, n;
    while (p < s) {
        n = row_next(p);
        if (n == NONE || n > s) break;
        p = n;
        r++;
    }
    return r;
}
static u16 row_count(void) { return row_index(tlen) + 1; }
static u16 row_at_index(u16 index)
{
    u16 p = 0, n;
    while (index--) {
        n = row_next(p);
        if (n == NONE) break;
        p = n;
    }
    return p;
}
static int longest_line(void)
{
    u16 p = 0;
    int best = 0, c;
    while (p <= tlen) {
        u16 e = line_end(p);
        c = col_of(p, e);
        if (c > best) best = c;
        if (e >= tlen) break;
        p = after_eol(e);
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* Selection, editing and undo                                         */
static u16 sel_a(void) { return caret < anchor ? caret : anchor; }
static u16 sel_b(void) { return caret < anchor ? anchor : caret; }
static int has_sel(void) { return caret != anchor; }

static void snapshot(void)
{
    if (!useg) useg = dos_alloc(0x1000);
    if (!useg) { undo_valid = 0; return; }
    far_copy(useg, 0, tseg, 0, tlen);
    ulen = tlen;
    ucaret = caret;
    uanchor = anchor;
    undo_valid = 1;
}
static void begin_edit(int typing)
{
    if (!(typing && typing_group)) snapshot();
    typing_group = typing;
    dirty = 1;
    goal = NONE;
}
static void undo(void)
{
    u16 n = tlen > ulen ? tlen : ulen, i, t;
    fptr a, b;
    if (!undo_valid) return;
    a = TP(0);
    b = (fptr)((u32)useg << 16);
    for (i = 0; i < n; i++) { char c = a[i]; a[i] = b[i]; b[i] = c; }
    t = tlen; tlen = ulen; ulen = t;
    t = caret; caret = ucaret; ucaret = t;
    t = anchor; anchor = uanchor; uanchor = t;
    app_log("[CIUKNOTE] undo", 0);
    if (caret > tlen) caret = tlen;
    if (anchor > tlen) anchor = tlen;
    typing_group = 0;
    dirty = 1;
    goal = NONE;
}
static void delete_range(u16 at, u16 n)
{
    if (!n) return;
    far_copy(tseg, at, tseg, at + n, tlen - at - n);
    tlen -= n;
}
static int insert_far(u16 at, u16 seg, u16 off, u16 n)
{
    if ((u32)tlen + n > TEXT_MAX) { app_sound(5); return 0; }
    far_copy(tseg, at + n, tseg, at, tlen - at);
    far_copy(tseg, at, seg, off, n);
    tlen += n;
    return 1;
}
static int insert_text(u16 at, const char *s, u16 n) { return insert_far(at, app_seg(), (u16)s, n); }
static void delete_selection(void)
{
    u16 a = sel_a();
    delete_range(a, sel_b() - a);
    caret = anchor = a;
}
/* Replace the selection with n bytes (a new undo step unless typing). */
static void replace_with(const char *s, u16 n, int typing)
{
    begin_edit(typing);
    if (has_sel()) delete_selection();
    if (insert_text(caret, s, n)) caret += n;
    anchor = caret;
}
static void copy_selection(void)
{
    u16 a = sel_a(), n = sel_b() - a;
    if (!n) return;
    if (!cseg) cseg = dos_alloc(0x1000);
    if (!cseg) { app_sound(5); return; }
    far_copy(cseg, 0, tseg, a, n);
    clen = n;
}
static void paste(void)
{
    if (!cseg || !clen) return;
    begin_edit(0);
    if (has_sel()) delete_selection();
    if (insert_far(caret, cseg, 0, clen)) caret += clen;
    anchor = caret;
}

/* ------------------------------------------------------------------ */
/* Caret movement                                                      */
static int is_word(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || (u8)c >= 128; }
static u16 left_of(u16 o)
{
    if (!o) return 0;
    o--;
    if (o > 0 && CH(o) == '\n' && CH(o - 1) == '\r') o--;
    return o;
}
static u16 right_of(u16 o)
{
    if (o >= tlen) return tlen;
    if (CH(o) == '\r' && o + 1 < tlen && CH(o + 1) == '\n') return o + 2;
    return o + 1;
}
static u16 word_left(u16 o)
{
    while (o > 0 && !is_word(CH(o - 1))) o = left_of(o);
    while (o > 0 && is_word(CH(o - 1))) o--;
    return o;
}
static u16 word_right(u16 o)
{
    while (o < tlen && is_word(CH(o))) o++;
    while (o < tlen && !is_word(CH(o))) o = right_of(o);
    return o;
}
static void ensure_visible(void)
{
    u16 r = row_of(caret), p;
    int i, c;
    layout();
    if (top > tlen) top = 0;
    top = row_of(top);
    if (r < top) top = r;
    else {
        p = top;
        for (i = 0; i < rows - 1; i++) {
            if (p == r) break;
            p = row_next(p);
            if (p == NONE) break;
        }
        if (p != r) {
            top = r;
            for (i = 0; i < rows - 1; i++) {
                p = row_prev(top);
                if (p == NONE) break;
                top = p;
            }
        }
    }
    if (!wrap) {
        c = col_of(r, caret);
        if (c < (int)hcol) hcol = c > 8 ? c - 8 : 0;
        if (c >= (int)hcol + cols) hcol = c - cols + 8;
    } else hcol = 0;
}
static void move_caret(u16 to, int extend)
{
    caret = to;
    if (!extend) anchor = to;
    typing_group = 0;
}
static void vertical(int rows_delta, int extend)
{
    u16 r = row_of(caret), n;
    if (goal == NONE) goal = col_of(r, caret);
    while (rows_delta < 0) {
        n = row_prev(r);
        if (n == NONE) break;
        r = n;
        rows_delta++;
    }
    while (rows_delta > 0) {
        n = row_next(r);
        if (n == NONE) break;
        r = n;
        rows_delta--;
    }
    {
        u16 keep = goal;
        move_caret(off_at_col(r, goal, 0), extend);
        goal = keep;
    }
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
static const char *file_name(const char *p)
{
    const char *n = p;
    while (*p) { if (*p == '\\' || *p == ':') n = p + 1; p++; }
    return n;
}
static void update_title(void)
{
    str_copy(app_title, path[0] ? file_name(path) : "Untitled");
    str_cat(app_title, " - CiukNote");
}
static void time_date(char *out)       /* "6:34 PM 9/29/2026" */
{
    int h, m, s, y, mo, d;
    char t[8];
    dos_get_time(&h, &m, &s);
    dos_get_date(&y, &mo, &d, 0);
    fmt_u32(out, (u32)(h % 12 == 0 ? 12 : h % 12));
    str_cat(out, ":");
    fmt_2(t, m); str_cat(out, t);
    str_cat(out, h < 12 ? " AM " : " PM ");
    fmt_u32(t, (u32)mo); str_cat(out, t); str_cat(out, "/");
    fmt_u32(t, (u32)d); str_cat(out, t); str_cat(out, "/");
    fmt_u32(t, (u32)y); str_cat(out, t);
}
static void new_document(void)
{
    tlen = 0; caret = anchor = top = hcol = 0; goal = NONE;
    undo_valid = 0; typing_group = 0; dirty = 0;
    path[0] = 0;
    update_title();
}
/* 0 ok, else a DOS error, or 1 when the file is too large. */
static int load_file(const char *p)
{
    char buf[512];
    int h = dos_open(p, 0), n, i;
    u16 len = 0;
    char prev = 0;
    if (h < 0) return h;
    for (;;) {
        n = dos_read(h, buf, sizeof buf);
        if (n < 0) { dos_close(h); return n; }
        if (n == 0) break;
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' && prev != '\r') {
                if (len >= TEXT_MAX) { dos_close(h); new_document(); return 1; }
                *TP(len++) = '\r';
            }
            if (len >= TEXT_MAX) { dos_close(h); new_document(); return 1; }
            *TP(len++) = c;
            prev = c;
        }
    }
    dos_close(h);
    if (len && CH(len - 1) == 0x1A) len--;
    tlen = len;
    caret = anchor = top = hcol = 0; goal = NONE;
    undo_valid = 0; typing_group = 0; dirty = 0;
    str_ncopy(path, p, PATH_MAX);
    update_title();
    app_log("[CIUKNOTE] opened", p);
    /* .LOG: each opening appends the time and date (as in Windows). */
    if (tlen >= 4 && CH(0) == '.' && CH(1) == 'L' && CH(2) == 'O' && CH(3) == 'G') {
        char stamp[40];
        str_copy(stamp, "\r\n");
        time_date(stamp + 2);
        str_cat(stamp, "\r\n");
        if (insert_text(tlen, stamp, str_len(stamp))) { dirty = 1; caret = anchor = tlen; }
    }
    return 0;
}
static int save_file(const char *p)
{
    int h = dos_create(p), n;
    u16 done = 0, chunk;
    if (h < 0) return h;
    while (done < tlen) {
        chunk = tlen - done > 0x4000 ? 0x4000 : tlen - done;
        n = dos_write_far(h, tseg, done, chunk);
        if (n < 0) { dos_close(h); return n; }
        if (n < (int)chunk) { dos_close(h); return -112; }
        done += chunk;
    }
    dos_close(h);
    str_ncopy(path, p, PATH_MAX);
    dirty = 0;
    update_title();
    app_log("[CIUKNOTE] saved", p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Menus                                                               */
enum {
    M_NEW = 1, M_OPEN, M_SAVE, M_SAVEAS, M_PAGE, M_PRINT, M_EXIT,
    M_UNDO, M_CUT, M_COPY, M_PASTE, M_DELETE, M_FIND, M_FINDNEXT, M_REPLACE,
    M_GOTO, M_SELALL, M_TIMEDATE, M_WRAP, M_FONT, M_STATUS, M_HELP, M_ABOUT
};
static struct menu_item file_items[] = {
    { "&New", "Ctrl+N", M_NEW, 0 }, { "&Open...", "Ctrl+O", M_OPEN, 0 },
    { "&Save", "Ctrl+S", M_SAVE, 0 }, { "Save &As...", 0, M_SAVEAS, 0 },
    { "", 0, 0, MI_SEP }, { "Page Set&up...", 0, M_PAGE, 0 },
    { "&Print...", "Ctrl+P", M_PRINT, 0 }, { "", 0, 0, MI_SEP }, { "E&xit", 0, M_EXIT, 0 } };
static struct menu_item edit_items[] = {
    { "&Undo", "Ctrl+Z", M_UNDO, 0 }, { "", 0, 0, MI_SEP },
    { "Cu&t", "Ctrl+X", M_CUT, 0 }, { "&Copy", "Ctrl+C", M_COPY, 0 },
    { "&Paste", "Ctrl+V", M_PASTE, 0 }, { "De&lete", "Del", M_DELETE, 0 },
    { "", 0, 0, MI_SEP }, { "&Find...", "Ctrl+F", M_FIND, 0 },
    { "Find &Next", "F3", M_FINDNEXT, 0 }, { "&Replace...", "Ctrl+H", M_REPLACE, 0 },
    { "&Go To...", "Ctrl+G", M_GOTO, 0 }, { "", 0, 0, MI_SEP },
    { "Select &All", "Ctrl+A", M_SELALL, 0 }, { "Time/&Date", "F5", M_TIMEDATE, 0 } };
static struct menu_item format_items[] = {
    { "&Word Wrap", 0, M_WRAP, 0 }, { "&Font...", 0, M_FONT, 0 } };
static struct menu_item view_items[] = { { "&Status Bar", 0, M_STATUS, 0 } };
static struct menu_item help_items[] = {
    { "&Help Topics", "F1", M_HELP, 0 }, { "", 0, 0, MI_SEP }, { "&About CiukNote", 0, M_ABOUT, 0 } };
static struct menu menus[] = {
    { "&File", file_items, 9 }, { "&Edit", edit_items, 14 }, { "F&ormat", format_items, 2 },
    { "&View", view_items, 1 }, { "&Help", help_items, 3 } };
static struct menubar bar = { menus, 5, 0, 0, 0, -1, 0 };
static struct menu_item context_items[] = {
    { "&Undo", 0, M_UNDO, 0 }, { "", 0, 0, MI_SEP }, { "Cu&t", 0, M_CUT, 0 },
    { "&Copy", 0, M_COPY, 0 }, { "&Paste", 0, M_PASTE, 0 }, { "&Delete", 0, M_DELETE, 0 },
    { "", 0, 0, MI_SEP }, { "Select &All", 0, M_SELALL, 0 } };
static struct popup context;

static void set_flag(struct menu_item *it, int flag, int on)
{
    if (on) it->flags |= flag; else it->flags &= ~flag;
}
static void update_menus(void)
{
    set_flag(&edit_items[0], MI_DISABLED, !undo_valid);
    set_flag(&edit_items[2], MI_DISABLED, !has_sel());
    set_flag(&edit_items[3], MI_DISABLED, !has_sel());
    set_flag(&edit_items[4], MI_DISABLED, !clen);
    set_flag(&edit_items[5], MI_DISABLED, !has_sel());
    set_flag(&edit_items[8], MI_DISABLED, !find_text[0]);
    set_flag(&edit_items[10], MI_DISABLED, wrap);        /* as in Windows XP */
    set_flag(&format_items[0], MI_CHECKED, wrap);
    set_flag(&view_items[0], MI_CHECKED, status_bar && !wrap);
    set_flag(&view_items[0], MI_DISABLED, wrap);
    mem_copy(&context_items[0].flags, &edit_items[0].flags, sizeof(int));
    set_flag(&context_items[0], MI_DISABLED, !undo_valid);
    set_flag(&context_items[2], MI_DISABLED, !has_sel());
    set_flag(&context_items[3], MI_DISABLED, !has_sel());
    set_flag(&context_items[4], MI_DISABLED, !clen);
    set_flag(&context_items[5], MI_DISABLED, !has_sel());
    set_flag(&context_items[7], MI_DISABLED, !tlen);
}

/* ------------------------------------------------------------------ */
/* Dialogs                                                             */
enum { D_NONE, D_MSG, D_SAVE_PROMPT, D_FIND, D_REPLACE, D_GOTO, D_FONT, D_PAGE,
       D_ABOUT, D_HELP, D_OPEN, D_SAVEAS, D_OVERWRITE };
enum { A_NONE, A_NEW, A_OPEN, A_EXIT, A_OPEN_PATH };
static struct dialog dlg;
static int dlg_kind, pending;
static char pending_path[PATH_MAX];
static struct dctl dc[20];
static struct field f1, f2, f3, f4, f5, f6;
static char b1[64], b2[64], b3[8], b4[8], b5[8], b6[8];
static char msg_buf[200];

static void message(const char *title, const char *text)
{
    msgbox(&dlg, title, text, "OK");
    dlg_kind = D_MSG;
    app_log("[CIUKNOTE] message", text);
}
static void ctl(int i, int type, int x, int y, int w, int h, const char *text, int id)
{
    mem_set(&dc[i], 0, sizeof dc[i]);
    dc[i].type = type; dc[i].x = x; dc[i].y = y; dc[i].w = w; dc[i].h = h;
    dc[i].text = text; dc[i].id = id;
}

static void save_prompt(int action)
{
    str_copy(msg_buf, "The text in the ");
    str_cat(msg_buf, path[0] ? file_name(path) : "Untitled");
    str_cat(msg_buf, " file has changed.\n\nDo you want to save the changes?");
    msgbox(&dlg, "CiukNote", msg_buf, "Yes|No|Cancel");
    dlg_kind = D_SAVE_PROMPT;
    app_log("[CIUKNOTE] dialog", "save changes");
    pending = action;
}

static void find_dialog(int replace)
{
    field_set(&f1, b1, 60, find_text);
    f1.sel = 1;
    ctl(0, DC_LABEL, 0, 10, 0, 16, "Fi&nd what:", 0);
    ctl(1, DC_FIELD, 90, 6, 200, 22, 0, 0); dc[1].field = &f1;
    if (!replace) {
        ctl(2, DC_CHECK, 0, 62, 0, 17, "Match &case", 0); dc[2].value = find_case;
        ctl(3, DC_GROUP, 150, 40, 140, 44, "Direction", 0);
        ctl(4, DC_RADIO, 160, 60, 0, 17, "&Up", 0); dc[4].group = 1; dc[4].value = find_up;
        ctl(5, DC_RADIO, 220, 60, 0, 17, "&Down", 0); dc[5].group = 1; dc[5].value = !find_up;
        ctl(6, DC_BUTTON, 304, 4, 90, 24, "&Find Next", 1);
        ctl(7, DC_BUTTON, 304, 34, 90, 24, "Cancel", 2);
        dialog_show(&dlg, "Find", dc, 8, 410, 92, 1, 2);
        dlg_kind = D_FIND;
        app_log("[CIUKNOTE] dialog", "Find");
    } else {
        field_set(&f2, b2, 60, replace_text);
        ctl(2, DC_LABEL, 0, 40, 0, 16, "Re&place with:", 0);
        ctl(3, DC_FIELD, 90, 36, 200, 22, 0, 0); dc[3].field = &f2;
        ctl(4, DC_CHECK, 0, 96, 0, 17, "Match &case", 0); dc[4].value = find_case;
        ctl(5, DC_BUTTON, 304, 4, 96, 24, "&Find Next", 1);
        ctl(6, DC_BUTTON, 304, 34, 96, 24, "&Replace", 3);
        ctl(7, DC_BUTTON, 304, 64, 96, 24, "Replace &All", 4);
        ctl(8, DC_BUTTON, 304, 94, 96, 24, "Cancel", 2);
        dialog_show(&dlg, "Replace", dc, 9, 416, 124, 1, 2);
        dlg_kind = D_REPLACE;
        app_log("[CIUKNOTE] dialog", "Replace");
    }
    dlg.focus = 1;
}

static int match_at(u16 o, const char *s, int n)
{
    int i;
    if ((u32)o + n > tlen) return 0;
    for (i = 0; i < n; i++) {
        char a = CH(o + i), b = s[i];
        if (!find_case) { a = to_upper(a); b = to_upper(b); }
        if (a != b) return 0;
    }
    return 1;
}
/* Select the next match. 0 when there is none. */
static int find_next(int up)
{
    int n = str_len(find_text);
    u16 o;
    if (!n) return 0;
    if (!up) {
        for (o = sel_b() == sel_a() ? caret : sel_b(); (u32)o + n <= tlen; o++)
            if (match_at(o, find_text, n)) { anchor = o; caret = o + n; ensure_visible(); return 1; }
    } else {
        o = sel_a();
        while (o > 0) {
            o--;
            if (match_at(o, find_text, n)) { anchor = o; caret = o + n; ensure_visible(); return 1; }
        }
    }
    return 0;
}
static void not_found(void)
{
    str_copy(msg_buf, "Cannot find \"");
    str_cat(msg_buf, find_text);
    str_cat(msg_buf, "\"");
    message("CiukNote", msg_buf);
}
static void do_find_next(int up)
{
    if (!find_text[0]) { find_dialog(0); return; }
    if (!find_next(up)) not_found();
}
static void replace_all(void)
{
    int n = str_len(find_text), r = str_len(replace_text), count = 0;
    u16 o = 0;
    if (!n) return;
    begin_edit(0);
    while ((u32)o + n <= tlen) {
        if (match_at(o, find_text, n)) {
            if ((u32)tlen - n + r > TEXT_MAX) break;
            delete_range(o, n);
            insert_text(o, replace_text, r);
            o += r;
            count++;
        } else o++;
    }
    caret = anchor = 0;
    { char n[8]; fmt_u32(n, (u32)count); app_log("[CIUKNOTE] replaced", n); }
    if (!count) { undo_valid = 0; not_found(); }
    ensure_visible();
}

static void goto_dialog(void)
{
    char t[8];
    u16 line = 1, i;
    for (i = 0; i < caret; i++) if (CH(i) == '\n') line++;
    fmt_u32(t, line);
    field_set(&f1, b3, 6, t);
    f1.sel = 1;
    ctl(0, DC_LABEL, 0, 0, 0, 16, "&Line number:", 0);
    ctl(1, DC_FIELD, 0, 20, 200, 22, 0, 0); dc[1].field = &f1;
    ctl(2, DC_BUTTON, 20, 56, 80, 24, "OK", 1);
    ctl(3, DC_BUTTON, 110, 56, 80, 24, "Cancel", 2);
    dialog_show(&dlg, "Go To Line", dc, 4, 230, 86, 1, 2);
    app_log("[CIUKNOTE] dialog", "Go To Line");
    dlg.focus = 1;
    dlg_kind = D_GOTO;
}
static void goto_line(u16 line)
{
    u16 o = 0, l = 1;
    while (l < line && o < tlen) {
        if (CH(o) == '\n') l++;
        o++;
    }
    if (l < line) { message("CiukNote - Goto Line", "The line number is beyond the total number of lines"); return; }
    move_caret(o, 0);
    goal = NONE;
    ensure_visible();
}

/* Font: the desktop's fixed-pitch font, or an installed font (\SYSTEM\FONTS)
 * drawn through the alternate font slot. */
#define NP_FONTS 12
static char font_files[NP_FONTS][13], font_names[NP_FONTS][32];
static int nfonts, font_first;
static void font_path(char *out, const char *file)
{
    str_copy(out, "C:\\SYSTEM\\FONTS\\");
    str_cat(out, file);
}
static void use_font(void)
{
    char p[40];
    if (!np_font[0]) return;
    font_path(p, np_font);
    if (app_font(p, 1)) np_font[0] = 0;       /* gone: the desktop font */
}
static void font_dialog(void)
{
    struct dos_find f;
    char p[40], h[64];
    int r, i, k;
    nfonts = 1;
    str_copy(font_files[0], "");
    str_copy(font_names[0], "Fixedsys (fixed pitch)");
    dos_set_dta(&f);
    for (r = dos_find_first("C:\\SYSTEM\\FONTS\\*.CFN", A_ARCH | A_RDONLY); r >= 0 && nfonts < NP_FONTS; r = dos_find_next())
        str_ncopy(font_files[nfonts++], f.name, 13);
    for (i = 1; i < nfonts; i++) {
        int fh;
        font_path(p, font_files[i]);
        fh = dos_open(p, 0);
        str_copy(font_names[i], font_files[i]);
        if (fh >= 0) {
            if (dos_read(fh, h, 64) == 64 && !str_nicmp(h, "CIUKFNT1", 8)) str_ncopy(font_names[i], h + 8, 32);
            dos_close(fh);
        }
    }
    k = 0;
    ctl(k++, DC_LABEL, 0, 0, 0, 16, "&Font:", 0);
    font_first = k;
    for (i = 0; i < nfonts; i++) {
        ctl(k, DC_RADIO, (i % 2) * 220, 22 + (i / 2) * 22, 0, 17, font_names[i], 0);
        dc[k].group = 2;
        dc[k].value = !str_icmp(np_font, font_files[i]);
        k++;
    }
    r = 22 + ((nfonts + 1) / 2) * 22 + 6;
    ctl(k++, DC_GROUP, 0, r, 440, 48, "Font style", 0);
    ctl(k, DC_RADIO, 12, r + 22, 0, 17, "&Regular", 0); dc[k].group = 1; dc[k].value = !bold; k++;
    ctl(k, DC_RADIO, 132, r + 22, 0, 17, "&Bold", 0); dc[k].group = 1; dc[k].value = bold; k++;
    ctl(k++, DC_GROUP, 0, r + 58, 440, 52, "Sample", 0);
    ctl(k++, DC_BUTTON, 264, r + 122, 84, 26, "OK", 1);
    ctl(k++, DC_BUTTON, 356, r + 122, 84, 26, "Cancel", 2);
    dialog_show(&dlg, "Font", dc, k, 456, r + 152, 1, 2);
    app_log("[CIUKNOTE] dialog", "Font");
    dlg_kind = D_FONT;
}
static int font_choice(void)
{
    int i;
    for (i = 0; i < nfonts; i++) if (dc[font_first + i].value) return i;
    return 0;
}
static void font_sample(void)
{
    int i = font_choice(), b = dc[font_first + nfonts + 2].value, y;
    int x = dlg.x + 8 + 12, gy = 0;
    char p[40];
    for (y = 0; y < dlg.n; y++) if (dc[y].type == DC_GROUP) gy = dc[y].y;
    y = dlg.y + DIALOG_TITLE_H + 6 + gy + 22;
    if (!i) { ui_mono(x, y, "AaBbYyZz 0123", C_INK | (b ? BOLD : 0), CELL); return; }
    font_path(p, font_files[i]);
    app_font(p, 1);
    ui_text(x, y, "AaBbYyZz 0123 The quick brown fox", C_INK | ALTFONT | (b ? BOLD : 0));
}

static void page_dialog(void)
{
    field_set(&f1, b1, 60, header_text);
    field_set(&f2, b2, 60, footer_text);
    field_set(&f3, b3, 4, margin_l);
    field_set(&f4, b4, 4, margin_r);
    field_set(&f5, b5, 4, margin_t);
    field_set(&f6, b6, 4, margin_b);
    ctl(0, DC_GROUP, 0, 0, 190, 70, "Orientation", 0);
    ctl(1, DC_RADIO, 12, 20, 0, 17, "P&ortrait", 0); dc[1].group = 1; dc[1].value = !landscape;
    ctl(2, DC_RADIO, 12, 42, 0, 17, "L&andscape", 0); dc[2].group = 1; dc[2].value = landscape;
    ctl(3, DC_GROUP, 200, 0, 220, 70, "Margins (characters, lines)", 0);
    ctl(4, DC_LABEL, 210, 20, 0, 16, "&Left:", 0);
    ctl(5, DC_FIELD, 256, 16, 44, 22, 0, 0); dc[5].field = &f3;
    ctl(6, DC_LABEL, 316, 20, 0, 16, "&Right:", 0);
    ctl(7, DC_FIELD, 366, 16, 44, 22, 0, 0); dc[7].field = &f4;
    ctl(8, DC_LABEL, 210, 44, 0, 16, "&Top:", 0);
    ctl(9, DC_FIELD, 256, 40, 44, 22, 0, 0); dc[9].field = &f5;
    ctl(10, DC_LABEL, 316, 44, 0, 16, "&Bottom:", 0);
    ctl(11, DC_FIELD, 366, 40, 44, 22, 0, 0); dc[11].field = &f6;
    ctl(12, DC_LABEL, 0, 84, 0, 16, "&Header:", 0);
    ctl(13, DC_FIELD, 70, 80, 350, 22, 0, 0); dc[13].field = &f1;
    ctl(14, DC_LABEL, 0, 112, 0, 16, "&Footer:", 0);
    ctl(15, DC_FIELD, 70, 108, 350, 22, 0, 0); dc[15].field = &f2;
    ctl(16, DC_BUTTON, 250, 142, 80, 24, "OK", 1);
    ctl(17, DC_BUTTON, 340, 142, 80, 24, "Cancel", 2);
    dialog_show(&dlg, "Page Setup", dc, 18, 436, 172, 1, 2);
    app_log("[CIUKNOTE] dialog", "Page Setup");
    dlg_kind = D_PAGE;
}

static void about_dialog(void)
{
    ctl(0, DC_LABEL, 50, 0, 0, 16, "CiukiOS CiukNote", 0);
    ctl(1, DC_LABEL, 50, 20, 0, 16, "Version 0.8.3", 0);
    ctl(2, DC_LABEL, 50, 40, 0, 16, "A modern Retro OS", 0);
    ctl(3, DC_LABEL, 50, 66, 0, 16, "Text limit: 61,440 bytes (as Windows 9x: 64 KB).", 0);
    ctl(4, DC_LABEL, 50, 86, 0, 16, "Printing goes to LPT1.", 0);
    ctl(5, DC_BUTTON, 150, 116, 80, 24, "OK", 1);
    dialog_show(&dlg, "About CiukNote", dc, 6, 380, 146, 1, 1);
    app_log("[CIUKNOTE] dialog", "About CiukNote");
    dlg_kind = D_ABOUT;
}
static const char *help_lines[] = {
    "Ctrl+N  New            Ctrl+Z  Undo           F3      Find Next",
    "Ctrl+O  Open           Ctrl+X  Cut            Ctrl+H  Replace",
    "Ctrl+S  Save           Ctrl+C  Copy           Ctrl+G  Go To",
    "Ctrl+P  Print          Ctrl+V  Paste          Ctrl+A  Select All",
    "Ctrl+F  Find           Del     Delete         F5      Time/Date",
    "Shift+arrows select. Double-click selects a word. Right-click: menu.",
    "Alt or F10 opens the menus. A file starting with .LOG gets the time",
    "and date added each time it is opened."
};
static void help_dialog(void)
{
    int i;
    for (i = 0; i < 8; i++) ctl(i, DC_LABEL, 0, i * 18, 0, 16, help_lines[i], 0);
    ctl(8, DC_BUTTON, 210, 8 * 18 + 10, 80, 24, "OK", 1);
    dialog_show(&dlg, "CiukNote Help", dc, 9, 500, 8 * 18 + 40, 1, 1);
    dlg_kind = D_HELP;
}

static void run_pending(void);

/* ---- Open / Save As ---- */
#define FD_MAX 160
static struct { char name[13]; u8 dir; } fd_list[FD_MAX];
static int fd_count, fd_sel, fd_top, fd_all;
static char fd_dir[PATH_MAX];
static unsigned fd_click_tick;
static int fd_click_item = -1;
#define FD_LIST_X 0
#define FD_LIST_Y 30
#define FD_LIST_W 440
#define FD_ROWS   9

static int fd_less(int a, int b)
{
    if (fd_list[a].dir != fd_list[b].dir) return fd_list[a].dir > fd_list[b].dir;
    return str_icmp(fd_list[a].name, fd_list[b].name) < 0;
}
static void fd_load(void)
{
    char pat[PATH_MAX + 8];
    struct dos_find f;
    int r, i, j;
    fd_count = 0; fd_sel = -1; fd_top = 0;
    str_copy(pat, fd_dir);
    if (pat[str_len(pat) - 1] != '\\') str_cat(pat, "\\");
    str_cat(pat, "*.*");
    dos_set_dta(&f);
    for (r = dos_find_first(pat, A_DIR); r >= 0 && fd_count < FD_MAX; r = dos_find_next()) {
        int len = str_len(f.name);
        if (f.attr & A_VOLUME) continue;
        if (f.name[0] == '.' && (f.name[1] == 0 || (f.name[1] == '.' && f.name[2] == 0))) continue;
        if (!(f.attr & A_DIR) && !fd_all && (len < 4 || str_icmp(f.name + len - 4, ".TXT"))) continue;
        str_copy(fd_list[fd_count].name, f.name);
        fd_list[fd_count].dir = (f.attr & A_DIR) ? 1 : 0;
        fd_count++;
    }
    for (i = 1; i < fd_count; i++)              /* insertion sort */
        for (j = i; j > 0 && fd_less(j, j - 1); j--) {
            char t[14];
            u8 d;
            str_copy(t, fd_list[j].name); d = fd_list[j].dir;
            str_copy(fd_list[j].name, fd_list[j - 1].name); fd_list[j].dir = fd_list[j - 1].dir;
            str_copy(fd_list[j - 1].name, t); fd_list[j - 1].dir = d;
        }
}
static void fd_open(int save)
{
    if (path[0]) {
        const char *n = file_name(path);
        str_ncopy(fd_dir, path, (int)(n - path) + 1);
        if (str_len(fd_dir) > 3 && fd_dir[str_len(fd_dir) - 1] == '\\') fd_dir[str_len(fd_dir) - 1] = 0;
        field_set(&f1, b1, 60, save ? n : "");
    } else {
        if (!fd_dir[0]) {
            fd_dir[0] = (char)('A' + dos_get_drive());
            fd_dir[1] = ':'; fd_dir[2] = '\\';
            dos_getcwd(0, fd_dir + 3);
        }
        field_set(&f1, b1, 60, save ? "*.txt" : "");
    }
    f1.sel = 1;
    fd_all = 0;
    fd_load();
    ctl(0, DC_LABEL, 0, 4, 0, 16, "Look in:", 0);
    ctl(1, DC_LABEL, 64, 4, 0, 16, fd_dir, 0);
    ctl(2, DC_BUTTON, 380, 0, 60, 22, "&Up", 5);
    ctl(3, DC_LABEL, 0, FD_LIST_Y + FD_ROWS * 18 + 12, 0, 16, "File &name:", 0);
    ctl(4, DC_FIELD, 96, FD_LIST_Y + FD_ROWS * 18 + 8, 250, 22, 0, 0); dc[4].field = &f1;
    ctl(5, DC_LABEL, 0, FD_LIST_Y + FD_ROWS * 18 + 40, 0, 16, "Files of type:", 0);
    ctl(6, DC_RADIO, 96, FD_LIST_Y + FD_ROWS * 18 + 40, 0, 17, "&Text Documents (*.txt)", 0);
    dc[6].group = 1; dc[6].value = 1;
    ctl(7, DC_RADIO, 290, FD_LIST_Y + FD_ROWS * 18 + 40, 0, 17, "All &Files", 0);
    dc[7].group = 1;
    ctl(8, DC_BUTTON, 360, FD_LIST_Y + FD_ROWS * 18 + 6, 80, 24, save ? "&Save" : "&Open", 1);
    ctl(9, DC_BUTTON, 360, FD_LIST_Y + FD_ROWS * 18 + 64, 80, 24, "Cancel", 2);
    dialog_show(&dlg, save ? "Save As" : "Open", dc, 10, 456, FD_LIST_Y + FD_ROWS * 18 + 94, 1, 2);
    app_log("[CIUKNOTE] dialog", save ? "Save As" : "Open");
    dlg.focus = 4;
    dlg_kind = save ? D_SAVEAS : D_OPEN;
}
static void fd_list_origin(int *x, int *y)
{
    *x = dlg.x + 8 + FD_LIST_X;
    *y = dlg.y + DIALOG_TITLE_H + 6 + FD_LIST_Y;
}
static void fd_draw(void)
{
    int x, y, i;
    fd_list_origin(&x, &y);
    ui_inset(x, y, FD_LIST_W, FD_ROWS * 18 + 4);
    for (i = 0; i < FD_ROWS && fd_top + i < fd_count; i++) {
        int k = fd_top + i, ry = y + 2 + i * 18, fg = C_INK;
        if (k == fd_sel) { ui_rect(x + 2, ry, FD_LIST_W - 20, 18, C_TITLE); fg = C_PAPER; }
        if (fd_list[k].dir) {
            ui_rect(x + 6, ry + 5, 12, 9, C_YELLOW);
            ui_rect(x + 6, ry + 3, 5, 2, C_YELLOW);
        } else {
            ui_rect(x + 7, ry + 2, 10, 13, C_PAPER);
            ui_rect(x + 7, ry + 2, 10, 1, C_SHADOW);
            ui_rect(x + 7, ry + 14, 10, 1, C_SHADOW);
            ui_rect(x + 7, ry + 2, 1, 13, C_SHADOW);
            ui_rect(x + 16, ry + 2, 1, 13, C_SHADOW);
            ui_rect(x + 9, ry + 6, 6, 1, C_SHADOW);
            ui_rect(x + 9, ry + 9, 6, 1, C_SHADOW);
        }
        ui_text(x + 24, ry + 1, fd_list[k].name, fg);
    }
    draw_scroll(x + FD_LIST_W - 18, y + 2, FD_ROWS * 18, fd_top, fd_count, FD_ROWS);
}
static void fd_enter(const char *name)
{
    if (!str_cmp(name, "..")) {
        int n = str_len(fd_dir);
        while (n > 3 && fd_dir[n - 1] != '\\') n--;
        if (n > 3) n--;
        fd_dir[n] = 0;
        if (n == 2) { fd_dir[2] = '\\'; fd_dir[3] = 0; }
    } else {
        if (fd_dir[str_len(fd_dir) - 1] != '\\') str_cat(fd_dir, "\\");
        str_cat(fd_dir, name);
    }
    fd_load();
}
/* The name typed or chosen: a folder, a filter, or the file. */
static void fd_accept(void)
{
    char full[PATH_MAX + 16];
    int i, has_dot = 0, attr;
    const char *n = f1.text;
    if (!n[0]) return;
    for (i = 0; n[i]; i++) {
        if (n[i] == '*' || n[i] == '?') { fd_all = str_icmp(n, "*.txt") != 0; fd_load(); return; }
        if (n[i] == '.') has_dot = 1;
    }
    if (n[1] == ':' || n[0] == '\\') str_copy(full, n);
    else {
        str_copy(full, fd_dir);
        if (full[str_len(full) - 1] != '\\') str_cat(full, "\\");
        str_cat(full, n);
    }
    for (i = 0; full[i]; i++) full[i] = to_upper(full[i]);
    attr = dos_get_attr(full);
    if (attr >= 0 && (attr & A_DIR)) {
        str_ncopy(fd_dir, full, PATH_MAX);
        f1.text[0] = 0; f1.len = f1.cursor = 0;
        fd_load();
        return;
    }
    if (dlg_kind == D_OPEN) {
        int r;
        if (attr < 0) {
            str_copy(msg_buf, full);
            str_cat(msg_buf, "\nFile not found.\nCheck the file name and try again.");
            message("Open", msg_buf);
            return;
        }
        dlg.open = 0;
        r = load_file(full);
        if (r == 1) message("CiukNote", "This file is too large for CiukNote.\nCiukNote opens files of up to 61,440 bytes.");
        else if (r) message("CiukNote", dos_error_text(r));
        dlg_kind = dlg.open ? dlg_kind : D_NONE;
        return;
    }
    if (!has_dot && !fd_all) str_cat(full, ".txt");
    for (i = 0; full[i]; i++) full[i] = to_upper(full[i]);
    if (dos_get_attr(full) >= 0) {
        str_ncopy(pending_path, full, PATH_MAX);
        str_copy(msg_buf, file_name(full));
        str_cat(msg_buf, " already exists.\nDo you want to replace it?");
        msgbox(&dlg, "Save As", msg_buf, "Yes|No");
        dlg_kind = D_OVERWRITE;
        return;
    }
    dlg.open = 0;
    dlg_kind = D_NONE;
    {
        int r = save_file(full);
        if (r) { pending = A_NONE; message("CiukNote", dos_error_text(r)); return; }
    }
    if (pending) run_pending();
}
static int fd_mouse(int kind, int sx, int sy)
{
    int x, y, i;
    fd_list_origin(&x, &y);
    if (kind != MOUSE_DOWN) return 0;
    if (sx >= x + FD_LIST_W - 18 && sx < x + FD_LIST_W - 2 && sy >= y + 2 && sy < y + 2 + FD_ROWS * 18) {
        long p = scroll_hit(x + FD_LIST_W - 18, y + 2, FD_ROWS * 18, sy, fd_count, FD_ROWS);
        if (p == -1 && fd_top > 0) fd_top--;
        else if (p == -2 && fd_top + FD_ROWS < fd_count) fd_top++;
        else if (p >= 16) fd_top = (int)(p - 16);
        return 1;
    }
    if (sx < x || sx >= x + FD_LIST_W - 18 || sy < y + 2 || sy >= y + 2 + FD_ROWS * 18) return 0;
    i = fd_top + (sy - y - 2) / 18;
    if (i >= fd_count) return 1;
    if (i == fd_click_item && HOST.ticks - fd_click_tick < 9) {
        fd_click_item = -1;
        if (fd_list[i].dir) { fd_enter(fd_list[i].name); return 1; }
        field_set(&f1, b1, 60, fd_list[i].name);
        fd_accept();
        return 1;
    }
    fd_click_item = i;
    fd_click_tick = HOST.ticks;
    fd_sel = i;
    if (!fd_list[i].dir) field_set(&f1, b1, 60, fd_list[i].name);
    dlg.focus = 4;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Printing to LPT1 (BIOS INT 17h), plain text with header and footer. */
static int print_char(int c)
{
    int st = bios_printer(0, c);
    return (st & 0x29) ? 0 : 1;          /* time-out, I/O error, no paper */
}
static int print_string(const char *s)
{
    while (*s) if (!print_char(*s++)) return 0;
    return 1;
}
static void expand_code(const char *fmt, int page, char *out)
{
    char t[40];
    *out = 0;
    while (*fmt) {
        if (*fmt == '&' && fmt[1]) {
            char c = to_lower(fmt[1]);
            fmt += 2;
            if (c == 'f') str_cat(out, path[0] ? file_name(path) : "Untitled");
            else if (c == 'p') { fmt_u32(t, (u32)page); str_cat(out, t); }
            else if (c == 'd' || c == 't') {
                time_date(t);
                if (c == 't') { char *sp = t; int k = 0; while (sp[k] && !(sp[k] == ' ' && (sp[k + 1] == 'A' || sp[k + 1] == 'P'))) k++; k += 3; t[k] = 0; str_cat(out, t); }
                else { int k = 0; while (t[k] && !(t[k] == 'M' && t[k + 1] == ' ')) k++; str_cat(out, t + k + 2); }
            }
            else if (c == '&') str_cat(out, "&");
            continue;
        }
        { int n = str_len(out); if (n < 120) { out[n] = *fmt; out[n + 1] = 0; } }
        fmt++;
    }
}
static int print_line(const char *s, int left)
{
    int i;
    for (i = 0; i < left; i++) if (!print_char(' ')) return 0;
    return print_string(s) && print_string("\r\n");
}
static void print_document(void)
{
    int st = bios_printer(0, -1);
    int ml = parse_u16(margin_l), mr = parse_u16(margin_r), mt = parse_u16(margin_t), mb = parse_u16(margin_b);
    int width = (landscape ? 136 : 80) - ml - mr, lines = (landscape ? 51 : 66) - mt - mb - 4;
    int page = 1, row, i;
    u16 o = 0;
    char buf[140];
    if (!(st & 0x80) || (st & 0x29) || !(st & 0x10)) {
        message("Print", "The printer on LPT1 is not ready.\nCheck that it is on, on line and has paper.");
        return;
    }
    if (width < 20) width = 20;
    if (lines < 5) lines = 5;
    while (o < tlen || page == 1) {
        for (i = 0; i < mt; i++) if (!print_string("\r\n")) goto failed;
        expand_code(header_text, page, buf);
        if (!print_line(buf, ml) || !print_string("\r\n")) goto failed;
        for (row = 0; row < lines && o < tlen; row++) {
            int n = 0, c = 0;
            while (o < tlen && CH(o) != '\r' && CH(o) != '\n' && c < width) {
                char ch = CH(o++);
                if (ch == '\t') { do buf[n++] = ' '; while (++c % TAB && c < width); }
                else { buf[n++] = ch; c++; }
            }
            buf[n] = 0;
            if (o < tlen && (CH(o) == '\r' || CH(o) == '\n')) o = after_eol(o);
            if (!print_line(buf, ml)) goto failed;
        }
        for (; row < lines; row++) if (!print_string("\r\n")) goto failed;
        expand_code(footer_text, page, buf);
        if (!print_string("\r\n") || !print_line(buf, ml) || !print_char('\f')) goto failed;
        page++;
        if (o >= tlen) break;
    }
    return;
failed:
    message("Print", "Printing stopped: the printer on LPT1 reported an error.");
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
static void command(int id);
static void run_pending(void)
{
    int p = pending;
    pending = A_NONE;
    if (p == A_NEW) new_document();
    else if (p == A_OPEN) fd_open(0);
    else if (p == A_EXIT) { closing = 1; app_close(); }
    else if (p == A_OPEN_PATH) {
        int r = load_file(pending_path);
        if (r == 1) message("CiukNote", "This file is too large for CiukNote.\nCiukNote opens files of up to 61,440 bytes.");
        else if (r) message("CiukNote", dos_error_text(r));
    }
}
static void ask_then(int action)
{
    if (dirty) save_prompt(action);
    else { pending = action; run_pending(); }
}
static void save(void)
{
    int r;
    if (!path[0]) { fd_open(1); return; }
    r = save_file(path);
    if (r) message("CiukNote", dos_error_text(r));
}
static void command(int id)
{
    switch (id) {
    case M_NEW: ask_then(A_NEW); break;
    case M_OPEN: ask_then(A_OPEN); break;
    case M_SAVE: save(); break;
    case M_SAVEAS: fd_open(1); break;
    case M_PAGE: page_dialog(); break;
    case M_PRINT: print_document(); break;
    case M_EXIT: ask_then(A_EXIT); break;
    case M_UNDO: undo(); break;
    case M_CUT: if (has_sel()) { copy_selection(); begin_edit(0); delete_selection(); } break;
    case M_COPY: copy_selection(); break;
    case M_PASTE: paste(); break;
    case M_DELETE:
        if (has_sel()) { begin_edit(0); delete_selection(); }
        else if (caret < tlen) { begin_edit(0); delete_range(caret, right_of(caret) - caret); anchor = caret; }
        break;
    case M_FIND: find_dialog(0); break;
    case M_FINDNEXT: do_find_next(find_up); break;
    case M_REPLACE: find_dialog(1); break;
    case M_GOTO: if (!wrap) goto_dialog(); break;
    case M_SELALL: anchor = 0; caret = tlen; break;
    case M_TIMEDATE: { char t[40]; time_date(t); replace_with(t, str_len(t), 0); } break;
    case M_WRAP: wrap = !wrap; top = row_of(top); hcol = 0; break;
    case M_FONT: font_dialog(); break;
    case M_STATUS: status_bar = !status_bar; break;
    case M_HELP: help_dialog(); break;
    case M_ABOUT: about_dialog(); break;
    }
    ensure_visible();
}

/* A dialog button (or Enter/Esc) gave result r. */
static void dialog_result(int r)
{
    int kind = dlg_kind;
    if (r < 0) return;
    if (kind == D_MSG || kind == D_ABOUT || kind == D_HELP) { dlg_kind = D_NONE; return; }
    if (kind == D_SAVE_PROMPT) {
        dlg_kind = D_NONE;
        if (r == MB_CANCEL) { pending = A_NONE; return; }
        if (r == MB_NO) { run_pending(); return; }
        if (!path[0]) { fd_open(1); return; }          /* pending stays */
        {
            int e = save_file(path);
            if (e) { pending = A_NONE; message("CiukNote", dos_error_text(e)); return; }
        }
        run_pending();
        return;
    }
    if (kind == D_FIND || kind == D_REPLACE) {
        str_copy(find_text, f1.text);
        if (kind == D_FIND) {
            find_case = dc[2].value;
            find_up = dc[4].value;
        } else {
            str_copy(replace_text, f2.text);
            find_case = dc[4].value;
        }
        if (r == 2) { dlg_kind = D_NONE; return; }
        dlg.open = 1;                                  /* stays open, as Windows */
        if (r == 1) { if (!find_next(kind == D_FIND ? find_up : 0)) { dlg.open = 0; not_found(); } }
        else if (r == 3) {
            int n = str_len(find_text);
            if (has_sel() && sel_b() - sel_a() == (u16)n && match_at(sel_a(), find_text, n)) {
                replace_with(replace_text, str_len(replace_text), 0);
            }
            if (!find_next(0)) { dlg.open = 0; not_found(); }
        } else if (r == 4) { dlg.open = 0; dlg_kind = D_NONE; replace_all(); }
        return;
    }
    if (kind == D_GOTO) {
        dlg_kind = D_NONE;
        if (r == 1) goto_line(parse_u16(f1.text));
        return;
    }
    if (kind == D_FONT) {
        dlg_kind = D_NONE;
        if (r == 1) {
            bold = dc[font_first + nfonts + 2].value;
            str_copy(np_font, font_files[font_choice()]);
            app_log("[CIUKNOTE] font", np_font[0] ? np_font : "Fixedsys");
        }
        use_font();
        return;
    }
    if (kind == D_PAGE) {
        dlg_kind = D_NONE;
        if (r == 1) {
            landscape = dc[2].value;
            str_copy(header_text, f1.text);
            str_copy(footer_text, f2.text);
            str_copy(margin_l, f3.text); str_copy(margin_r, f4.text);
            str_copy(margin_t, f5.text); str_copy(margin_b, f6.text);
        }
        return;
    }
    if (kind == D_OPEN || kind == D_SAVEAS) {
        if (r == 2) { dlg_kind = D_NONE; pending = A_NONE; return; }
        if (r == 5) { fd_enter(".."); dlg.open = 1; return; }
        dlg.open = 1;
        fd_all = dc[7].value;
        fd_accept();
        return;
    }
    if (kind == D_OVERWRITE) {
        if (r == MB_YES) {
            int e = save_file(pending_path);
            dlg_kind = D_NONE;
            if (e) { message("CiukNote", dos_error_text(e)); return; }
            if (pending) run_pending();
        } else fd_open(1);
    }
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
static void draw_row(u16 s, int y)
{
    char buf[160];
    u16 e = row_end(s), i, a = sel_a(), b = sel_b();
    int c = 0, n = 0, ca = -1, cb = -1, x;
    for (i = s; i <= e; i++) {
        if (i == a) ca = c;
        if (i == b) cb = c;
        if (i == e) break;
        {
            int w = char_w(i, c), k;
            char ch = CH(i);
            if (ch == '\t') ch = ' ';
            if ((u8)ch < 32 || (u8)ch > 126) ch = '?';
            for (k = 0; k < w; k++) {
                if (c >= (int)hcol && c < (int)hcol + cols && n < 158) buf[n++] = ch;
                c++;
            }
        }
    }
    buf[n] = 0;
    if (a < s) ca = 0;
    if (b > e) cb = c + 1;                  /* the selection runs past this row */
    x = ex + 4;
    if (ca >= 0 && cb >= 0 && cb > ca && a != b) {
        int x0 = ca - (int)hcol, x1 = cb - (int)hcol;
        if (x0 < 0) x0 = 0;
        if (x1 > cols) x1 = cols;
        if (x1 > x0) ui_rect(x + x0 * CELL, y, (x1 - x0) * CELL, LINE_H, C_TITLE);
        ui_mono(x, y, buf, C_INK | text_style(), CELL);
        if (x1 > x0) {
            char sel[160];
            int k = 0;
            while (x0 + k < x1 && x0 + k < n) { sel[k] = buf[x0 + k]; k++; }
            sel[k] = 0;
            ui_mono(x + x0 * CELL, y, sel, C_PAPER | text_style(), CELL);
        }
    } else {
        ui_mono(x, y, buf, C_INK | text_style(), CELL);
    }
}
static void hscroll_draw(int x, int y, int w, int pos, int total, int page)
{
    int track = w - 32, tw, tx, i;
    ui_rect(x, y, w, 16, C_FACE);
    ui_rect(x + 16, y, w - 32, 16, C_LIGHT);
    ui_bevel(x, y, 16, 16, C_FACE);
    ui_bevel(x + w - 16, y, 16, 16, C_FACE);
    for (i = 0; i < 4; i++) {
        ui_rect(x + 6 + i, y + 8 - i, 1, 1 + 2 * i, C_INK);
        ui_rect(x + w - 7 - i, y + 8 - i, 1, 1 + 2 * i, C_INK);
    }
    if (total <= page || track < 8) return;
    tw = (int)((long)track * page / total);
    if (tw < 12) tw = 12;
    tx = (int)((long)(track - tw) * pos / (total - page));
    ui_bevel(x + 16 + tx, y, tw, 16, C_FACE);
}
static void use_font(void);
static void paint(void)
{
    int i, y;
    u16 r;
    layout();
    use_font();                                  /* the alternate slot is shared */
    update_menus();
    bar.x = HOST.x + 3; bar.y = HOST.y + TITLE_H; bar.w = HOST.w - 6;
    ui_rect(ex - 1, ey - 1, ew + 18, eh + 2, C_FACE);
    ui_inset(ex, ey, ew, eh);
    r = row_of(top);
    y = ey + 2;
    for (i = 0; i < rows; i++) {
        draw_row(r, y);
        if (HOST.active && !dlg.open && caret >= r && caret <= row_end(r) &&
            (row_of(caret) == r)) {
            int c = col_of(r, caret) - (int)hcol;
            if (c >= 0 && c <= cols) ui_rect(ex + 4 + c * CELL, y + 1, 2, LINE_H - 2, C_INK);
        }
        r = row_next(r);
        if (r == NONE) break;
        y += LINE_H;
    }
    {
        u16 total = row_count(), pos = row_index(row_of(top));
        draw_scroll(ex + ew, ey, eh, pos, total, rows);
    }
    if (!wrap) {
        int longest = longest_line() + 1;
        hscroll_draw(ex, ey + eh, ew, hcol, longest, cols);
        ui_rect(ex + ew, ey + eh, 16, 16, C_FACE);
    }
    if (status_bar && !wrap) {
        char t[40], n[8];
        u16 line = 1, col = 1, i2, ls = line_start(caret);
        int sy = ey + eh + 18;
        for (i2 = 0; i2 < caret; i2++) if (CH(i2) == '\n') line++;
        col = (u16)col_of(ls, caret) + 1;
        ui_rect(ex - 1, sy, ew + 18, 20, C_FACE);
        ui_rect(ex - 1, sy, ew + 18, 1, C_SHADOW);
        str_copy(t, "Ln ");
        fmt_u32(n, line); str_cat(t, n);
        str_cat(t, ", Col ");
        fmt_u32(n, col); str_cat(t, n);
        ui_inset(ex + ew - 150, sy + 2, 166, 17);
        ui_text(ex + ew - 144, sy + 2, t, C_INK);
    }
    menubar_draw(&bar);
    if (context.open) popup_draw(&context);
}
static void paint_dialog(void)
{
    dialog_draw(&dlg);
    if (dlg_kind == D_OPEN || dlg_kind == D_SAVEAS) fd_draw();
    if (dlg_kind == D_FONT) font_sample();
    if (dlg_kind == D_ABOUT) ui_icon(dlg.x + 10, dlg.y + DIALOG_TITLE_H + 8, ICON_EDITOR);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
static u16 offset_at(int sx, int sy)
{
    int row = (sy - ey - 2) / LINE_H, c, i;
    u16 r = row_of(top), n;
    if (sy < ey + 2) row = -1;
    if (row < 0) { n = row_prev(r); return n == NONE ? r : n; }
    for (i = 0; i < row; i++) {
        n = row_next(r);
        if (n == NONE) return tlen;
        r = n;
    }
    c = sx - ex - 4;
    c = (c < 0 ? 0 : (c + CELL / 2) / CELL) + (int)hcol;
    return off_at_col(r, c, 0);
}
static void select_word(u16 o)
{
    u16 a = o, b = o;
    if (o < tlen && is_word(CH(o))) {
        while (a > 0 && is_word(CH(a - 1))) a--;
        while (b < tlen && is_word(CH(b))) b++;
    } else if (o < tlen && CH(o) != '\r' && CH(o) != '\n') b = o + 1;
    while (b < tlen && CH(b) == ' ') b++;                /* with its space, as Windows */
    anchor = a;
    caret = b;
}
static int on_mouse(int kind, int x, int y)
{
    int sx = HOST.x + x, sy = HOST.y + TITLE_H + y, r;
    layout();
    if (dlg.open) {
        dialog_place(&dlg);
        if (dialog_mine(&dlg) && (dlg_kind == D_OPEN || dlg_kind == D_SAVEAS) && fd_mouse(kind, sx, sy)) return 1;
        r = dialog_mouse(&dlg, kind, sx, sy);
        if (r >= 0) dialog_result(r);
        return 1;
    }
    if (context.open) {
        r = popup_mouse(&context, kind, sx, sy);
        if (r >= 0) command(r);
        return 1;
    }
    r = menubar_mouse(&bar, kind, sx, sy);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    if (kind == MOUSE_RIGHT) {
        update_menus();
        popup_open(&context, context_items, 8, sx, sy, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
        return 1;
    }
    /* Scroll bars. */
    if (sx >= ex + ew && sx < ex + ew + 16 && sy >= ey && sy < ey + eh) {
        if (kind == MOUSE_DOWN || kind == MOUSE_MOVE) {
            u16 total = row_count(), pos = row_index(row_of(top));
            long p = scroll_hit(ex + ew, ey, eh, sy, total, rows);
            u16 nt;
            if (p == -1) { nt = row_prev(row_of(top)); if (nt != NONE) top = nt; }
            else if (p == -2) { nt = row_next(row_of(top)); if (nt != NONE && pos + rows < total) top = nt; }
            else if (p >= 16) top = row_at_index((u16)(p - 16));
        }
        return 1;
    }
    if (!wrap && sy >= ey + eh && sy < ey + eh + 16 && sx >= ex && sx < ex + ew) {
        if (kind == MOUSE_DOWN) {
            int longest = longest_line() + 1;
            if (sx < ex + 16) { if (hcol) hcol--; }
            else if (sx >= ex + ew - 16) { if ((int)hcol + cols < longest) hcol++; }
            else if (sx < ex + ew / 2) hcol = hcol > (u16)cols ? hcol - cols : 0;
            else if ((int)hcol + cols < longest) hcol += cols;
        }
        return 1;
    }
    if (kind == MOUSE_DOWN && sx >= ex && sx < ex + ew && sy >= ey && sy < ey + eh) {
        u16 o = offset_at(sx, sy);
        typing_group = 0;
        goal = NONE;
        if (o == last_click_off && HOST.ticks - last_click_tick < 9) {
            select_word(o);
            last_click_off = NONE;
            return 1;
        }
        last_click_off = o;
        last_click_tick = HOST.ticks;
        caret = o;
        if (!(HOST.shift & SH_SHIFT)) anchor = o;
        dragging = 1;
        return 1;
    }
    if (kind == MOUSE_MOVE && dragging) {
        if (sy < ey + 2) { u16 p = row_prev(row_of(top)); if (p != NONE) top = p; }
        if (sy >= ey + eh - 2) { u16 p = row_next(row_of(top)); if (p != NONE) top = p; }
        caret = offset_at(sx, sy);
        return 1;
    }
    if (kind == MOUSE_UP) { dragging = 0; return 1; }
    return 0;
}

static int on_key(int key)
{
    int s = KEY_SCAN(key), ch = KEY_CHAR(key), shift = HOST.shift;
    int ext = (shift & SH_SHIFT) != 0, ctrl = (shift & SH_CTRL) != 0, letter, r;
    layout();
    if (dlg.open) {
        int before = dlg_kind;
        if ((before == D_OPEN || before == D_SAVEAS) && !ch && (s == K_UP || s == K_DOWN)) {
            if (s == K_UP && fd_sel > 0) fd_sel--;
            if (s == K_DOWN && fd_sel + 1 < fd_count) fd_sel++;
            if (fd_sel < 0 && fd_count) fd_sel = 0;
            if (fd_sel < fd_top) fd_top = fd_sel;
            if (fd_sel >= fd_top + FD_ROWS) fd_top = fd_sel - FD_ROWS + 1;
            if (fd_sel >= 0) {
                field_set(&f1, b1, 60, fd_list[fd_sel].name);
                if (fd_list[fd_sel].dir && ch == 0) { /* Enter opens it */ }
            }
            return 1;
        }
        if ((before == D_OPEN || before == D_SAVEAS) && ch == 13 && dlg.focus == 4 &&
            fd_sel >= 0 && fd_list[fd_sel].dir && !str_icmp(f1.text, fd_list[fd_sel].name)) {
            fd_enter(fd_list[fd_sel].name);
            f1.text[0] = 0; f1.len = f1.cursor = 0;
            return 1;
        }
        if ((before == D_OPEN || before == D_SAVEAS) && ch == 8 && dlg.focus == 4 && !f1.len) {
            fd_enter("..");
            return 1;
        }
        r = dialog_key(&dlg, key, shift);
        if (r >= 0) dialog_result(r);
        return 1;
    }
    if (context.open) {
        r = popup_key(&context, key);
        if (r >= 0) command(r);
        return 1;
    }
    r = menubar_key(&bar, key, shift);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    if (key == 0x5D00) {                          /* Shift+F10, Menu key */
        update_menus();
        popup_open(&context, context_items, 8, ex + 40, ey + 40, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
        return 1;
    }
    letter = ctrl ? key_ctrl_letter(key) : 0;
    if (letter) {
        switch (letter) {
        case 'N': command(M_NEW); return 1;
        case 'O': command(M_OPEN); return 1;
        case 'S': command(M_SAVE); return 1;
        case 'P': command(M_PRINT); return 1;
        case 'Z': command(M_UNDO); return 1;
        case 'X': command(M_CUT); return 1;
        case 'C': command(M_COPY); return 1;
        case 'V': command(M_PASTE); return 1;
        case 'F': command(M_FIND); return 1;
        case 'H': command(M_REPLACE); return 1;
        case 'G': command(M_GOTO); return 1;
        case 'A': command(M_SELALL); return 1;
        }
        return 1;
    }
    if (!ch || ch == 0xE0) {
        switch (s) {
        case K_F1: command(M_HELP); return 1;
        case K_F3: command(M_FINDNEXT); return 1;
        case K_F5: command(M_TIMEDATE); return 1;
        case K_LEFT: case 0x73: move_caret(ctrl ? word_left(caret) : (!ext && has_sel() ? sel_a() : left_of(caret)), ext); goal = NONE; break;
        case K_RIGHT: case 0x74: move_caret(ctrl ? word_right(caret) : (!ext && has_sel() ? sel_b() : right_of(caret)), ext); goal = NONE; break;
        case K_UP: vertical(-1, ext); break;
        case K_DOWN: vertical(1, ext); break;
        case K_PGUP: vertical(-(rows - 1), ext); break;
        case K_PGDN: vertical(rows - 1, ext); break;
        case K_HOME: case 0x77: move_caret(ctrl ? 0 : row_of(caret), ext); goal = NONE; break;
        case K_END: case 0x75: move_caret(ctrl ? tlen : row_end(row_of(caret)), ext); goal = NONE; break;
        case K_DEL:
            if (shift & SH_SHIFT) command(M_CUT); else command(M_DELETE);
            break;
        case K_INS:
            if (shift & SH_SHIFT) command(M_PASTE); else if (ctrl) command(M_COPY);
            break;
        case 0x92: command(M_COPY); break;                 /* Ctrl+Ins */
        case 0x93: command(M_DELETE); break;
        default: return 0;
        }
        ensure_visible();
        return 1;
    }
    if (ch == 8) {
        if (has_sel()) { begin_edit(0); delete_selection(); }
        else if (caret) {
            u16 p = left_of(caret);
            begin_edit(0);
            delete_range(p, caret - p);
            caret = anchor = p;
        }
        ensure_visible();
        return 1;
    }
    if (key == K_ESC) return 1;
    if (ch == 13) { replace_with("\r\n", 2, 0); ensure_visible(); return 1; }
    if (ch == '\t' || (ch >= ' ' && ch != 127)) {
        char c = (char)ch;
        replace_with(&c, 1, 1);
        ensure_visible();
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
static void release_memory(void)
{
    if (tseg) dos_free(tseg);
    if (useg) dos_free(useg);
    if (cseg) dos_free(cseg);
    tseg = useg = cseg = 0;
}

static int notepad_event(int ev, int a, int b, int c);
int app_event(int ev, int a, int b, int c)
{
    int r, orig = ev;
    if (ev == EV_OPEN && a == 2) { dlg.win = 0; dialog_sync(&dlg); return 1; }
    r = dialog_pre(&dlg, WIN_NOTEPAD, &ev, &a);
    if (r >= 0) return r;
    if (dialog_mine(&dlg) && ev == EV_PAINT) { paint_dialog(); return 0; }
    if (ev == EV_MOUSE && a == MOUSE_HOVER) {
        int sx = HOST.x + b, sy = HOST.y + TITLE_H + c;
        ui_dirty = 0;
        if (dialog_mine(&dlg)) dialog_hover(&dlg, sx, sy);
        else if (dlg.open) return 0;
        else if (context.open) popup_mouse(&context, MOUSE_HOVER, sx, sy);
        else if (menubar_open(&bar)) { layout(); menubar_mouse(&bar, MOUSE_HOVER, sx, sy); }
        return ui_dirty;
    }
    r = notepad_event(ev, a, b, c);
    r = dialog_post(&dlg, WIN_NOTEPAD, ev, r);
    return orig == EV_CLOSE && ev != EV_CLOSE ? 0 : r;   /* a dialog's close box */
}
static int notepad_event(int ev, int a, int b, int c)
{
    switch (ev) {
    case EV_OPEN:
        if (!tseg) {
            tseg = dos_alloc(0x0F01);
            if (!tseg) { app_sound(5); app_close(); return 0; }
            new_document();
            HDR_WIDTH = HOST.screen_w - 60 > 640 ? 640 : HOST.screen_w - 60;
            HDR_HEIGHT = HOST.screen_h - 100 > 440 ? 440 : HOST.screen_h - 100;
        }
        if (APP_ARG[0]) {
            char arg[PATH_MAX];
            str_ncopy(arg, APP_ARG, PATH_MAX);
            APP_ARG[0] = 0;
            if (a == 1 || !dirty) {
                int r = load_file(arg);
                if (r < 0) {
                    char error_code[8];
                    fmt_u32(error_code, (u32)-r);
                    app_log("[CIUKNOTE] open error", error_code);
                }
                if (r == 1) message("CiukNote", "This file is too large for CiukNote.\nCiukNote opens files of up to 61,440 bytes.");
                else if (r && a != 1) {
                    /* A new name: an empty document that will be saved there. */
                    new_document();
                    str_ncopy(path, arg, PATH_MAX);
                    update_title();
                }
            } else {
                str_ncopy(pending_path, arg, PATH_MAX);
                save_prompt(A_OPEN_PATH);
            }
        }
        update_title();
        return 1;
    case EV_PAINT:
        paint();
        return 0;
    case EV_WHEEL: {
        int at, last;
        if (dlg.open || c < 0) return 0;
        layout();
        at = row_index(row_of(top)) + a * 3;
        last = (int)row_count() - rows;
        if (at > last) at = last;
        if (at < 0) at = 0;
        top = row_at_index((u16)at);
        return 1;
    }
    case EV_KEY:
        return on_key(a);
    case EV_MOUSE:
        return on_mouse(a, b, c);
    case EV_POLL:
        return 0;
    case EV_CLOSE:
        if (dirty && !closing) { save_prompt(A_EXIT); return 1; }
        release_memory();
        closing = 0;
        return 0;
    case EV_SUSPEND:
        if (dirty) return 1;               /* unsaved text stays in memory */
        str_copy(APP_ARG, path);
        release_memory();
        return 0;
    }
    return 0;
}
