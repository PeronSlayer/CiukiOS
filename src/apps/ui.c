/* Toolkit of the desktop application modules: menus (bar, pull-down and
 * context), edit fields, scroll bars and small marks. Everything is drawn
 * through the shell's primitives; see app.h. */
#include "app.h"

#define ITEM_H 20
#define SEP_H  8

int ui_dirty;
static void place(struct dialog *d);

/* Label without '&'; *mn = mnemonic (upper case) and *at = its index. */
static void strip_amp(const char *s, char *out, char *mn, int *at)
{
    int n = 0;
    *mn = 0; *at = -1;
    while (*s && n < 63) {
        if (*s == '&' && s[1]) {
            s++;
            if (!*mn) { *mn = to_upper(*s); *at = n; }
        }
        out[n++] = *s++;
    }
    out[n] = 0;
}

static void underline(int x, int y, const char *label, int at, int color)
{
    char t[64];
    int x0, w;
    if (at < 0) return;
    str_ncopy(t, label, at + 1);
    x0 = ui_measure(t);
    str_ncopy(t, label + at, 2);
    w = ui_measure(t);
    ui_rect(x + x0, y + 14, w, 1, color);
}

int text_fit(const char *s, int w, char *out)
{
    int n = str_len(s);
    if (n > 63) n = 63;
    str_ncopy(out, s, n + 1);
    if (ui_measure(out) <= w) return n;
    while (n > 0) {
        n--;
        str_ncopy(out, s, n + 1);
        str_cat(out, "...");
        if (ui_measure(out) <= w) return n;
    }
    out[0] = 0;
    return 0;
}

void draw_frame_text(int x, int y, int w, const char *s, int color)
{
    char t[72];
    text_fit(s, w, t);
    ui_text(x, y, t, color);
}

void draw_check(int x, int y, int on)
{
    ui_inset(x, y, 13, 13);
    if (on) {
        ui_rect(x + 3, y + 6, 2, 3, C_INK);
        ui_rect(x + 5, y + 8, 2, 2, C_INK);
        ui_rect(x + 7, y + 4, 2, 5, C_INK);
        ui_rect(x + 9, y + 3, 1, 3, C_INK);
    }
}

void draw_radio(int x, int y, int on)
{
    ui_rect(x + 3, y, 7, 1, C_SHADOW);
    ui_rect(x + 1, y + 1, 11, 11, C_PAPER);
    ui_rect(x, y + 3, 1, 7, C_SHADOW);
    ui_rect(x + 12, y + 3, 1, 7, C_FACE);
    ui_rect(x + 3, y + 12, 7, 1, C_FACE);
    if (on) ui_rect(x + 4, y + 4, 5, 5, C_INK);
}

void draw_focus(int x, int y, int w, int h)
{
    int i;
    for (i = 0; i < w; i += 2) { ui_rect(x + i, y, 1, 1, C_INK); ui_rect(x + i, y + h - 1, 1, 1, C_INK); }
    for (i = 0; i < h; i += 2) { ui_rect(x, y + i, 1, 1, C_INK); ui_rect(x + w - 1, y + i, 1, 1, C_INK); }
}

static void check_mark(int x, int y, int color)
{
    ui_rect(x, y + 5, 2, 3, color);
    ui_rect(x + 2, y + 7, 2, 2, color);
    ui_rect(x + 4, y + 3, 2, 5, color);
    ui_rect(x + 6, y + 1, 2, 3, color);
}

/* ---------------- popup menus ---------------- */
static int item_h(struct menu_item *it) { return (it->flags & MI_SEP) ? SEP_H : ITEM_H; }

void popup_open(struct popup *p, struct menu_item *items, int count,
                int x, int y, int max_x, int max_y)
{
    int i, lw = 0, kw = 0, h = 6;
    char t[64], mn;
    int at;
    for (i = 0; i < count; i++) {
        if (items[i].flags & MI_SEP) { h += SEP_H; continue; }
        strip_amp(items[i].label, t, &mn, &at);
        if (ui_measure(t) > lw) lw = ui_measure(t);
        if (items[i].key && ui_measure(items[i].key) > kw) kw = ui_measure(items[i].key);
        h += ITEM_H;
    }
    p->w = 22 + lw + (kw ? 28 + kw : 0) + 16;
    p->h = h;
    if (x + p->w > max_x) x = max_x - p->w;
    if (y + p->h > max_y) y = max_y - p->h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    p->x = x; p->y = y;
    p->items = items; p->count = count;
    p->hover = -1;
    p->open = 1;
}

void popup_draw(struct popup *p)
{
    int i, y;
    char t[64], mn;
    int at;
    if (!p->open) return;
    ui_rect(p->x + 3, p->y + 3, p->w, p->h, C_SHADOW);
    ui_bevel(p->x, p->y, p->w, p->h, C_FACE);
    y = p->y + 3;
    for (i = 0; i < p->count; i++) {
        struct menu_item *it = &p->items[i];
        if (it->flags & MI_SEP) {
            ui_rect(p->x + 4, y + 3, p->w - 8, 1, C_SHADOW);
            ui_rect(p->x + 4, y + 4, p->w - 8, 1, C_PAPER);
            y += SEP_H;
            continue;
        }
        {
            int fg = (it->flags & MI_DISABLED) ? C_SHADOW : C_INK;
            if (i == p->hover && !(it->flags & MI_DISABLED)) {
                ui_rect(p->x + 3, y, p->w - 6, ITEM_H, C_TITLE);
                fg = C_PAPER;
            }
            if (it->flags & MI_CHECKED) check_mark(p->x + 8, y + 5, fg);
            strip_amp(it->label, t, &mn, &at);
            ui_text(p->x + 22, y + 2, t, fg);
            underline(p->x + 22, y + 2, t, at, fg);
            if (it->key) ui_text(p->x + p->w - 12 - ui_measure(it->key), y + 2, it->key, fg);
        }
        y += ITEM_H;
    }
}

static int popup_item_at(struct popup *p, int sy)
{
    int i, y = p->y + 3;
    for (i = 0; i < p->count; i++) {
        int h = item_h(&p->items[i]);
        if (sy >= y && sy < y + h) return (p->items[i].flags & MI_SEP) ? -1 : i;
        y += h;
    }
    return -1;
}

int popup_mouse(struct popup *p, int kind, int sx, int sy)
{
    int inside, i;
    if (!p->open) return -2;
    inside = sx >= p->x && sx < p->x + p->w && sy >= p->y && sy < p->y + p->h;
    if (!inside) {
        if (kind == MOUSE_DOWN || kind == MOUSE_RIGHT) { p->open = 0; return -2; }
        if ((kind == MOUSE_MOVE || kind == MOUSE_HOVER) && p->hover >= 0) {
            p->hover = -1;
            ui_dirty = 1;
        }
        return -1;
    }
    i = popup_item_at(p, sy);
    if (i >= 0 && (p->items[i].flags & MI_DISABLED)) i = -1;
    if (p->hover != i) ui_dirty = 1;
    p->hover = i;
    if (kind == MOUSE_HOVER) return -1;
    if ((kind == MOUSE_UP || kind == MOUSE_DOWN) && i >= 0 &&
        !(p->items[i].flags & MI_DISABLED)) {
        p->open = 0;
        return p->items[i].id;
    }
    return -1;
}

int popup_key(struct popup *p, int key)
{
    int s = KEY_SCAN(key), i, n;
    char t[64], mn;
    int at;
    if (!p->open) return -2;
    if (key == K_ESC) { p->open = 0; return -2; }
    if (s == K_UP || s == K_DOWN) {
        int step = (s == K_UP) ? -1 : 1;
        i = p->hover;
        for (n = 0; n < p->count; n++) {
            i += step;
            if (i < 0) i = p->count - 1;
            if (i >= p->count) i = 0;
            if (!(p->items[i].flags & (MI_SEP | MI_DISABLED))) break;
        }
        p->hover = i;
        return -1;
    }
    if (KEY_CHAR(key) == 13 && p->hover >= 0) {
        p->open = 0;
        return p->items[p->hover].id;
    }
    if (KEY_CHAR(key) > ' ') {
        for (i = 0; i < p->count; i++) {
            if (p->items[i].flags & (MI_SEP | MI_DISABLED)) continue;
            strip_amp(p->items[i].label, t, &mn, &at);
            if (mn && mn == to_upper((char)KEY_CHAR(key))) { p->open = 0; return p->items[i].id; }
        }
    }
    return -1;
}

/* ---------------- menu bar ---------------- */
static int title_x(struct menubar *m, int index, int *w)
{
    int i, x = m->x + 4;
    char t[64], mn;
    int at;
    for (i = 0; i < m->count; i++) {
        int tw;
        strip_amp(m->menus[i].title, t, &mn, &at);
        tw = ui_measure(t) + 16;
        if (i == index) { *w = tw; return x; }
        x += tw;
    }
    *w = 0;
    return x;
}

static void open_menu(struct menubar *m, int index)
{
    int w, x = title_x(m, index, &w);
    m->active = index;
    popup_open(&m->pop, m->menus[index].items, m->menus[index].count,
               x, m->y + MENUBAR_H, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
    if (m->keyboard) m->pop.hover = 0;
    while (m->pop.hover >= 0 && m->pop.hover < m->pop.count &&
           (m->pop.items[m->pop.hover].flags & (MI_SEP | MI_DISABLED))) m->pop.hover++;
}

int menubar_open(struct menubar *m) { return m->active >= 0 || m->keyboard; }

void menubar_draw(struct menubar *m)
{
    int i;
    char t[64], mn;
    int at;
    ui_rect(m->x, m->y, m->w, MENUBAR_H, C_FACE);
    ui_rect(m->x, m->y + MENUBAR_H - 1, m->w, 1, C_SHADOW);
    for (i = 0; i < m->count; i++) {
        int w, x = title_x(m, i, &w);
        int fg = C_INK;
        strip_amp(m->menus[i].title, t, &mn, &at);
        if (i == m->active || (m->keyboard && m->active < 0 && i == 0)) {
            ui_rect(x, m->y + 1, w, MENUBAR_H - 2, C_TITLE);
            fg = C_PAPER;
        }
        ui_text(x + 8, m->y + 2, t, fg);
        underline(x + 8, m->y + 2, t, at, fg);
    }
    if (m->active >= 0) popup_draw(&m->pop);
}

/* -3 not for the bar, -1 used, >= 0 chosen command. */
int menubar_mouse(struct menubar *m, int kind, int sx, int sy)
{
    int i, w, x;
    if (sy >= m->y && sy < m->y + MENUBAR_H && sx >= m->x && sx < m->x + m->w) {
        for (i = 0; i < m->count; i++) {
            x = title_x(m, i, &w);
            if (sx >= x && sx < x + w) {
                if (kind == MOUSE_DOWN) {
                    if (m->active == i) { m->active = -1; m->pop.open = 0; }
                    else { m->keyboard = 0; open_menu(m, i); }
                } else if ((kind == MOUSE_MOVE || kind == MOUSE_HOVER) &&
                           m->active >= 0 && m->active != i) {
                    open_menu(m, i);
                    ui_dirty = 1;
                }
                return -1;
            }
        }
        if (kind == MOUSE_DOWN && m->active >= 0) { m->active = -1; m->pop.open = 0; return -1; }
        return m->active >= 0 ? -1 : -3;
    }
    if (m->active < 0) return -3;
    i = popup_mouse(&m->pop, kind, sx, sy);
    if (i == -2) { m->active = -1; m->keyboard = 0; return -1; }
    if (i >= 0) { m->active = -1; m->keyboard = 0; return i; }
    return -1;
}

int menubar_key(struct menubar *m, int key, int shift)
{
    int s = KEY_SCAN(key), i, letter;
    char t[64], mn;
    int at;
    (void)shift;
    if (m->active < 0 && !m->keyboard) {
        letter = key_alt_letter(key);
        if (s == K_F10 && !KEY_CHAR(key)) { m->keyboard = 1; return -1; }
        if (!letter) return -3;
        for (i = 0; i < m->count; i++) {
            strip_amp(m->menus[i].title, t, &mn, &at);
            if (mn == letter) { m->keyboard = 1; open_menu(m, i); return -1; }
        }
        return -3;
    }
    if (m->active < 0) {                    /* F10: the bar has the focus */
        if (key == K_ESC || s == K_F10) { m->keyboard = 0; return -1; }
        if (s == K_DOWN || KEY_CHAR(key) == 13) { open_menu(m, 0); return -1; }
        if (s == K_RIGHT && m->count > 1) { open_menu(m, 0); return -1; }
        letter = to_upper((char)KEY_CHAR(key));
        if (!letter) letter = key_alt_letter(key);
        for (i = 0; i < m->count; i++) {
            strip_amp(m->menus[i].title, t, &mn, &at);
            if (mn && mn == letter) { open_menu(m, i); return -1; }
        }
        return -1;
    }
    if (s == K_LEFT && !KEY_CHAR(key)) { open_menu(m, (m->active + m->count - 1) % m->count); return -1; }
    if (s == K_RIGHT && !KEY_CHAR(key)) { open_menu(m, (m->active + 1) % m->count); return -1; }
    if (s == K_F10 && !KEY_CHAR(key)) { m->active = -1; m->pop.open = 0; m->keyboard = 0; return -1; }
    i = popup_key(&m->pop, key);
    if (i == -2) { m->active = -1; m->keyboard = 0; return -1; }
    if (i >= 0) { m->active = -1; m->keyboard = 0; return i; }
    return -1;
}

/* ---------------- scroll bar (vertical, 16 px) ---------------- */
static void arrow(int x, int y, int up, int color)
{
    int i;
    for (i = 0; i < 4; i++) {
        int w = up ? 1 + 2 * i : 7 - 2 * i;
        ui_rect(x + 7 - w / 2, y + 6 + i, w, 1, color);
    }
}

void draw_scroll(int x, int y, int h, long pos, long total, long page)
{
    int track = h - 32, th, ty, idle = total <= page || track < 8;
    ui_rect(x, y, 16, h, C_FACE);
    /* Nothing to scroll: a plain, disabled bar (grey arrows). */
    if (!idle) ui_rect(x, y + 16, 16, h - 32, C_LIGHT);
    ui_bevel(x, y, 16, 16, C_FACE);
    arrow(x, y, 1, idle ? C_SHADOW : C_INK);
    ui_bevel(x, y + h - 16, 16, 16, C_FACE);
    arrow(x, y + h - 16, 0, idle ? C_SHADOW : C_INK);
    if (idle) return;
    th = (int)((long)track * page / total);
    if (th < 12) th = 12;
    if (th > track) th = track;
    ty = (int)((long)(track - th) * pos / (total - page));
    ui_bevel(x, y + 16 + ty, 16, th, C_FACE);
}

/* New position for a click at my (screen y) on the bar. */
long scroll_hit(int x, int y, int h, int my, long total, long page)
{
    int track = h - 32, th;
    long max = total - page;
    (void)x;
    if (max <= 0) return 0;
    if (my < y + 16) return -1;                 /* one line up */
    if (my >= y + h - 16) return -2;            /* one line down */
    th = (int)((long)track * page / total);
    if (th < 12) th = 12;
    {
        long p = (long)(my - y - 16 - th / 2) * max / (track - th > 0 ? track - th : 1);
        if (p < 0) p = 0;
        if (p > max) p = max;
        return p + 16;                          /* >= 16: absolute pos + 16 */
    }
}

/* ---------------- edit field ---------------- */
void field_set(struct field *f, char *buffer, int max, const char *init)
{
    f->text = buffer;
    f->max = max;
    str_ncopy(buffer, init, max);
    f->len = str_len(buffer);
    f->cursor = f->len;
    f->sel = 0;
    f->scroll = 0;
}

int field_key(struct field *f, int key, int shift)
{
    int s = KEY_SCAN(key), c = KEY_CHAR(key);
    (void)shift;
    if (c == 1) { f->sel = f->len > 0; f->cursor = f->len; return 1; }     /* Ctrl+A */
    if (f->sel && ((c >= ' ' && c < 127) || c == 8 || s == K_DEL)) {
        f->len = 0; f->cursor = 0; f->text[0] = 0; f->sel = 0;
        if (c == 8 || s == K_DEL) return 1;
    }
    f->sel = 0;
    if (c >= ' ' && c < 127) {
        if (f->len + 1 >= f->max) return 0;
        mem_move(f->text + f->cursor + 1, f->text + f->cursor, f->len - f->cursor + 1);
        f->text[f->cursor++] = (char)c;
        f->len++;
        return 1;
    }
    if (c == 8) {
        if (!f->cursor) return 0;
        mem_move(f->text + f->cursor - 1, f->text + f->cursor, f->len - f->cursor + 1);
        f->cursor--; f->len--;
        return 1;
    }
    if (c) return 0;
    if (s == K_DEL && f->cursor < f->len) {
        mem_move(f->text + f->cursor, f->text + f->cursor + 1, f->len - f->cursor);
        f->len--;
        return 1;
    }
    if (s == K_LEFT && f->cursor) { f->cursor--; return 1; }
    if (s == K_RIGHT && f->cursor < f->len) { f->cursor++; return 1; }
    if (s == K_HOME) { f->cursor = 0; return 1; }
    if (s == K_END) { f->cursor = f->len; return 1; }
    return 0;
}

static int prefix_w(const char *s, int n)
{
    char t[130];
    if (n > 128) n = 128;
    str_ncopy(t, s, n + 1);
    return ui_measure(t);
}

void field_draw(struct field *f, int x, int y, int w, int focused)
{
    char t[130];
    int cx;
    ui_inset(x, y, w, 22);
    if (f->cursor < f->scroll) f->scroll = f->cursor;
    while (f->scroll < f->cursor && prefix_w(f->text + f->scroll, f->cursor - f->scroll) > w - 10)
        f->scroll++;
    text_fit(f->text + f->scroll, w - 8, t);
    if (f->sel && f->len) {
        ui_rect(x + 3, y + 3, ui_measure(t) + 2, 16, C_TITLE);
        ui_text(x + 4, y + 3, t, C_PAPER);
    } else {
        ui_text(x + 4, y + 3, t, C_INK);
    }
    if (focused) {
        cx = x + 4 + prefix_w(f->text + f->scroll, f->cursor - f->scroll);
        ui_rect(cx, y + 4, 1, 15, C_INK);
    }
}

void field_click(struct field *f, int x, int w, int mx)
{
    int i;
    (void)w;
    f->sel = 0;
    for (i = f->scroll; i <= f->len; i++) {
        if (x + 4 + prefix_w(f->text + f->scroll, i - f->scroll) > mx) { f->cursor = i > f->scroll ? i - 1 : i; return; }
    }
    f->cursor = f->len;
}

/* ---------------- dialogs ---------------- */
void dialog_show(struct dialog *d, const char *title, struct dctl *c, int n,
                 int w, int h, int def_id, int cancel_id)
{
    int i;
    d->title = title; d->c = c; d->n = n;
    d->w = w; d->h = h + DIALOG_TITLE_H;
    d->x = HOST.x + (HOST.w - d->w) / 2;
    d->y = HOST.y + TITLE_H + (HOST.h - TITLE_H - d->h) / 2;
    if (d->x < HOST.x + 4) d->x = HOST.x + 4;
    if (d->y < HOST.y + TITLE_H) d->y = HOST.y + TITLE_H;
    d->def_id = def_id; d->cancel_id = cancel_id;
    d->focus = -1;
    d->hot = -1;
    for (i = 0; i < n; i++) {
        if (c[i].type == DC_FIELD || c[i].type == DC_CHECK || c[i].type == DC_RADIO ||
            c[i].type == DC_BUTTON) {
            if (!c[i].disabled) { d->focus = i; break; }
        }
    }
    d->open = 1;
}

static int focusable(struct dctl *c)
{
    return !c->disabled && (c->type == DC_FIELD || c->type == DC_CHECK ||
                            c->type == DC_RADIO || c->type == DC_BUTTON);
}

void dialog_draw(struct dialog *d)
{
    int i, ox, oy;
    char t[64], mn;
    int at;
    if (!d->open) return;
    if (d->win) {
        /* The shell drew the frame and the title of its window. */
        if (HOST.window != d->win) return;
        place(d);
    } else {
        ui_rect(d->x + 4, d->y + 4, d->w, d->h, C_INK);
        ui_bevel(d->x, d->y, d->w, d->h, C_FACE);
        ui_rect(d->x + 3, d->y + 3, d->w - 6, DIALOG_TITLE_H - 3, C_TITLE);
        ui_text(d->x + 8, d->y + 4, d->title, C_PAPER | BOLD);
    }
    ox = d->x + 8;
    oy = d->y + DIALOG_TITLE_H + 6;
    for (i = 0; i < d->n; i++) {
        struct dctl *c = &d->c[i];
        int x = ox + c->x, y = oy + c->y;
        int fg = c->disabled ? C_SHADOW : C_INK;
        strip_amp(c->text ? c->text : "", t, &mn, &at);
        switch (c->type) {
        case DC_LABEL:
            ui_text(x, y, t, fg);
            underline(x, y, t, at, fg);
            break;
        case DC_GROUP:
            ui_rect(x, y + 7, c->w, 1, C_SHADOW);
            ui_rect(x, y + c->h - 1, c->w, 1, C_SHADOW);
            ui_rect(x, y + 7, 1, c->h - 7, C_SHADOW);
            ui_rect(x + c->w - 1, y + 7, 1, c->h - 7, C_SHADOW);
            ui_rect(x + 6, y, ui_measure(t) + 6, 16, C_FACE);
            ui_text(x + 9, y, t, fg);
            break;
        case DC_FIELD:
            field_draw(c->field, x, y, c->w, i == d->focus);
            break;
        case DC_CHECK:
            draw_check(x, y + 2, c->value);
            ui_text(x + 20, y, t, fg);
            underline(x + 20, y, t, at, fg);
            if (i == d->focus) draw_focus(x + 18, y, ui_measure(t) + 5, 17);
            break;
        case DC_RADIO:
            draw_radio(x, y + 2, c->value);
            ui_text(x + 20, y, t, fg);
            underline(x + 20, y, t, at, fg);
            if (i == d->focus) draw_focus(x + 18, y, ui_measure(t) + 5, 17);
            break;
        case DC_BUTTON: {
            int tx = x + (c->w - ui_measure(t)) / 2;
            if (c->id == d->def_id) ui_rect(x - 1, y - 1, c->w + 2, c->h + 2, C_INK);
            ui_bevel(x, y, c->w, c->h, i == d->hot && !c->disabled ? C_PAPER : C_FACE);
            ui_text(tx, y + (c->h - 16) / 2, t, fg);
            underline(tx, y + (c->h - 16) / 2, t, at, fg);
            if (i == d->focus) draw_focus(x + 3, y + 3, c->w - 6, c->h - 6);
            break;
        }
        }
    }
}

/* A windowed dialog follows its window: the client area is below the
 * shell's title rail, inside the 3-pixel frame. */
static void place(struct dialog *d)
{
    d->x = HOST.x + 3;
    d->y = HOST.y + TITLE_H - DIALOG_TITLE_H;
}

static int control_at(struct dialog *d, int sx, int sy)
{
    int i, ox = d->x + 8, oy = d->y + DIALOG_TITLE_H + 6;
    for (i = 0; i < d->n; i++) {
        struct dctl *c = &d->c[i];
        int x = ox + c->x, y = oy + c->y, w = c->w, h = c->h ? c->h : 17;
        if (c->type == DC_GROUP || c->type == DC_LABEL) continue;
        if (c->type == DC_CHECK || c->type == DC_RADIO) w = 20 + ui_measure(c->text) + 4;
        if (c->type == DC_FIELD) h = 22;
        if (sx >= x && sx < x + w && sy >= y && sy < y + h) return i;
    }
    return -1;
}

int dialog_hover(struct dialog *d, int sx, int sy)
{
    int i;
    if (!d->open || (d->win && HOST.window != d->win)) return 0;
    if (d->win) place(d);
    i = control_at(d, sx, sy);
    if (i >= 0 && (d->c[i].type != DC_BUTTON || d->c[i].disabled)) i = -1;
    if (i == d->hot) return 0;
    d->hot = i;
    ui_dirty = 1;
    return 1;
}

void dialog_place(struct dialog *d)
{
    if (d->open && d->win && HOST.window == d->win) place(d);
}

int dialog_mine(struct dialog *d)
{
    return d->open && d->win && HOST.window == d->win;
}

void dialog_sync(struct dialog *d)
{
    int ww, wh;
    if (!d->open) {
        if (d->win) win_close(d->win);
        d->win = 0;
        return;
    }
    ww = d->w + 6;
    wh = d->h - DIALOG_TITLE_H + TITLE_H + 3;
    if (d->win && (ww != d->ww || wh != d->wh)) { win_close(d->win); d->win = 0; }
    if (!d->win) {
        d->win = win_open(-1, -1, ww, wh, d->title, WS_DIALOG);
        d->ww = ww; d->wh = wh;
        d->hot = -1;
        ui_repaint();
    }
}

static void radio_select(struct dialog *d, int i)
{
    int k;
    for (k = 0; k < d->n; k++)
        if (d->c[k].type == DC_RADIO && d->c[k].group == d->c[i].group) d->c[k].value = 0;
    d->c[i].value = 1;
}

/* A control was activated: its result (button id) or -1. */
static int activate(struct dialog *d, int i)
{
    struct dctl *c = &d->c[i];
    if (c->disabled) return -1;
    d->focus = i;
    if (c->type == DC_CHECK) { c->value = !c->value; return -1; }
    if (c->type == DC_RADIO) { radio_select(d, i); return -1; }
    if (c->type == DC_BUTTON) { d->open = 0; return c->id; }
    return -1;
}

int dialog_mouse(struct dialog *d, int kind, int sx, int sy)
{
    int i;
    if (!d->open) return -1;
    if (d->win && HOST.window != d->win) {
        /* Modal: a click on its owner brings the dialog forward. */
        if (kind == MOUSE_DOWN || kind == MOUSE_RIGHT) app_window_cmd(d->win, 1);
        return -1;
    }
    if (d->win) place(d);
    if (kind == MOUSE_HOVER || kind == MOUSE_MOVE) { dialog_hover(d, sx, sy); return -1; }
    if (kind != MOUSE_DOWN) return -1;
    i = control_at(d, sx, sy);
    if (i < 0) return -1;
    {
        struct dctl *c = &d->c[i];
        if (c->type == DC_FIELD) {
            if (!c->disabled) { d->focus = i; field_click(c->field, d->x + 8 + c->x, c->w, sx); }
            return -1;
        }
        if (focusable(c)) return activate(d, i);
    }
    return -1;
}

int dialog_key(struct dialog *d, int key, int shift)
{
    int s = KEY_SCAN(key), ch = KEY_CHAR(key), i, k, letter;
    char t[64], mn;
    int at;
    if (!d->open) return -1;
    if (key == K_ESC) { d->open = 0; return d->cancel_id; }
    if (s == 0x0F) {                              /* Tab, Shift+Tab */
        int step = (shift & SH_SHIFT) || ch == 0 ? -1 : 1;
        if (key == K_TAB && !(shift & SH_SHIFT)) step = 1;
        i = d->focus;
        for (k = 0; k < d->n; k++) {
            i = (i + step + d->n) % d->n;
            if (focusable(&d->c[i])) break;
        }
        d->focus = i;
        if (d->c[i].type == DC_FIELD) d->c[i].field->sel = 1;
        return -1;
    }
    if (ch == 13) {
        if (d->focus >= 0 && d->c[d->focus].type == DC_BUTTON) return activate(d, d->focus);
        d->open = 0;
        return d->def_id;
    }
    letter = key_alt_letter(key);
    if (letter || (ch > ' ' && d->focus >= 0 && d->c[d->focus].type != DC_FIELD)) {
        if (!letter) letter = to_upper((char)ch);
        for (i = 0; i < d->n; i++) {
            strip_amp(d->c[i].text ? d->c[i].text : "", t, &mn, &at);
            if (!mn || mn != letter) continue;
            /* A label names the control after it (a field). */
            k = i;
            if (d->c[i].type == DC_LABEL && i + 1 < d->n) k = i + 1;
            if (!focusable(&d->c[k])) continue;
            if (d->c[k].type == DC_FIELD) { d->focus = k; d->c[k].field->sel = 1; return -1; }
            return activate(d, k);
        }
        if (key_alt_letter(key)) return -1;
    }
    if (d->focus < 0) return -1;
    {
        struct dctl *c = &d->c[d->focus];
        if (c->type == DC_FIELD) { field_key(c->field, key, shift); return -1; }
        if (ch == ' ') return activate(d, d->focus);
        if (c->type == DC_RADIO && (s == K_UP || s == K_DOWN || s == K_LEFT || s == K_RIGHT) && !ch) {
            int step = (s == K_UP || s == K_LEFT) ? -1 : 1;
            i = d->focus;
            for (k = 0; k < d->n; k++) {
                i = (i + step + d->n) % d->n;
                if (d->c[i].type == DC_RADIO && d->c[i].group == c->group && !d->c[i].disabled) break;
            }
            d->focus = i;
            radio_select(d, i);
        }
    }
    return -1;
}

int dialog_value(struct dialog *d, int index) { return d->c[index].value; }

/* ---------------- message box ---------------- */
static struct dctl mb_c[12];
static char mb_lines[6][80];
static const char *mb_names[] = { "", "OK", "&Yes", "&No", "Cancel", "&Save", "Do&n't Save", "Yes to &All" };

void msgbox(struct dialog *d, const char *title, const char *text, const char *buttons)
{
    int n = 0, lines = 0, i, w = 0, bw, bx, h, ids[4], nb = 0;
    const char *p = text;
    while (*p && lines < 6) {
        int k = 0;
        while (*p && *p != '\n' && k < 79) mb_lines[lines][k++] = *p++;
        mb_lines[lines][k] = 0;
        if (*p == '\n') p++;
        if (ui_measure(mb_lines[lines]) > w) w = ui_measure(mb_lines[lines]);
        lines++;
    }
    p = buttons;
    while (*p && nb < 4) {
        const char *q = p;
        int len = 0;
        while (q[len] && q[len] != '|') len++;
        for (i = 1; i <= 7; i++) {
            char plain[16], mn;
            int at;
            strip_amp(mb_names[i], plain, &mn, &at);
            if (str_len(plain) == len && !str_nicmp(plain, p, len)) break;
        }
        ids[nb++] = i <= 7 ? i : MB_OK;
        p += len;
        if (*p == '|') p++;
    }
    for (i = 0; i < lines; i++) {
        mb_c[n].type = DC_LABEL; mb_c[n].x = 50; mb_c[n].y = 6 + i * 18;
        mb_c[n].w = 0; mb_c[n].h = 16; mb_c[n].text = mb_lines[i]; mb_c[n].disabled = 0;
        n++;
    }
    bw = 86;
    if (w + 70 < nb * (bw + 8) + 16) w = nb * (bw + 8) + 16 - 70;
    h = 6 + lines * 18 + 16;
    bx = (w + 70 - nb * (bw + 8)) / 2;
    for (i = 0; i < nb; i++) {
        mb_c[n].type = DC_BUTTON; mb_c[n].x = bx + i * (bw + 8); mb_c[n].y = h;
        mb_c[n].w = bw; mb_c[n].h = 24; mb_c[n].text = mb_names[ids[i]]; mb_c[n].id = ids[i];
        mb_c[n].disabled = 0;
        n++;
    }
    dialog_show(d, title, mb_c, n, w + 76, h + 36, ids[0],
                nb > 1 ? ids[nb - 1] : ids[0]);
    d->focus = lines;                            /* the first button */
}

/* ---------------- a module's modal dialog window ---------------- */
/* Before the module handles an event: -1 go on (ev and a may now be an Esc
 * key: the dialog's close box), else the answer. */
int dialog_pre(struct dialog *d, int main_win, int *ev, int *a)
{
    if (dialog_mine(d)) {
        if (*ev == EV_CLOSE) { d->win = 0; *ev = EV_KEY; *a = K_ESC; }
        return -1;
    }
    if (*ev == EV_CLOSE && d->open && d->win && HOST.window == main_win) {
        app_window_cmd(d->win, 1);             /* answer the dialog first */
        return 1;
    }
    return -1;
}

/* After it: the owner shows what the dialog changed; the window follows. */
int dialog_post(struct dialog *d, int main_win, int ev, int r)
{
    if (r && ev != EV_PAINT && ev != EV_CLOSE && HOST.window != main_win)
        ui_repaint_win(main_win);
    dialog_sync(d);
    return r;
}
