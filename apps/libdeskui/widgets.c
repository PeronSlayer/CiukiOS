/* SPDX-License-Identifier: MIT
 * Widget registry, input dispatch, focus/release, timers and simple widgets. */
#include "internal.h"
#include "textarea.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef __linux__
/* Host ABI headers also name the native sysconf selectors. */
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#endif
#include <unistd.h>

static char *clipboard;
static char clipboard_path[DU_TEXT_MAX] = "/system/settings/text.clip";
static unsigned clipboard_serial;
int du_clipboard_path(const char *path)
{
    if (!path) path = "/system/settings/text.clip";
    if (strlen(path) + 40 >= sizeof(clipboard_path)) return -1;
    strcpy(clipboard_path, path); free(clipboard); clipboard = NULL;
    return 0;
}
int du_clipboard_set_n(const char *s, size_t n)
{
    if ((!s && n) || n > DU_CLIPBOARD_MAX || (n && memchr(s, 0, n))) return -1;
    char *next = malloc(n + 1);
    if (!next) return -1;
    if (n) memcpy(next, s, n);
    next[n] = 0;
    /* As with the file clipboard, publish a complete replacement. Keep a
     * local copy when storage is unavailable, including read-only sessions. */
    char temp[DU_TEXT_MAX + 40];
    int fd = -1;
    for (unsigned i = 0; i < 32; ++i) {
        snprintf(temp, sizeof(temp), "%s.%lu-%u.tmp", clipboard_path, (unsigned long)getpid(), ++clipboard_serial);
        fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    struct stat previous;
    if (fd < 0 && !stat(clipboard_path, &previous)) { free(next); return -1; }
    if (fd >= 0) {
        size_t used = 0; int failed = 0;
        while (used < n) {
            ssize_t written = write(fd, next + used, n - used);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { failed = 1; break; }
            used += (size_t)written;
        }
        if (!failed && fsync(fd)) failed = 1;
        if (close(fd)) failed = 1;
        if (!failed && rename(temp, clipboard_path)) failed = 1;
        if (failed) { unlink(temp); free(next); return -1; }
    }
    free(clipboard); clipboard = next;
    return 0;
}
void du_clipboard_set(const char *s) { (void)du_clipboard_set_n(s, s ? strlen(s) : 0); }
const char *du_clipboard_get(void)
{
    FILE *f = fopen(clipboard_path, "rb");
    if (f) {
        char *next = NULL; long size = -1;
        if (!fseek(f, 0, SEEK_END)) size = ftell(f);
        if (size >= 0 && (unsigned long)size <= DU_CLIPBOARD_MAX && !fseek(f, 0, SEEK_SET)) {
            next = malloc((size_t)size + 1);
            if (next && (fread(next, 1, (size_t)size, f) != (size_t)size || ferror(f) || memchr(next, 0, (size_t)size))) {
                free(next); next = NULL;
            }
        }
        if (fclose(f)) { free(next); next = NULL; }
        if (next) { next[size] = 0; free(clipboard); clipboard = next; }
    }
    return clipboard ? clipboard : "";
}

/* The shell's drag data object (apps/files/transfer.c fm_drag_save): "X",
 * then one "<D|F>/path" line per item. */
static char drop_source[DU_TEXT_MAX] = DESK_DRAG_FILE;
int du_drop_source(const char *path)
{
    if (!path || !*path || strlen(path) >= sizeof(drop_source)) return -1;
    strcpy(drop_source, path); return 0;
}
int du_drop_file(unsigned index, char *out, size_t size) { return du_drop_entry(index, out, size, NULL); }
int du_drop_entry(unsigned index, char *out, size_t size, int *folder)
{
    if (out && size) out[0] = 0;
    if (folder) *folder = 0;
    int fd = open(drop_source, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    char block[256], line[DU_TEXT_MAX + 2]; size_t used = 0; unsigned lines = 0; int result = 0;
    for (;;) {
        ssize_t got = read(fd, block, sizeof(block));
        if (got < 0) { if (errno == EINTR) continue; result = -errno; break; }
        if (!got) { if (used) result = -EINVAL; break; }
        for (ssize_t k = 0; k < got && !result; k++) {
            char c = block[k];
            if (c != '\n') { if (!c || used + 1 >= sizeof(line)) result = -EINVAL; else line[used++] = c; continue; }
            line[used] = 0;
            if (!lines) { if (strcmp(line, "X")) result = -EINVAL; }
            else if ((line[0] != 'D' && line[0] != 'F') || line[1] != '/') result = -EINVAL;
            else if (lines - 1 == index) { if (out && size) snprintf(out, size, "%s", line + 1); if (folder) *folder = line[0] == 'D'; }
            lines++; used = 0;
        }
        if (result) break;
    }
    close(fd);
    if (result) { if (out && size) out[0] = 0; return result; }
    return lines ? (int)(lines - 1) : -EINVAL;
}

void du_emit(struct du_ui *u, struct du_event e)
{
    if (!e.x && !e.y) { e.x = u->mouse_x; e.y = u->mouse_y; }
    if (!e.modifiers) e.modifiers = u->modifiers;
    /* A timer notifies the application; only changed content needs paint.
     * Preserve any pending full, dialog-only or bounded invalidation. */
    if (e.type != DU_EV_TICK && e.type != DU_EV_POINTER && e.type != DU_EV_KEY && e.type != DU_EV_FOCUS)
    {
        struct du_widget *w=du_get(u,e.id);
        if (!(e.type==DU_EV_CHANGE && w && w->kind==DU_TEXTAREA)) du_invalidate_widget(u,e.id);
    }
    if (u->event) u->event(u, &e, u->user);
}

void du_post(struct du_ui *u, enum du_event_type type, int id, int index)
{ du_emit(u, (struct du_event){.type = type, .id = id, .index = index}); }

void du_init(struct du_ui *u, uint32_t *pixels, int width, int height)
{
    memset(u, 0, sizeof(*u));
    u->canvas = (struct du_canvas){pixels, width, height, {0, 0, width, height}};
    u->focus = u->capture = u->hover = u->menu = u->popup_owner = -1;
    u->last_click_widget = -1; u->focused_window = 1; u->dirty = 1;
}

void du_resize(struct du_ui *u, uint32_t *pixels, int width, int height)
{
    int dx = width - u->canvas.width, dy = height - u->canvas.height;
    if (!pixels || width <= 0 || height <= 0) return;
    if (u->capture >= 0 && (unsigned)u->capture < u->count && u->widgets[u->capture].kind == DU_CANVAS)
        du_post(u, DU_EV_POINTER, u->widgets[u->capture].id, DU_POINTER_CANCEL);
    du_menu_close(u);
    if (u->combo_open) du_combo_drop(u,u->combo_open,0);
    du_tip_hide(u);
    u->capture = u->hover = -1;
    for (unsigned i = 0; i < u->count; i++) {
        struct du_widget *w = &u->widgets[i];
        w->pressed = w->hot = 0; w->drag = 0;
        if (w->kind == DU_TEXTAREA && w->data) du_ta_mouse_up(w->data);
        if (w->anchors & DU_ANCHOR_RIGHT) {
            if (w->anchors & DU_ANCHOR_LEFT) w->box.w += dx;
            else w->box.x += dx;
        }
        if (w->anchors & DU_ANCHOR_BOTTOM) {
            if (w->anchors & DU_ANCHOR_TOP) w->box.h += dy;
            else w->box.y += dy;
        }
    }
    u->canvas = (struct du_canvas){pixels, width, height, {0, 0, width, height}};
    if (u->layout) u->layout(u, u->user);
    /* Centre the modal panel on its resized owner, retaining its controls. */
    struct du_dialog *d = &u->dialog;
    if (d->kind) {
        int x = (width-d->box.w)/2, y = (height-d->box.h)/2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        for (unsigned i = 0; i < d->count; i++) {
            d->items[i].box.x += x - d->box.x;
            d->items[i].box.y += y - d->box.y;
            d->items[i].pressed = 0; d->items[i].drag = 0;
        }
        d->box.x = x; d->box.y = y; d->dragging = 0; d->capture = -1;
    }
    u->dirty = 1;
}

struct du_widget *du_add(struct du_ui *u, enum du_kind kind, int id, struct du_rect r, const char *text)
{
    if (u->count == DU_WIDGETS || du_get(u, id)) return NULL;
    struct du_widget *w = &u->widgets[u->count++];
    memset(w, 0, sizeof(*w));
    w->kind = kind; w->id = id; w->box = r; w->vertical = 1; w->sort = -1; w->focus_rect = 1;
    du_set_text(w,text); du_invalidate(u,r);
    return w;
}

struct du_widget *du_get(struct du_ui *u, int id)
{
    for (unsigned i = 0; i < u->count; i++) if (u->widgets[i].id == id) return &u->widgets[i];
    return NULL;
}

void du_set_text(struct du_widget *w, const char *s)
{
    snprintf(w->text, sizeof(w->text), "%s", s ? s : "");
    w->cursor = w->anchor = strlen(w->text); w->offset = 0;
    memset(&w->selection, 0, sizeof(w->selection));
}

int du_focusable(const struct du_widget *w)
{
    return !w->hidden && !w->disabled && (w->kind == DU_EDIT || w->kind == DU_LISTVIEW ||
           w->kind == DU_TREE || w->kind == DU_BUTTON || w->kind == DU_TABS || w->kind == DU_TEXTAREA ||
           w->kind == DU_CHECKBOX || w->kind == DU_RADIO || w->kind == DU_CANVAS ||
           w->kind == DU_COMBO || w->kind == DU_TRACKBAR);
}

int du_hit(const struct du_ui *u, int x, int y)
{
    for (unsigned i = u->count; i > 0; i--) {
        const struct du_widget *w = &u->widgets[i - 1];
        if (!w->hidden && du_contains(w->box, x, y)) return (int)i - 1;
    }
    return -1;
}
void du_cursor(struct du_ui *u, unsigned shape)
{ if (shape < DESK_CURSOR_COUNT) u->cursor = shape; }
static unsigned widget_cursor(const struct du_widget *w, const struct du_ui *u)
{
    if (w->disabled) return DESK_CURSOR_FORBIDDEN;
    if (w->pointer_cursor && w->pointer_cursor < DESK_CURSOR_COUNT)
        return !w->cursor_box.w || du_contains(w->cursor_box, u->mouse_x, u->mouse_y) ? w->pointer_cursor : DESK_CURSOR_ARROW;
    if (w->kind == DU_EDIT) return DESK_CURSOR_TEXT;
    if (w->kind == DU_COMBO && w->editable && u->mouse_x < w->box.x + w->box.w - 19) return DESK_CURSOR_TEXT;
    if (w->kind == DU_TEXTAREA && w->data) {
        const struct du_textarea *ta = w->data;
        if (ta->drag == DU_TA_DRAG_SELECT) return DESK_CURSOR_TEXT;
        if (ta->drag != DU_TA_DRAG_NONE) return DESK_CURSOR_ARROW;
        struct du_rect text = w->box; text.x += 2; text.y += 2; text.w -= DU_TA_BAR + 4;
        text.h -= 4 + (ta->wrap ? 0 : DU_TA_BAR);
        if (ta->line_numbers) { text.x += 42; text.w -= 42; }
        return du_contains(text, u->mouse_x, u->mouse_y) ? DESK_CURSOR_TEXT : DESK_CURSOR_ARROW;
    }
    if (w->kind == DU_SPLITTER) return w->vertical ? DESK_CURSOR_RESIZE_EW : DESK_CURSOR_RESIZE_NS;
    return u->cursor;
}
unsigned du_cursor_at(const struct du_ui *u)
{
    if (!du_contains((struct du_rect){0,0,u->canvas.width,u->canvas.height},u->mouse_x,u->mouse_y) && u->capture < 0) return DESK_CURSOR_ARROW;
    if (u->dialog.kind) {
        const struct du_dialog *d = &u->dialog;
        if (d->dragging) return DESK_CURSOR_MOVE;
        for (unsigned n = d->count; n > 0; --n)
            if (!d->items[n-1].hidden && du_contains(d->items[n-1].box,u->mouse_x,u->mouse_y)) return widget_cursor(&d->items[n-1],u);
        return du_contains(d->box,u->mouse_x,u->mouse_y) ? DESK_CURSOR_ARROW : DESK_CURSOR_FORBIDDEN;
    }
    if (u->menu >= 0 || u->popup) return DESK_CURSOR_ARROW;
    int i = u->capture >= 0 ? u->capture : du_hit(u,u->mouse_x,u->mouse_y);
    return i >= 0 && (unsigned)i < u->count ? widget_cursor(&u->widgets[i],u) : u->cursor;
}

void du_focus(struct du_ui *u, int id)
{
    for (unsigned i = 0; i < u->count; i++) if (u->widgets[i].id == id) {
        if (u->focus != (int)i) { if (u->focus>=0) du_invalidate(u,u->widgets[u->focus].box); u->focus=(int)i; du_invalidate(u,u->widgets[i].box); }
        return;
    }
}

int du_focused(const struct du_ui *u)
{ return u->focus >= 0 && (unsigned)u->focus < u->count ? u->widgets[u->focus].id : 0; }

void du_release(struct du_ui *u)
{
    memset(u->keys, 0, sizeof(u->keys));
    u->modifiers = u->buttons = 0; u->menu_armed = 0;
    if (u->capture >= 0 && (unsigned)u->capture < u->count) {
        struct du_widget *w = &u->widgets[u->capture];
        if (w->kind == DU_CANVAS) du_post(u, DU_EV_POINTER, w->id, DU_POINTER_CANCEL);
        if (w->band) { w->band = 0; du_emit(u, (struct du_event){.type = DU_EV_MARQUEE, .id = w->id, .index = 2}); }
        w->pressed = 0; w->drag = 0; w->file_drag_pending = 0;
    }
    du_timer_stop(u, DU_TIMER_BAND);
    for (unsigned i = 0; i < u->count; i++) {
        struct du_widget *w = &u->widgets[i];
        w->selection.ns = 0;
        if (w->kind == DU_TEXTAREA && w->data) { du_ta_mouse_up(w->data); ((struct du_textarea *)w->data)->selection.ns = 0; }
    }
    for (unsigned i = 0; i < u->dialog.count; i++) {
        struct du_widget *w = &u->dialog.items[i]; w->pressed = 0; w->drag = 0; w->selection.ns = 0;
        if (w->kind == DU_TEXTAREA && w->data) du_ta_mouse_up(w->data);
    }
    u->dialog.dragging = 0;
    /* G16: an open list, a tooltip and a held repeat do not survive losing the pointer. */
    if (u->combo_open) du_combo_drop(u,u->combo_open,0);
    du_tip_hide(u);
    du_timer_stop(u,DU_TIMER_REPEAT);
    if (u->repeat.w) { u->repeat.w->pressed=0; u->repeat.w->drag=0; u->repeat.w=NULL; }
    if (u->capture>=0 && (unsigned)u->capture<u->count) du_invalidate(u,u->widgets[u->capture].box);
    u->capture=-1;
    if (u->dialog.kind) du_invalidate(u,u->dialog.box);
    if (u->focus>=0 && (unsigned)u->focus<u->count) du_invalidate(u,u->widgets[u->focus].box);
}

void du_window_focus(struct du_ui *u, int focused)
{
    if (!focused) { du_release(u); du_menu_close(u); }
    if (u->focused_window != focused) {
        u->focused_window = focused;
        du_post(u, DU_EV_FOCUS, 0, focused);
    }
    if (u->focus>=0 && (unsigned)u->focus<u->count) du_invalidate(u,u->widgets[u->focus].box);
    if (u->dialog.kind) du_invalidate(u,u->dialog.box);
}

unsigned du_key_char(const struct du_ui *u, unsigned code)
{
    static const char plain[] = "\0\0" "1234567890-=\0\0" "qwertyuiop[]\0\0" "asdfghjkl;'`\0\\" "zxcvbnm,./";
    static const char shift[] = "\0\0" "!@#$%^&*()_+\0\0" "QWERTYUIOP{}\0\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?";
    if (u->modifiers & (DU_CTRL | DU_ALT)) return 0;
    if (code == DU_KEY_SPACE) return ' ';
    if (code >= sizeof(plain) - 1 || !plain[code]) return 0;
    unsigned ch = (unsigned char)((u->modifiers & DU_SHIFT) ? shift[code] : plain[code]);
    if (u->caps && ch >= 'a' && ch <= 'z') ch -= 32;
    else if (u->caps && ch >= 'A' && ch <= 'Z') ch += 32;
    return ch;
}

/* ---- timers ---- */
int du_timer_start(struct du_ui *u, int id, uint64_t interval, int repeat)
{
    if (!interval || UINT64_MAX - u->now < interval) return -1;
    int slot = -1;
    for (int i = 0; i < DU_TIMERS; i++) {
        if (u->timers[i].active && u->timers[i].id == id) { slot = i; break; }
        if (!u->timers[i].active && slot < 0) slot = i;
    }
    if (slot < 0) return -1;
    u->timers[slot] = (struct du_timer){u->now + interval, interval, id, 1, repeat};
    return 0;
}

void du_timer_stop(struct du_ui *u, int id)
{ for (int i = 0; i < DU_TIMERS; i++) if (u->timers[i].id == id) u->timers[i].active = 0; }

void du_tick(struct du_ui *u, uint64_t now)
{
    if (now > u->now) u->now = now;
    if (u->service) u->service(u,u->user);
    du_dialog_tick(u);
    du_tip_tick(u);
    for (int i = 0; i < DU_TIMERS; i++) {
        struct du_timer *t = &u->timers[i];
        if (!t->active || u->now < t->deadline) continue;
        int id = t->id;
        if (t->repeat) t->deadline = UINT64_MAX - u->now < t->interval ? UINT64_MAX : u->now + t->interval;
        else t->active = 0;
        if (id == DU_TIMER_REPEAT) { du_repeat_tick(u); continue; }
        if (id == DU_TIMER_BAND) {
            if (u->capture >= 0 && (unsigned)u->capture < u->count && u->widgets[u->capture].kind == DU_LISTVIEW)
                du_list_band_tick(u, &u->widgets[u->capture]);
            else t->active = 0;
            continue;
        }
        du_emit(u, (struct du_event){.type = DU_EV_TICK, .id = id});
    }
}

uint64_t du_next_deadline(const struct du_ui *u)
{
    uint64_t next = du_dialog_deadline(u), tip = du_tip_deadline(u);
    if (tip < next) next = tip;
    for (int i = 0; i < DU_TIMERS; i++) if (u->timers[i].active && u->timers[i].deadline < next) next = u->timers[i].deadline;
    return next;
}

/* ---- edit ---- */
static size_t sel_lo(const struct du_widget *w) { return w->cursor < w->anchor ? w->cursor : w->anchor; }
static size_t sel_hi(const struct du_widget *w) { return w->cursor > w->anchor ? w->cursor : w->anchor; }

void du_edit_insert(struct du_widget *w, const char *s)
{
    size_t a = sel_lo(w), b = sel_hi(w), len = strlen(w->text), n = 0;
    while (s[n] && s[n] != '\n' && s[n] != '\r') n++;
    if (len - (b - a) + n >= sizeof(w->text)) return;
    memmove(w->text + a + n, w->text + b, len - b + 1);
    memcpy(w->text + a, s, n);
    w->cursor = w->anchor = a + n;
}

static int text_x(const struct du_widget *w) { return w->box.x + 5 + (w->icon ? 20 : 0); }
static size_t char_at(const char *s, size_t chars)
{ size_t at = 0; for (size_t n = 0; n < chars && s[at]; n++) at = du_utf8_next(s, at); return at; }

void du_edit_press(struct du_widget *w, int x, int extend)
{
    /* The character boundary nearest to x, from the scrolled first character. */
    int font = desk_theme->font_ui, pen = text_x(w);
    size_t at = char_at(w->text, w->offset);
    while (w->text[at]) {
        size_t used; unsigned ch = desk_utf8_decode(w->text + at, &used);
        int adv = desk_text_advance(font, ch);
        if (x < pen + adv / 2) break;
        pen += adv; at += used;
    }
    if (x < text_x(w) && w->offset) at = char_at(w->text, w->offset - 1);
    w->cursor = at;
    if (!extend) w->anchor = at;
}

int du_edit_key(struct du_ui *u, struct du_widget *w, unsigned code)
{
    if (w->disabled) return 0;
    w->selection.ns = 0;
    if (du_updown_buddy_key(u, w, code)) return 1;
    size_t len = strlen(w->text);
    int shift = (u->modifiers & DU_SHIFT) != 0, ctrl = (u->modifiers & DU_CTRL) != 0;
    unsigned ch = du_key_char(u, code);
    /* EM_UNDO: Ctrl+Z and Alt+Backspace; a second undo redoes. */
    if ((ctrl && !(u->modifiers & DU_ALT) && code == 0x2c) || ((u->modifiers & DU_ALT) && code == DU_KEY_BACKSPACE))
    { du_edit_undo(u, w); return 1; }
    /* The state before an edit is kept for undo; typing and deleting runs count as one step. */
    char before[DU_TEXT_MAX]; size_t before_cursor = w->cursor, before_anchor = w->anchor; int kind = 0;
    if (ch) kind = 1;
    else if (code == DU_KEY_BACKSPACE || code == DU_KEY_DELETE) kind = 3;
    else if (ctrl && (code == 0x2d || code == 0x2f)) kind = 2;
    if (kind) snprintf(before, sizeof(before), "%s", w->text);
    if (ch) { char s[2] = {(char)ch, 0}; du_edit_insert(w, s); }
    else if (ctrl && !(u->modifiers & DU_ALT) && code == 0x1e) { w->anchor = 0; w->cursor = len; }
    else if (ctrl && (code == 0x2e || code == 0x2d)) {
        size_t a = sel_lo(w), b = sel_hi(w);
        if (b > a) {
            char copy[DU_TEXT_MAX]; memcpy(copy, w->text + a, b - a); copy[b - a] = 0;
            if (!du_clipboard_set_n(copy, b - a) && code == 0x2d) du_edit_insert(w, "");
        }
    } else if (ctrl && code == 0x2f) du_edit_insert(w, du_clipboard_get());
    else if (code == DU_KEY_LEFT || code == DU_KEY_RIGHT) {
        if (!shift && w->cursor != w->anchor) w->cursor = code == DU_KEY_LEFT ? sel_lo(w) : sel_hi(w);
        else w->cursor = code == DU_KEY_LEFT ? du_utf8_prev(w->text, w->cursor) : du_utf8_next(w->text, w->cursor);
        if (!shift) w->anchor = w->cursor;
    } else if (code == DU_KEY_HOME || code == DU_KEY_END || code == DU_KEY_UP || code == DU_KEY_DOWN) {
        w->cursor = code == DU_KEY_HOME || code == DU_KEY_UP ? 0 : len;
        if (!shift) w->anchor = w->cursor;
    } else if (code == DU_KEY_BACKSPACE || code == DU_KEY_DELETE) {
        if (w->cursor == w->anchor)
            w->anchor = code == DU_KEY_BACKSPACE ? du_utf8_prev(w->text, w->cursor) : du_utf8_next(w->text, w->cursor);
        du_edit_insert(w, "");
    } else if (code == DU_KEY_ENTER) { du_post(u, DU_EV_COMMIT, w->id, 0); return 1; }
    else if (code == DU_KEY_ESC) { du_post(u, DU_EV_CANCEL, w->id, 0); return 1; }
    else return 0;
    if (kind && strcmp(before, w->text)) du_undo_note(u, w, before, before_cursor, before_anchor, kind);
    if (ch || code == DU_KEY_BACKSPACE || code == DU_KEY_DELETE || (ctrl && (code == 0x2d || code == 0x2f)))
        du_post(u, DU_EV_CHANGE, w->id, 0);
    du_invalidate(u,w->box);
    return 1;
}

void du_draw_edit(struct du_canvas *c, struct du_widget *w, int focused)
{
    struct du_rect b = w->box;
    int font = desk_theme->font_ui;
    if (w->def) { du_fill(c, b, DU_WINDOW); du_outline(c, b, DU_DARK); }
    else du_well(c, b, w->disabled ? DU_FACE : DU_WINDOW);
    if (focused && !w->def && desk_theme->focus_style == DESK_FOCUS_RING) du_focus_ring(c, b);
    struct du_rect old = c->clip;
    c->clip = du_intersection(old, (struct du_rect){b.x + 2, b.y + 2, b.w - 4, b.h - 4});
    if (w->icon) du_icon_blit(c, b.x + 4, b.y + (b.h - 16) / 2, w->icon, 16, DU_ICON_NORMAL);
    int x = text_x(w), y = b.y + (b.h - DU_GLYPH_H) / 2, room = b.x + b.w - 4 - x;
    if (room < 4) room = 4;
    size_t cursor_chars = 0, chars = du_cells(w->text);
    for (size_t i = 0; i < w->cursor && w->text[i]; i = du_utf8_next(w->text, i)) cursor_chars++;
    if (cursor_chars < w->offset) w->offset = cursor_chars;
    while (w->offset < cursor_chars &&
           desk_text_measure_n(font, w->text + char_at(w->text, w->offset), cursor_chars - w->offset) > room - 1) w->offset++;
    if (w->offset > chars) w->offset = chars;
    size_t start = char_at(w->text, w->offset);
    size_t lo = 0, hi = 0, cells = 0;
    for (size_t i = 0; w->text[i]; i = du_utf8_next(w->text, i), cells++) {
        if (i == sel_lo(w)) lo = cells;
        if (i == sel_hi(w)) hi = cells;
    }
    if (sel_lo(w) >= strlen(w->text)) lo = cells;
    if (sel_hi(w) >= strlen(w->text)) hi = cells;
    uint32_t ink = w->disabled ? DU_GRAY : DU_INK;
    du_text(c, x, y, w->text + start, ink);
    if (focused && hi > lo) {
        size_t a = lo > w->offset ? lo : w->offset;
        if (hi > a) {
            int x0 = x + desk_text_measure_n(font, w->text + start, a - w->offset);
            int x1 = x + desk_text_measure_n(font, w->text + start, hi - w->offset);
            struct du_rect r = {x0, y - 3, x1 - x0, DU_GLYPH_H + 6};
            du_fill(c, r, DU_SELECT);
            struct du_rect keep = c->clip; c->clip = du_intersection(keep, r);
            du_text(c, x, y, w->text + start, DU_SELECT_INK);
            c->clip = keep;
        }
    }
    if (focused && w->cursor == w->anchor) du_vline(c, x + desk_text_measure_n(font, w->text + start, cursor_chars - w->offset) - 1, y - 3, DU_GLYPH_H + 6, DU_INK);
    c->clip = old;
}

/* ---- buttons, status, progress, scrollbars ---- */
void du_draw_button(struct du_canvas *c, const struct du_widget *w, int focused)
{
    struct du_rect b = w->box;
    int down = w->pressed && w->hot, platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM;
    du_fill(c, b, DU_FACE);
    struct du_rect face = b;
    if (platinum && w->def) {
        /* Platinum default button: a dark rounded ring round the face. */
        du_fill(c, (struct du_rect){b.x + 1, b.y, b.w - 2, b.h}, desk_theme->outline);
        du_fill(c, (struct du_rect){b.x, b.y + 1, b.w, b.h - 2}, desk_theme->outline);
        face = (struct du_rect){b.x + 2, b.y + 2, b.w - 4, b.h - 4};
    } else if (!platinum && (w->def || focused) && !down) {
        du_outline(c, b, DU_DARK);
        face = (struct du_rect){b.x + 1, b.y + 1, b.w - 2, b.h - 2};
    }
    if (!platinum) {
        if (down) du_edge(c, face, DU_PRESSED); else du_edge(c, face, DU_BUTTON_EDGE);
    } else du_raised(c, face, down, w->hot && !w->disabled);
    int tw = du_label_width(w->text);
    int iw = w->icon ? w->icon_size + 4 : 0;
    int x = b.x + (b.w - tw - iw) / 2 + down, y = b.y + (b.h - DU_GLYPH_H) / 2 + down;
    /* f2-64: the ring goes under the label, so a focused default button
     * (inset by its default ring) keeps its mnemonic underline visible. */
    if (focused && desk_theme->focus_style == DESK_FOCUS_RING)
        du_focus_ring(c, (struct du_rect){face.x + 2, face.y + 2, face.w - 4, face.h - 4});
    if (w->icon) du_icon_blit(c, x, b.y + (b.h - w->icon_size) / 2 + down, w->icon, w->icon_size, w->disabled ? DU_ICON_DISABLED : DU_ICON_NORMAL);
    if (w->disabled) { du_label(c, x + iw + 1, y + 1, w->text, DU_HILITE, 1); du_label(c, x + iw, y, w->text, DU_GRAY, 1); }
    else du_label(c, x + iw, y, w->text, DU_INK, 1);
    if (focused && desk_theme->focus_style != DESK_FOCUS_RING) du_dotted(c, (struct du_rect){b.x + 4, b.y + 4, b.w - 8, b.h - 8});
}

static void draw_toolbutton(struct du_canvas *c, const struct du_widget *w)
{
    struct du_rect b = w->box;
    int down = w->pressed && w->hot && !w->disabled;
    int split = w->dropdown > 0 ? w->dropdown : 0;
    int platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM;
    struct du_rect main = {b.x, b.y, b.w - split, b.h};
    if (platinum) {
        if (down && w->drag != 2) du_raised(c, main, 1, 0);
        else if ((w->hot || w->drag == 2) && !w->disabled) du_raised(c, main, 0, 1);
    } else if (down && w->drag != 2) du_edge(c, main, DU_SUNKEN_THIN);
    else if ((w->hot || w->drag == 2) && !w->disabled) du_edge(c, main, DU_RAISED_THIN);
    if (split) {
        struct du_rect arrow = {b.x + b.w - split, b.y, split, b.h};
        if (platinum) { if (w->drag == 2 || (w->hot && !w->disabled)) du_raised(c, arrow, w->drag == 2, w->drag != 2); }
        else if (w->drag == 2) du_edge(c, arrow, DU_SUNKEN_THIN);
        else if (w->hot && !w->disabled) du_edge(c, arrow, DU_RAISED_THIN);
        du_triangle(c, arrow.x + split / 2, b.y + b.h / 2, 1, 3, w->disabled ? DU_GRAY : DU_INK);
    }
    int off = down && w->drag != 2;
    int size = w->icon_size ? w->icon_size : DU_ICON_TOOLBAR;
    int has_text = w->text[0] != 0;
    int ix = main.x + (main.w - size) / 2 + off, iy = has_text ? b.y + 3 + off : b.y + (b.h - size) / 2 + off;
    du_icon_blit(c, ix, iy, w->icon, size, w->disabled ? DU_ICON_DISABLED : DU_ICON_NORMAL);
    if (has_text) {
        int tx = main.x + (main.w - du_text_width(w->text)) / 2 + off, ty = b.y + size + 5 + off;
        if (w->disabled) du_text_disabled(c, tx, ty, w->text); else du_text(c, tx, ty, w->text, DU_INK);
    }
}

static void draw_status(struct du_canvas *c, const struct du_widget *w)
{
    struct du_rect b = w->box; int x = b.x;
    int platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM;
    if (platinum) {
        struct desk_paint p = {c->pixels, c->width, c->height, c->clip.x, c->clip.y, c->clip.w, c->clip.h};
        desk_paint_vgradient(&p, b.x, b.y, b.w, b.h, desk_theme->face_top, desk_theme->face_bottom);
        du_hline(c, b.x, b.y, b.w, DU_SHADOW); du_hline(c, b.x, b.y + 1, b.w, DU_HILITE);
    } else du_fill(c, b, DU_FACE);
    const char *s = w->text;
    for (int i = 0; i < 4; i++) {
        int width = w->panels[i] > 0 ? w->panels[i] : b.x + b.w - x;
        if (width <= 0) break;
        struct du_rect panel = {x, b.y + 2, width - 2, b.h - 2};
        if (platinum) { if (x > b.x) { du_vline(c, x - 1, b.y + 4, b.h - 7, DU_SHADOW); du_vline(c, x, b.y + 4, b.h - 7, DU_HILITE); } }
        else du_edge(c, panel, DU_SUNKEN_THIN);
        const char *end = strchr(s, '\t');
        char part[256]; size_t n = end ? (size_t)(end - s) : strlen(s);
        if (n >= sizeof(part)) n = sizeof(part) - 1;
        memcpy(part, s, n); part[n] = 0;
        struct du_rect old = c->clip; c->clip = du_intersection(old, panel);
        du_text_fit(c, panel.x + 5, panel.y + (panel.h - DU_GLYPH_H) / 2, panel.w - 8, part, DU_INK);
        c->clip = old;
        x += width;
        if (!end || w->panels[i] <= 0) break;
        s = end + 1;
    }
}

static void draw_progress(struct du_canvas *c, const struct du_widget *w)
{ du_progress_paint(c,w->box,w->value,w->total,0); }

int du_sb_clamp(int value, int total, int page)
{
    int max = total > page ? total - page : 0;
    return value < 0 ? 0 : value > max ? max : value;
}

int du_wheel_value(int value, int detents, int total, int page)
{
    int max=total>page ? total-page : 0;
    int64_t next=(int64_t)value-(int64_t)detents*3;
    return next<0 ? 0 : next>max ? max : (int)next;
}

static void sb_parts(const struct du_sb *s, int *start, int *track, int *thumb, int *at)
{
    int length = s->vertical ? s->box.h : s->box.w, arrow = s->vertical ? s->box.w : s->box.h;
    if (arrow * 2 > length) arrow = length / 2;
    *start = arrow; *track = length - 2 * arrow;
    int total = s->total > 0 ? s->total : 1, page = s->page > 0 ? s->page : 1;
    *thumb = total > page ? (int)((int64_t)*track * page / total) : *track;
    if (*thumb < 8) *thumb = *track < 8 ? *track : 8;
    int range = total - page, value = du_sb_clamp(s->value, total, page);
    *at = range > 0 ? (int)((int64_t)(*track - *thumb) * value / range) : 0;
}

void du_sb_draw(struct du_canvas *c, const struct du_sb *s, int pressed)
{
    int start, track, thumb, at;
    sb_parts(s, &start, &track, &thumb, &at);
    struct du_rect b = s->box, dec, inc, tr, th;
    if (s->vertical) {
        dec = (struct du_rect){b.x, b.y, b.w, start}; inc = (struct du_rect){b.x, b.y + b.h - start, b.w, start};
        tr = (struct du_rect){b.x, b.y + start, b.w, track}; th = (struct du_rect){b.x, b.y + start + at, b.w, thumb};
    } else {
        dec = (struct du_rect){b.x, b.y, start, b.h}; inc = (struct du_rect){b.x + b.w - start, b.y, start, b.h};
        tr = (struct du_rect){b.x + start, b.y, track, b.h}; th = (struct du_rect){b.x + start + at, b.y, thumb, b.h};
    }
    int platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM;
    if (platinum) {
        /* Platinum groove: darker at the leading edge, lighter towards the far one. */
        struct desk_paint p = {c->pixels, c->width, c->height, c->clip.x, c->clip.y, c->clip.w, c->clip.h};
        uint32_t g0 = desk_mix(DU_SHADOW, DU_FACE, 150), g1 = desk_mix(DU_SHADOW, DU_FACE, 60);
        if (s->vertical) desk_paint_hgradient(&p, tr.x, tr.y, tr.w, tr.h, g0, g1);
        else desk_paint_vgradient(&p, tr.x, tr.y, tr.w, tr.h, g0, g1);
    } else du_checker(c, tr, DU_HILITE, DU_FACE);
    uint32_t press = platinum ? desk_mix(DU_DARK, DU_SHADOW, 120) : DU_DARK;
    if (pressed == 2) du_fill(c, (struct du_rect){tr.x, tr.y, s->vertical ? tr.w : th.x - tr.x, s->vertical ? th.y - tr.y : tr.h}, press);
    if (pressed == 4) du_fill(c, s->vertical ? (struct du_rect){tr.x, th.y + th.h, tr.w, tr.y + tr.h - th.y - th.h}
                                             : (struct du_rect){th.x + th.w, tr.y, tr.x + tr.w - th.x - th.w, tr.h}, press);
    int enabled = s->total > s->page;
    struct du_rect arrows[2] = {dec, inc};
    for (int i = 0; i < 2; i++) {
        struct du_rect a = arrows[i];
        int off = pressed == (i ? 5 : 1);
        if (platinum) du_raised(c, a, off, 0);
        else { du_fill(c, a, DU_FACE); du_edge(c, a, off ? DU_PRESSED : DU_BUTTON_EDGE); }
        int dir = s->vertical ? (i ? 1 : 0) : (i ? 3 : 2);
        uint32_t ink = enabled ? DU_INK : DU_GRAY;
        if (!enabled && !platinum) du_triangle(c, a.x + a.w / 2 + 1, a.y + a.h / 2 + 1, dir, 4, DU_HILITE);
        du_triangle(c, a.x + a.w / 2 + off, a.y + a.h / 2 + off, dir, 4, ink);
    }
    if (enabled && track > 0) {
        if (platinum) {
            du_raised(c, th, 0, 0);
            /* Grip: three short ridges across the middle of the thumb. */
            int cx = th.x + th.w / 2, cy = th.y + th.h / 2;
            for (int k = -1; k <= 1 && (s->vertical ? th.h : th.w) > 14; k++) {
                if (s->vertical) { du_hline(c, cx - 3, cy + 2 * k - 1, 6, DU_HILITE); du_hline(c, cx - 3, cy + 2 * k, 6, DU_SHADOW); }
                else { du_vline(c, cx + 2 * k - 1, cy - 3, 6, DU_HILITE); du_vline(c, cx + 2 * k, cy - 3, 6, DU_SHADOW); }
            }
        } else { du_fill(c, th, DU_FACE); du_edge(c, th, DU_BUTTON_EDGE); }
    }
}

int du_sb_part(const struct du_sb *s, int x, int y)
{
    if (!du_contains(s->box, x, y)) return 0;
    int start, track, thumb, at;
    sb_parts(s, &start, &track, &thumb, &at);
    int p = s->vertical ? y - s->box.y : x - s->box.x;
    if (p < start) return 1;
    if (p >= start + track) return 5;
    if (s->total <= s->page) return 0;
    if (p < start + at) return 2;
    if (p < start + at + thumb) return 3;
    return 4;
}

int du_sb_apply(const struct du_sb *s, int part)
{
    int step = part == 1 ? -1 : part == 5 ? 1 : part == 2 ? -(s->page > 1 ? s->page - 1 : 1) : part == 4 ? (s->page > 1 ? s->page - 1 : 1) : 0;
    return du_sb_clamp(s->value + step, s->total, s->page);
}

int du_sb_grab(const struct du_sb *s, int x, int y)
{
    int start, track, thumb, at;
    sb_parts(s, &start, &track, &thumb, &at);
    return (s->vertical ? y - s->box.y : x - s->box.x) - start - at;
}

int du_sb_drag(const struct du_sb *s, int x, int y, int grab)
{
    int start, track, thumb, at;
    sb_parts(s, &start, &track, &thumb, &at);
    int p = (s->vertical ? y - s->box.y : x - s->box.x) - start - grab, room = track - thumb;
    if (room <= 0) return 0;
    if (p < 0) p = 0;
    if (p > room) p = room;
    return du_sb_clamp((int)((int64_t)p * (s->total - s->page) / room), s->total, s->page);
}

/* ---- f2-33: tab control, history graph, group frame ---- */
static int tab_width(const struct du_widget *w, unsigned i)
{ return du_text_width_font(desk_theme->font_bold, w->items[i].text[0] ? w->items[i].text[0] : "") + 18; }

int du_tabs_hit(const struct du_widget *w, int x, int y)
{
    if (w->kind != DU_TABS || y < w->box.y || y >= w->box.y + DU_TAB_H || x < w->box.x) return -1;
    int at = w->box.x + 2;
    for (unsigned i = 0; i < w->count; i++) {
        int width = tab_width(w, i);
        if (x >= at && x < at + width) return (int)i;
        at += width;
    }
    return -1;
}

static void draw_tabs(struct du_canvas *c, const struct du_widget *w, int focused)
{
    struct du_rect b = w->box;
    int base = b.y + DU_TAB_H - 1, platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM;
    struct du_rect page = {b.x, base, b.w, b.y + b.h - base};
    du_fill(c, page, DU_FACE);
    if (platinum) {
        du_outline(c, page, desk_mix(desk_theme->outline, DU_FACE, 150));
        du_hline(c, page.x + 1, page.y + 1, page.w - 2, DU_HILITE); du_vline(c, page.x + 1, page.y + 1, page.h - 2, DU_HILITE);
    } else {
        du_hline(c, page.x, page.y, page.w, DU_HILITE); du_vline(c, page.x, page.y, page.h, DU_HILITE);
        du_vline(c, page.x + page.w - 1, page.y, page.h, DU_DARK); du_vline(c, page.x + page.w - 2, page.y + 1, page.h - 2, DU_SHADOW);
        du_hline(c, page.x, page.y + page.h - 1, page.w, DU_DARK); du_hline(c, page.x + 1, page.y + page.h - 2, page.w - 2, DU_SHADOW);
    }
    int at = b.x + 2;
    for (unsigned i = 0; i < w->count; i++) {
        int width = tab_width(w, i), sel = i == w->active;
        struct du_rect r = {at - (sel ? 2 : 0), b.y + (sel ? 0 : 2), width + (sel ? 4 : 0), DU_TAB_H - (sel ? 0 : 2)};
        if (platinum) {
            struct desk_paint p = {c->pixels, c->width, c->height, c->clip.x, c->clip.y, c->clip.w, c->clip.h};
            desk_paint_vgradient(&p, r.x + 1, r.y + 1, r.w - 2, r.h - 1, sel ? desk_theme->face_top : desk_mix(DU_SHADOW, desk_theme->face_top, 50),
                                 sel ? DU_FACE : desk_mix(DU_SHADOW, desk_theme->face_bottom, 60));
            uint32_t edge = desk_mix(desk_theme->outline, DU_FACE, 150);
            du_hline(c, r.x + 2, r.y, r.w - 4, edge); du_vline(c, r.x, r.y + 2, r.h - 2, edge); du_vline(c, r.x + r.w - 1, r.y + 2, r.h - 2, edge);
            du_fill(c, (struct du_rect){r.x + 1, r.y + 1, 1, 1}, edge); du_fill(c, (struct du_rect){r.x + r.w - 2, r.y + 1, 1, 1}, edge);
            du_hline(c, r.x + 2, r.y + 1, r.w - 4, DU_HILITE); du_vline(c, r.x + 1, r.y + 2, r.h - 2, DU_HILITE);
            if (sel) { du_hline(c, r.x + 1, base, r.w - 2, DU_FACE); du_hline(c, r.x + 3, r.y + 1, r.w - 6, desk_theme->accent); }
        } else {
            du_fill(c, r, DU_FACE);
            du_hline(c, r.x + 2, r.y, r.w - 4, DU_HILITE); du_vline(c, r.x, r.y + 2, r.h - 2, DU_HILITE);
            du_fill(c, (struct du_rect){r.x + 1, r.y + 1, 1, 1}, DU_HILITE);
            du_vline(c, r.x + r.w - 1, r.y + 2, r.h - 2, DU_DARK); du_vline(c, r.x + r.w - 2, r.y + 1, r.h - 1, DU_SHADOW);
            if (sel) du_hline(c, r.x + 1, base, r.w - 3, DU_FACE);
        }
        const char *t = w->items[i].text[0] ? w->items[i].text[0] : "";
        int font = sel ? desk_theme->font_bold : desk_theme->font_ui;
        int tw = du_text_width_font(font, t), tx = r.x + (r.w - tw) / 2, ty = r.y + (DU_TAB_H - 2 - DU_GLYPH_H) / 2 + (sel ? 1 : 0);
        du_text_font(c, font, tx, ty, t, DU_INK);
        if (sel && focused) du_dotted(c, (struct du_rect){tx - 3, ty - 3, tw + 6, DU_GLYPH_H + 7});
        at += width;
    }
}

static int graph_y(const struct du_widget *w, unsigned v)
{ if (v > 100) v = 100; return w->box.y + w->box.h - 2 - (int)((int64_t)(w->box.h - 4) * v / 100); }

static void draw_graph(struct du_canvas *c, const struct du_widget *w)
{
    struct du_rect b = w->box;
    du_fill(c, b, 0x000000u);
    for (int y = b.y + b.h - 1; y >= b.y; y -= 12) du_hline(c, b.x, y, b.w, 0x00573fu);
    for (int x = b.x + b.w - 1; x >= b.x; x -= 12) du_vline(c, x, b.y, b.h, 0x00573fu);
    unsigned capacity = w->total > 1 ? (unsigned)w->total : 2, n = w->sample_count < capacity ? w->sample_count : capacity;
    if (!w->samples || !n) return;
    /* Newest sample at the right edge; integer Bresenham segments. */
    for (unsigned i = 0; i < n; i++) {
        unsigned slot = capacity - n + i;
        int x1 = b.x + (int)((int64_t)(b.w - 1) * slot / (capacity - 1)), y1 = graph_y(w, w->samples[w->sample_count - n + i]);
        if (!i) { du_fill(c, (struct du_rect){x1, y1, 1, 1}, 0x00ff00u); continue; }
        int x0 = b.x + (int)((int64_t)(b.w - 1) * (slot - 1) / (capacity - 1)), y0 = graph_y(w, w->samples[w->sample_count - n + i - 1]);
        int dx = x1 - x0, dy = y1 > y0 ? y1 - y0 : y0 - y1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
        for (;;) {
            du_fill(c, (struct du_rect){x0, y0, 1, 1}, 0x00ff00u);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0++; }
            if (e2 < dx) { err += dx; y0 += sy; }
        }
    }
}

static void draw_group(struct du_canvas *c, const struct du_widget *w)
{
    struct du_rect b = w->box;
    du_edge(c, (struct du_rect){b.x, b.y + DU_GLYPH_H / 2, b.w, b.h - DU_GLYPH_H / 2}, DU_ETCHED);
    if (w->text[0]) {
        int tw = du_label_width(w->text);
        du_fill(c, (struct du_rect){b.x + 6, b.y - 2, tw + 8, DU_GLYPH_H + 4}, DU_FACE);
        du_label(c, b.x + 10, b.y, w->text, w->disabled ? DU_GRAY : DU_INK, 1);
    }
}

/* ---- ui-look: check boxes, radio buttons, tool-bar bands, headers ---- */
static const char *const radio_mask[12] = {
    "....####....", "..##oooo##..", ".#oooooooo#.", ".#oooooooo#.", "#oooooooooo#", "#oooooooooo#",
    "#oooooooooo#", "#oooooooooo#", ".#oooooooo#.", ".#oooooooo#.", "..##oooo##..", "....####...."};

static void draw_check(struct du_canvas *c, struct du_widget *w, int focused)
{
    struct du_rect b = w->box;
    int platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM, size = platinum ? 12 : 13;
    int bx = b.x, by = b.y + (b.h - size) / 2, down = w->pressed && w->hot;
    uint32_t ink = w->disabled ? DU_GRAY : DU_INK;
    if (w->kind == DU_CHECKBOX) {
        struct du_rect box = {bx, by, size, size};
        if (platinum) {
            struct desk_paint p = {c->pixels, c->width, c->height, c->clip.x, c->clip.y, c->clip.w, c->clip.h};
            du_outline(c, box, w->disabled ? DU_SHADOW : desk_theme->outline);
            desk_paint_vgradient(&p, bx + 1, by + 1, size - 2, size - 2, down ? DU_SHADOW : DU_FACE, down ? DU_FACE : DU_HILITE);
            if (!down) { du_hline(c, bx + 1, by + 1, size - 2, desk_mix(DU_SHADOW, DU_FACE, 120)); du_vline(c, bx + 1, by + 1, size - 2, desk_mix(DU_SHADOW, DU_FACE, 120)); }
        } else { du_fill(c, box, down || w->disabled ? DU_FACE : DU_WINDOW); du_edge(c, box, DU_SUNKEN); }
        if (w->value==2) {
            du_fill(c,(struct du_rect){bx+3,by+5,size-6,3},ink);
        } else if (w->value) {
            /* Tick: two strokes, two pixels thick. */
            static const int8_t tick[][2] = {{2,5},{3,6},{4,7},{5,6},{6,5},{7,4},{8,3},{9,2}};
            uint32_t mark = platinum && !w->disabled ? desk_theme->accent : ink;
            for (unsigned k = 0; k < sizeof(tick) / sizeof(tick[0]); k++)
                du_fill(c, (struct du_rect){bx + tick[k][0] + (platinum ? 0 : 1), by + tick[k][1] - 1 + (platinum ? 0 : 1), 1, 3}, mark);
        }
        if (focused && desk_theme->focus_style == DESK_FOCUS_RING) du_focus_ring(c, (struct du_rect){bx - 2, by - 2, size + 4, size + 4});
    } else {
        for (int y = 0; y < 12; y++) for (int x = 0; x < 12; x++) {
            char k = radio_mask[y][x];
            if (k == '#') du_fill(c, (struct du_rect){bx + x, by + y, 1, 1},
                platinum ? (w->disabled ? DU_SHADOW : desk_theme->outline) : (y + x < 12 ? DU_SHADOW : DU_HILITE));
            else if (k == 'o') du_fill(c, (struct du_rect){bx + x, by + y, 1, 1},
                down || w->disabled ? DU_FACE : platinum ? desk_mix(DU_FACE, DU_HILITE, (unsigned)(y * 20)) : DU_WINDOW);
        }
        if (w->value) {
            uint32_t mark = platinum && !w->disabled ? desk_theme->accent : ink;
            du_fill(c, (struct du_rect){bx + 4, by + 3, 4, 6}, mark); du_fill(c, (struct du_rect){bx + 3, by + 4, 6, 4}, mark);
        }
        if (focused && desk_theme->focus_style == DESK_FOCUS_RING) du_focus_ring(c, (struct du_rect){bx - 2, by - 2, 16, 16});
    }
    int tx = bx + size + 6, ty = b.y + (b.h - DU_GLYPH_H) / 2;
    if (w->disabled) du_text_disabled(c, tx, ty, w->text); else du_label(c, tx, ty, w->text, DU_INK, 1);
    if (focused && desk_theme->focus_style == DESK_FOCUS_DOTTED)
        du_dotted(c, (struct du_rect){tx - 2, ty - 3, du_label_width(w->text) + 4, DU_GLYPH_H + 7});
}

static void draw_band(struct du_canvas *c, const struct du_widget *w)
{
    struct du_rect b = w->box;
    if (desk_theme->control_style == DESK_CONTROLS_PLATINUM) {
        struct desk_paint p = {c->pixels, c->width, c->height, c->clip.x, c->clip.y, c->clip.w, c->clip.h};
        desk_paint_vgradient(&p, b.x, b.y, b.w, b.h, desk_theme->face_top, desk_theme->face_bottom);
    } else du_fill(c, b, DU_FACE);
    du_hline(c, b.x, b.y, b.w, DU_HILITE);
    du_hline(c, b.x, b.y + b.h - 1, b.w, DU_SHADOW);
}

static void draw_header_box(struct du_canvas *c, const struct du_widget *w)
{
    if (desk_theme->control_style == DESK_CONTROLS_PLATINUM) {
        struct desk_paint p = {c->pixels, c->width, c->height, c->clip.x, c->clip.y, c->clip.w, c->clip.h};
        desk_paint_vgradient(&p, w->box.x, w->box.y, w->box.w, w->box.h, desk_theme->face_top, desk_theme->face_bottom);
        du_hline(c, w->box.x, w->box.y + w->box.h - 1, w->box.w, DU_SHADOW);
        du_hline(c, w->box.x, w->box.y, w->box.w, DU_HILITE);
    } else { du_fill(c, w->box, DU_FACE); du_edge(c, w->box, DU_RAISED_THIN); }
    du_text_font(c, desk_theme->font_bold, w->box.x + 6, w->box.y + (w->box.h - DU_GLYPH_H) / 2, w->text, DU_INK);
}

static void select_tab(struct du_ui *u, struct du_widget *w, int index)
{
    if (index < 0 || (unsigned)index >= w->count) return;
    w->active = (unsigned)index; du_invalidate(u,w->box);
    du_emit(u, (struct du_event){.type = DU_EV_SELECT, .id = w->id, .index = index});
}

static struct du_sb widget_sb(const struct du_widget *w)
{ return (struct du_sb){w->box, w->vertical, w->value, w->total, w->page}; }

void du_draw_widget(struct du_ui *u, struct du_widget *w, int focused)
{
    struct du_canvas *c = &u->canvas;
    switch (w->kind) {
    case DU_CANVAS: break; /* the application's paint hook draws it */
    case DU_LABEL:
        if (w->disabled) du_text_disabled(c, w->box.x, w->box.y + (w->box.h - DU_GLYPH_H) / 2, w->text);
        else du_label(c, w->box.x, w->box.y + (w->box.h - DU_GLYPH_H) / 2, w->text, DU_INK, 1);
        break;
    case DU_HEADER: draw_header_box(c, w); break;
    case DU_SEPARATOR:
        if (w->box.w <= 2) { du_vline(c, w->box.x, w->box.y, w->box.h, DU_SHADOW); du_vline(c, w->box.x + 1, w->box.y, w->box.h, DU_HILITE); }
        else { du_hline(c, w->box.x, w->box.y, w->box.w, DU_SHADOW); du_hline(c, w->box.x, w->box.y + 1, w->box.w, DU_HILITE); }
        break;
    case DU_BUTTON: du_draw_button(c, w, focused); break;
    case DU_TOOLBUTTON: draw_toolbutton(c, w); break;
    case DU_EDIT: du_draw_edit(c, w, focused && u->focused_window); break;
    case DU_LISTVIEW: du_draw_list(c, w, focused, u->focused_window); break;
    case DU_TREE: du_draw_tree(c, w, focused, u->focused_window); break;
    case DU_SCROLLBAR: { struct du_sb s = widget_sb(w); du_sb_draw(c, &s, w->pressed && w->hot ? w->drag : 0); break; }
    case DU_STATUS: draw_status(c, w); break;
    case DU_SPLITTER: du_fill(c, w->box, DU_FACE); break;
    case DU_PROGRESS: draw_progress(c, w); break;
    case DU_TABS: draw_tabs(c, w, focused && u->focused_window); break;
    case DU_GRAPH: draw_graph(c, w); break;
    case DU_GROUP: draw_group(c, w); break;
    case DU_TEXTAREA: du_ta_draw(c, w->box, (struct du_textarea *)w->data, focused && u->focused_window); break;
    case DU_CHECKBOX: case DU_RADIO: draw_check(c, w, focused && u->focused_window); break;
    case DU_TOOLBAR: draw_band(c, w); break;
    case DU_COMBO: du_draw_combo(c, w, focused, u->focused_window); break;
    case DU_TRACKBAR: du_draw_trackbar(c, w, focused && u->focused_window); break;
    case DU_UPDOWN: du_draw_updown(c, w); break;
    }
}

static struct du_rect covering(struct du_rect a, struct du_rect b)
{
    int x = a.x < b.x ? a.x : b.x, y = a.y < b.y ? a.y : b.y;
    int r = a.x+a.w > b.x+b.w ? a.x+a.w : b.x+b.w;
    int d = a.y+a.h > b.y+b.h ? a.y+a.h : b.y+b.h;
    return (struct du_rect){x,y,r-x,d-y};
}

/* Fixed storage, no allocation in the paint path. Keep distant widgets
 * separate; capacity pressure joins the pair with the least added area. */
void du_region_add(struct du_rect *rects, unsigned *count, struct du_rect r)
{
    if (r.w <= 0 || r.h <= 0) return;
    for (unsigned i=0; i<*count; i++) {
        struct du_rect a=rects[i];
        if (r.x>=a.x && r.y>=a.y && r.x+r.w<=a.x+a.w && r.y+r.h<=a.y+a.h) return;
        if (a.x>=r.x && a.y>=r.y && a.x+a.w<=r.x+r.w && a.y+a.h<=r.y+r.h) {
            rects[i]=rects[--*count]; i--; continue;
        }
    }
    if (*count == DU_REGIONS) {
        unsigned ai=0, bi=1; int64_t best=INT64_MAX;
        for (unsigned i=0; i<*count; i++) for (unsigned j=i+1; j<*count; j++) {
            struct du_rect box=covering(rects[i],rects[j]);
            int64_t cost=(int64_t)box.w*box.h-(int64_t)rects[i].w*rects[i].h-(int64_t)rects[j].w*rects[j].h;
            if (cost<best) { best=cost; ai=i; bi=j; }
        }
        rects[ai]=covering(rects[ai],rects[bi]); rects[bi]=rects[--*count];
    }
    rects[(*count)++]=r;
}

void du_invalidate(struct du_ui *u, struct du_rect r)
{
    r=du_intersection(r,(struct du_rect){0,0,u->canvas.width,u->canvas.height});
    if (r.w<=0 || r.h<=0 || u->dirty==1) return;
    if (u->dirty!=3) {
        u->update_count=0;
        if (u->dirty==2 && u->dialog.kind) du_region_add(u->updates,&u->update_count,u->dialog.box);
    }
    du_region_add(u->updates,&u->update_count,r);
    u->invalid=u->update_count==1 ? r : covering(u->invalid,r); u->dirty=3;
}
void du_invalidate_widget(struct du_ui *u, int id)
{
    struct du_widget *w=du_get(u,id);
    if (w && !w->hidden) du_invalidate(u,w->box);
}

void du_scroll(struct du_ui *u, struct du_rect r, int dx, int dy)
{
    r=du_intersection(r,(struct du_rect){0,0,u->canvas.width,u->canvas.height});
    if (r.w<=0 || r.h<=0 || (!dx && !dy)) return;
    if (u->dirty==1 || u->dirty==2 || u->dialog.kind || u->menu>=0 || u->popup ||
        dx<=-r.w || dx>=r.w || dy<=-r.h || dy>=r.h) { du_invalidate(u,r); return; }
    for (unsigned i=0;i<u->update_count && u->dirty==3;i++) {
        struct du_rect q=du_intersection(r,u->updates[i]);
        if (q.w>0 && q.h>0) { du_invalidate(u,r); return; }
    }
    struct du_rect dest=du_intersection(r,(struct du_rect){r.x+dx,r.y+dy,r.w,r.h});
    for (int n=0;n<dest.h;n++) {
        int y=dy>0 ? dest.y+dest.h-1-n : dest.y+n;
        memmove(u->canvas.pixels+(size_t)y*u->canvas.width+dest.x,
                u->canvas.pixels+(size_t)(y-dy)*u->canvas.width+dest.x-dx,(size_t)dest.w*4);
    }
    du_region_add(u->copied,&u->copied_count,dest);
    if (dy) du_invalidate(u,(struct du_rect){r.x,dy>0?r.y:r.y+r.h+dy,r.w,dy>0?dy:-dy});
    if (dx) du_invalidate(u,(struct du_rect){dx>0?r.x:r.x+r.w+dx,dest.y,dx>0?dx:-dx,dest.h});
}

void du_render(struct du_ui *u)
{
    struct du_canvas *c=&u->canvas;
    struct du_rect rects[DU_REGIONS]; unsigned count=0;
    if (u->dirty==3) {
        count=u->update_count; memcpy(rects,u->updates,count*sizeof(*rects));
        /* Compatibility with clients retaining only the old bounded field. */
        if (!count) rects[count++]=u->invalid;
    } else rects[count++]=u->dirty==2 && u->dialog.kind==DU_DLG_PROGRESS ? u->dialog.box :
        (struct du_rect){0,0,c->width,c->height};
    u->dirty=0; u->update_count=0; u->frame_count=0;
    for (unsigned i=0;i<u->copied_count;i++) du_region_add(u->frames,&u->frame_count,u->copied[i]);
    u->copied_count=0;
    for (unsigned n=0;n<count;n++) {
        struct du_rect repaint=rects[n];
        du_region_add(u->frames,&u->frame_count,repaint);
        c->clip=repaint; du_fill(c,repaint,DU_FACE);
        for (unsigned i=0;i<u->count;i++) {
            struct du_widget *w=&u->widgets[i];
            if (w->hidden) continue;
            c->clip=du_intersection(repaint,w->box);
            if (c->clip.w>0 && c->clip.h>0) du_draw_widget(u,w,u->focus==(int)i && !u->dialog.kind);
        }
        c->clip=repaint;
        if (u->paint) u->paint(u,u->user);
        c->clip=repaint; du_draw_menus(u);
        c->clip=repaint; du_draw_dialog(u);
        c->clip=repaint; du_draw_overlays(u);
    }
    u->rendered=(struct du_rect){0};
    for (unsigned i=0;i<u->frame_count;i++) u->rendered=i ? covering(u->rendered,u->frames[i]) : u->frames[i];
    c->clip=(struct du_rect){0,0,c->width,c->height};
}

/* ---- per-widget input ---- */
static void toggle(struct du_ui *u, struct du_widget *w)
{
    if (w->kind == DU_RADIO) {
        struct du_widget *items = u->widgets; unsigned count = u->count;
        for (unsigned i = 0; i < u->dialog.count; i++) if (w == &u->dialog.items[i]) {
            items = u->dialog.items; count = u->dialog.count; break;
        }
        for (unsigned i = 0; i < count; i++)
            if (items[i].kind == DU_RADIO && items[i].total == w->total) { if (items[i].value) du_invalidate(u,items[i].box); items[i].value=0; }
        w->value = 1;
    } else w->value = !w->value;
    du_invalidate(u,w->box);
    du_post(u, DU_EV_CHANGE, w->id, w->value);
}

static struct du_rect ta_caret(struct du_widget *w)
{ du_ta_fit(w->data,w->box); return du_ta_caret(w->data,w->box); }
static void ta_changed(struct du_ui *u, struct du_widget *w, struct du_textarea before, struct du_rect caret)
{
    struct du_textarea *ta=w->data;
    struct du_rect body={w->box.x+DU_TA_PAD+(ta->line_numbers ? 42 : 0),w->box.y+DU_TA_PAD,
        w->box.w-2*DU_TA_PAD-DU_TA_BAR-(ta->line_numbers ? 42 : 0),
        w->box.h-2*DU_TA_PAD-(ta->wrap ? 0 : DU_TA_BAR)};
    if (ta->revision!=before.revision) { du_invalidate(u,w->box); return; }
    int selection=ta->cursor!=ta->anchor || before.cursor!=before.anchor;
    int moved=ta->cursor!=before.cursor || ta->anchor!=before.anchor;
    int scrolled=ta->top!=before.top || ta->left!=before.left;
    if (!moved && !scrolled) return;
    if (ta->top!=before.top || ta->left!=before.left) {
        int dx=((int)before.left-(int)ta->left)*DU_TA_CELL_W,dy=((int)before.top-(int)ta->top)*DU_TA_CELL_H;
        /* Padding and partial cells are fixed background, not retained text.
         * Moving them would copy glyph tails into the top/bottom margin. */
        struct du_rect cells=du_intersection(body,(struct du_rect){body.x+DU_TA_PAD,body.y+DU_TA_PAD,
            (int)ta->cols*DU_TA_CELL_W,(int)ta->rows*DU_TA_CELL_H});
        du_scroll(u,cells,dx,dy);
        du_invalidate(u,(struct du_rect){body.x,body.y,body.w,cells.y-body.y});
        du_invalidate(u,(struct du_rect){body.x,cells.y+cells.h,body.w,body.y+body.h-cells.y-cells.h});
        du_invalidate(u,(struct du_rect){body.x,cells.y,cells.x-body.x,cells.h});
        du_invalidate(u,(struct du_rect){cells.x+cells.w,cells.y,body.x+body.w-cells.x-cells.w,cells.h});
        caret.x+=dx; caret.y+=dy;
        du_invalidate(u,(struct du_rect){w->box.x+w->box.w-DU_TA_BAR-2,w->box.y,DU_TA_BAR+2,w->box.h});
        if (!ta->wrap) du_invalidate(u,(struct du_rect){w->box.x,w->box.y+w->box.h-DU_TA_BAR-2,w->box.w,DU_TA_BAR+2});
        if (ta->line_numbers) du_invalidate(u,(struct du_rect){w->box.x+2,w->box.y+2,42,body.h});
    }
    if (selection) {
        if (moved) {
            unsigned first=du_ta_visual_line_of(ta,ta->cursor,NULL),last=first;
            unsigned rows[]={du_ta_visual_line_of(ta,ta->anchor,NULL),du_ta_visual_line_of(&before,before.cursor,NULL),du_ta_visual_line_of(&before,before.anchor,NULL)};
            for (unsigned i=0;i<3;i++) { if(rows[i]<first)first=rows[i];if(rows[i]>last)last=rows[i]; }
            int y=w->box.y+4+((int)first-(int)ta->top-1)*DU_TA_CELL_H;
            du_invalidate(u,du_intersection(body,(struct du_rect){body.x,y,body.w,((int)last-(int)first+3)*DU_TA_CELL_H}));
        }
    } else {
        du_invalidate(u,du_intersection(caret,body));
        du_invalidate(u,du_intersection(ta_caret(w),body));
    }
}

int du_widget_wheel(struct du_ui *u, struct du_widget *w, int detents)
{
    if (w->disabled || w->hidden || !detents) return 0;
    int changed=0;
    switch (w->kind) {
    case DU_LISTVIEW: case DU_TREE: {
        int v=w->scroll,h=w->hscroll; changed=du_list_wheel(w,detents);
        if (changed) du_list_scrolled(u,w,v,h);
        return changed;
    }
    case DU_TEXTAREA:
        if (w->data) {
            struct du_rect caret=ta_caret(w); struct du_textarea before=*(struct du_textarea *)w->data;
            changed=du_ta_wheel(w->data,w->box,detents);
            if (changed) ta_changed(u,w,before,caret);
        }
        return changed;
    case DU_TRACKBAR: return du_trackbar_wheel(u,w,detents);
    case DU_SCROLLBAR: {
        int next=du_wheel_value(w->value,detents,w->total,w->page);
        changed=next!=w->value; w->value=next;
        if (changed) du_post(u,DU_EV_CHANGE,w->id,w->value);
        break;
    }
    default: break;
    }
    if (changed) du_invalidate(u,w->box);
    return changed;
}

int du_widget_key(struct du_ui *u, struct du_widget *w, unsigned code)
{
    if (w->disabled) return 0;
    switch (w->kind) {
    case DU_EDIT: return du_edit_key(u, w, code);
    case DU_LISTVIEW: case DU_TREE: return du_list_key(u, w, code);
    case DU_TEXTAREA: {
        struct du_textarea *ta = w->data;
        if (!ta) return 0;
        du_ta_fit(ta, w->box);
        struct du_rect caret=ta_caret(w); struct du_textarea before=*ta;
        unsigned ch = du_key_char(u, code);
        if (!(ch ? du_ta_text(ta, ch) : du_ta_key(ta, code, u->modifiers))) return 0;
        ta_changed(u,w,before,caret);
        du_post(u, DU_EV_CHANGE, w->id, 0);
        return 1;
    }
    case DU_COMBO: return du_combo_key(u, w, code);
    case DU_TRACKBAR: return du_trackbar_key(u, w, code);
    case DU_BUTTON:
        if (code == DU_KEY_SPACE || code == DU_KEY_ENTER) { du_post(u, DU_EV_ACTIVATE, w->id, 0); return 1; }
        return 0;
    case DU_CHECKBOX: case DU_RADIO:
        if (code != DU_KEY_SPACE) return 0;
        toggle(u, w); return 1;
    case DU_TABS:
        if (!w->count || (code != DU_KEY_LEFT && code != DU_KEY_RIGHT && code != DU_KEY_HOME && code != DU_KEY_END)) return 0;
        select_tab(u, w, code == DU_KEY_HOME ? 0 : code == DU_KEY_END ? (int)w->count - 1 :
                   (int)(w->active + (code == DU_KEY_LEFT ? w->count - 1 : 1)) % (int)w->count);
        return 1;
    default: return 0;
    }
}

int du_widget_press(struct du_ui *u, struct du_widget *w, int button, int x, int y)
{
    if (w->disabled) return 1;
    switch (w->kind) {
    case DU_CANVAS:
        if (w->drag) return 1;
        w->drag = button;
        du_emit(u, (struct du_event){.type = DU_EV_POINTER, .id = w->id, .index = DU_POINTER_DOWN, .key = (unsigned)button, .x = x, .y = y});
        return 1;
    case DU_BUTTON: case DU_TOOLBUTTON: case DU_CHECKBOX: case DU_RADIO:
        if (button != 1) return 1;
        w->pressed = 1; w->hot = 1;
        w->drag = w->kind == DU_TOOLBUTTON && w->dropdown > 0 && x >= w->box.x + w->box.w - w->dropdown ? 2 : 1;
        if (w->drag == 2) { w->pressed = 0; du_post(u, DU_EV_CHANGE, w->id, 1); }
        return 1;
    case DU_EDIT:
        if (button == 1) {
            size_t anchor = w->anchor;
            du_edit_press(w, x, 1);
            du_select_click(&w->selection, w->text, w->cursor, (u->modifiers & DU_SHIFT) != 0,
                            u->now, x, y, &anchor, &w->cursor);
            w->anchor = anchor; w->drag = 1; du_invalidate(u,w->box);
        } else if (button == 2) du_emit(u, (struct du_event){.type = DU_EV_CONTEXT, .id = w->id, .index = -1, .x = x, .y = y});
        return 1;
    case DU_TEXTAREA:
        if (button == 2) du_emit(u, (struct du_event){.type = DU_EV_CONTEXT, .id = w->id, .index = -1, .x = x, .y = y});
        else if (button==1 && w->data) {
            struct du_rect caret=ta_caret(w); struct du_textarea before=*(struct du_textarea *)w->data;
            if (du_ta_mouse_down(w->data,w->box,x,y,u->modifiers,u->now)) { ta_changed(u,w,before,caret); du_post(u,DU_EV_CHANGE,w->id,0); }
        }
        return 1;
    case DU_LISTVIEW: case DU_TREE: return du_list_press(u, w, button, x, y);
    case DU_COMBO: return du_combo_press(u, w, button, x, y);
    case DU_TRACKBAR: return du_trackbar_press(u, w, button, x, y);
    case DU_UPDOWN: return du_updown_press(u, w, button, x, y);
    case DU_SCROLLBAR: {
        if (button != 1) return 1;
        struct du_sb s = widget_sb(w); int part = du_sb_part(&s, x, y);
        w->pressed = 1; w->hot = 1; w->drag = part;
        if (part == 3) w->drag_offset = du_sb_grab(&s, x, y);
        else if (part) { int v = du_sb_apply(&s, part); if (v != w->value) { w->value = v; du_post(u, DU_EV_CHANGE, w->id, v); } }
        return 1;
    }
    case DU_SPLITTER:
        if (button == 1) { w->drag = 1; w->drag_offset = (w->vertical ? x - w->box.x : y - w->box.y); }
        return 1;
    case DU_TABS:
        if (button == 1) select_tab(u, w, du_tabs_hit(w, x, y));
        return 1;
    default: return 0;
    }
}

void du_widget_motion(struct du_ui *u, struct du_widget *w, int x, int y)
{
    switch (w->kind) {
    case DU_CANVAS:
        du_emit(u, (struct du_event){.type = DU_EV_POINTER, .id = w->id, .index = DU_POINTER_MOVE, .key = (unsigned)w->drag, .x = x, .y = y});
        break;
    case DU_BUTTON: case DU_TOOLBUTTON: case DU_CHECKBOX: case DU_RADIO: {
        int hot = du_contains(w->box, x, y);
        if (hot != w->hot) { w->hot = (uint8_t)hot; du_invalidate(u,w->box); }
        break;
    }
    case DU_TRACKBAR: du_trackbar_motion(u, w, x, y); break;
    case DU_UPDOWN: du_updown_motion(u, w, x, y); break;
    case DU_COMBO: case DU_EDIT: if (w->drag) {
        du_edit_press(w, x, 1);
        du_select_drag(&w->selection, w->text, w->cursor, &w->anchor, &w->cursor);
        du_invalidate(u,w->box);
    } break;
    case DU_TEXTAREA: if (w->data) {
        struct du_rect caret=ta_caret(w); struct du_textarea before=*(struct du_textarea *)w->data;
        if (du_ta_mouse_move(w->data,w->box,x,y)) { ta_changed(u,w,before,caret); du_post(u,DU_EV_CHANGE,w->id,0); }
    } break;
    case DU_LISTVIEW: case DU_TREE: du_list_motion(u, w, x, y); break;
    case DU_SCROLLBAR:
        if (w->drag == 3) {
            struct du_sb s = widget_sb(w); int v = du_sb_drag(&s, x, y, w->drag_offset);
            if (v != w->value) { w->value = v; du_post(u, DU_EV_CHANGE, w->id, v); }
        } else {
            unsigned hot=du_sb_part(&(struct du_sb){w->box,w->vertical,w->value,w->total,w->page},x,y)==w->drag;
            if (hot!=w->hot) { w->hot=(uint8_t)hot; du_invalidate(u,w->box); }
        }
        break;
    case DU_SPLITTER:
        if (w->drag) du_post(u, DU_EV_CHANGE, w->id, w->vertical ? x - w->drag_offset : y - w->drag_offset);
        break;
    default: break;
    }
}

void du_widget_release(struct du_ui *u, struct du_widget *w, int button, int x, int y)
{
    if (w->kind == DU_CANVAS) {
        if (w->drag != button) return;
        w->drag = 0;
        du_emit(u, (struct du_event){.type = DU_EV_POINTER, .id = w->id, .index = DU_POINTER_UP, .key = (unsigned)button, .x = x, .y = y});
        return;
    }
    if (button == 2 && (w->kind == DU_LISTVIEW || w->kind == DU_TREE)) {
        if (w->drag == 52 || w->drag == 53) du_list_release(u, w);
        return;
    }
    if (button != 1) return;
    if (w->kind == DU_TRACKBAR) { du_trackbar_release(u, w); return; }
    if (w->kind == DU_UPDOWN) { du_updown_release(u, w); return; }
    if (w->kind == DU_COMBO) { w->pressed = 0; w->drag = 0; du_invalidate(u, w->box); return; }
    int fire = (w->kind == DU_BUTTON || w->kind == DU_TOOLBUTTON) && w->pressed && w->drag == 1 && du_contains(w->box, x, y) && !w->disabled;
    int flip = (w->kind == DU_CHECKBOX || w->kind == DU_RADIO) && w->pressed && du_contains(w->box, x, y) && !w->disabled;
    if (w->kind == DU_LISTVIEW || w->kind == DU_TREE) du_list_release(u, w);
    if (w->kind == DU_TEXTAREA && w->data) du_ta_mouse_up(w->data);
    w->pressed = 0; w->drag = 0; w->hot = (uint8_t)du_contains(w->box, x, y);
    du_invalidate(u,w->box);
    if (fire) du_post(u, DU_EV_ACTIVATE, w->id, 0);
    if (flip) toggle(u, w);
}

/* ---- dispatch ---- */
static void focus_next(struct du_ui *u, int back)
{
    if (!u->count) return;
    int i = u->focus;
    for (unsigned n = 0; n < u->count; n++) {
        i = back ? (i <= 0 ? (int)u->count - 1 : i - 1) : (i + 1) % (int)u->count;
        if (du_focusable(&u->widgets[i])) { du_focus(u,u->widgets[i].id); return; }
    }
}

static void key_down(struct du_ui *u, unsigned code)
{
    du_tip_hide(u);
    if (u->combo_open) { du_combo_overlay_key(u, code); return; }
    if (u->dialog.kind) { du_dialog_key(u, code); return; }
    if (u->menu >= 0 || u->popup) { du_menu_key(u, code); return; }
    if (code == DU_KEY_F10 && !(u->modifiers & ~DU_SHIFT)) {
        if (u->modifiers & DU_SHIFT) { /* Shift+F10: context menu of the focus */ }
        else { du_menu_open(u, 0, 1); return; }
    }
    if ((u->modifiers & DU_ALT) && !(u->modifiers & DU_CTRL)) {
        unsigned saved = u->modifiers; u->modifiers = 0;
        unsigned ch = du_key_char(u, code); u->modifiers = saved;
        if (ch && du_menu_mnemonic(u, ch)) return;
        /* f2-33: Alt plus a button's underlined (&) letter presses it. */
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        for (unsigned i = 0; ch && i < u->count; i++) {
            const struct du_widget *b = &u->widgets[i];
            const char *amp = (b->kind == DU_BUTTON || b->kind == DU_CHECKBOX || b->kind == DU_RADIO) && !b->hidden && !b->disabled ? strchr(b->text, '&') : NULL;
            unsigned m = amp && amp[1] && amp[1] != '&' ? (unsigned char)amp[1] : 0;
            if (m >= 'A' && m <= 'Z') m += 32;
            if (m == ch) {
                if (b->kind == DU_BUTTON) du_post(u, DU_EV_ACTIVATE, b->id, 0); else toggle(u, &u->widgets[i]);
                return;
            }
        }
    }
    struct du_widget *f = u->focus >= 0 && (unsigned)u->focus < u->count ? &u->widgets[u->focus] : NULL;
    if (f && (f->hidden || f->disabled)) f = NULL;
    if (f && f->kind == DU_EDIT && du_edit_key(u, f, code)) return;
    if (f && f->kind == DU_TEXTAREA && f->data && ((struct du_textarea *)f->data)->readonly &&
        du_widget_key(u, f, code)) return;
    if (du_menu_accelerator(u, code)) return;
    /* f2-33: Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+PgDn/PgUp cycle the first visible tab control. */
    if ((u->modifiers & DU_CTRL) && !(u->modifiers & DU_ALT) && (code == DU_KEY_TAB || code == DU_KEY_PGUP || code == DU_KEY_PGDN))
        for (unsigned i = 0; i < u->count; i++) {
            struct du_widget *t = &u->widgets[i];
            if (t->kind != DU_TABS || t->hidden || t->disabled || !t->count) continue;
            int back = code == DU_KEY_PGUP || (code == DU_KEY_TAB && (u->modifiers & DU_SHIFT));
            select_tab(u, t, (int)(t->active + (back ? t->count - 1 : 1)) % (int)t->count);
            return;
        }
    if (code == DU_KEY_TAB && !(u->modifiers & (DU_CTRL | DU_ALT)) && !(f && (f->kind == DU_TEXTAREA || (f->kind == DU_CANVAS && f->accepts_tab)))) { focus_next(u, (u->modifiers & DU_SHIFT) != 0); return; }
    if (f && (code == DU_KEY_MENU || (code == DU_KEY_F10 && u->modifiers == DU_SHIFT)) && (f->kind == DU_LISTVIEW || f->kind == DU_TREE)) {
        struct du_rect r = f->count && f->active < f->count ? du_list_label(f, f->active) : du_list_body(f);
        du_emit(u, (struct du_event){.type = DU_EV_CONTEXT, .id = f->id, .index = f->count ? (int)f->active : -1,
                                     .x = r.x + 8, .y = r.y + r.h, .keyboard = 1});
        return;
    }
    if (f && du_widget_key(u, f, code)) return;
    du_emit(u, (struct du_event){.type = DU_EV_KEY, .id = f ? f->id : 0, .key = code, .ch = du_key_char(u, code)});
}

void du_input(struct du_ui *u, const struct ciuki_input_event *e)
{
    if (e->monotonic_ns > u->now) u->now = e->monotonic_ns;
    if (e->type==CIUKI_INPUT_CONSUMER) {
        if (e->lost_count==CIUKI_HID_VERSION && e->value==1 &&
            (unsigned)e->code>=CIUKI_CONSUMER_PLAY_PAUSE && (unsigned)e->code<=CIUKI_CONSUMER_PREVIOUS)
            du_emit(u,(struct du_event){.type=DU_EV_CONSUMER,.key=(unsigned)e->code});
        return;
    }
    if (e->type == CIUKI_INPUT_RESYNC) { du_release(u); return; }
    if (e->type == CIUKI_INPUT_KEY) {
        if (e->code < 0 || e->code > 0x200) return;
        unsigned code = (unsigned)e->code;
        u->keys[code] = (uint8_t)(e->value != 0);
        u->modifiers = (u->keys[DU_KEY_LSHIFT] || u->keys[DU_KEY_RSHIFT] ? DU_SHIFT : 0) |
                       (u->keys[DU_KEY_LCTRL] || u->keys[DU_KEY_RCTRL] ? DU_CTRL : 0) |
                       (u->keys[DU_KEY_LALT] || u->keys[DU_KEY_RALT] ? DU_ALT : 0);
        if (code == DU_KEY_LALT || code == DU_KEY_RALT) {
            if (e->value == 1) u->menu_armed = 1;
            else if (!e->value && u->menu_armed && !u->dialog.kind) {
                u->menu_armed = 0;
                if (u->menu >= 0 || u->popup) du_menu_close(u); else if (u->menu_count) du_menu_open(u, 0, 1);
            }
            return;
        }
        if (!e->value) return;
        u->menu_armed = 0;
        if (code == DU_KEY_CAPS && e->value == 1) { u->caps = !u->caps; return; }
        if (code == DU_KEY_LSHIFT || code == DU_KEY_RSHIFT || code == DU_KEY_LCTRL || code == DU_KEY_RCTRL) return;
        key_down(u, code);
        return;
    }
    if (e->type == CIUKI_INPUT_TEXT) return; /* text derives from KEY events plus Shift/Caps */
    if (e->type == CIUKI_INPUT_WHEEL) {
        du_tip_hide(u);
        if (u->combo_open) { du_combo_overlay_wheel(u,e->value); return; }
        if (u->dialog.kind) { du_dialog_wheel(u,e->value); return; }
        if (u->menu>=0 || u->popup || u->capture>=0) return;
        int hit=du_hit(u,u->mouse_x,u->mouse_y);
        if (hit>=0) du_widget_wheel(u,&u->widgets[hit],e->value);
        return;
    }
    if (e->type == CIUKI_INPUT_MOTION) {
        u->mouse_x = e->value; u->mouse_y = e->value2;
        if (u->combo_open) { du_combo_overlay_motion(u); return; }
        if (u->dialog.kind) { du_dialog_motion(u); return; }
        if (u->menu >= 0 || u->popup) { du_tip_hide(u); du_menu_motion(u); return; }
        if (u->capture >= 0 && (unsigned)u->capture < u->count) { du_tip_hide(u); du_widget_motion(u, &u->widgets[u->capture], u->mouse_x, u->mouse_y); return; }
        int hit = du_hit(u, u->mouse_x, u->mouse_y);
        du_tip_motion(u, hit >= 0 ? &u->widgets[hit] : NULL);
        if (hit != u->hover) {
            if (u->hover>=0 && (unsigned)u->hover<u->count) {
                struct du_widget *old=&u->widgets[u->hover];
                if (old->hot) { old->hot=0; du_invalidate(u,old->box); }
                /* Give an application canvas one leave motion to erase its
                 * own hover overlay without repainting the entire widget. */
                if (old->kind==DU_CANVAS && !old->disabled) du_widget_motion(u,old,u->mouse_x,u->mouse_y);
            }
            u->hover = hit;
        }
        if (hit >= 0 && (u->widgets[hit].kind == DU_TOOLBUTTON || u->widgets[hit].kind == DU_BUTTON) &&
            !u->widgets[hit].hot && !u->widgets[hit].disabled) { u->widgets[hit].hot = 1; du_invalidate(u,u->widgets[hit].box); }
        if (hit >= 0 && u->widgets[hit].kind == DU_CANVAS && !u->widgets[hit].disabled)
            du_widget_motion(u, &u->widgets[hit], u->mouse_x, u->mouse_y);
        return;
    }
    if (e->type != CIUKI_INPUT_BUTTON || e->code < 1 || e->code > 3) return;
    int button = e->code;
    unsigned bit = 1u << (button - 1);
    if (e->value) u->buttons |= bit; else u->buttons &= ~bit;
    du_tip_hide(u);
    if (u->combo_open) { du_combo_overlay_button(u, button, e->value != 0); return; }
    if (u->dialog.kind) { if (e->value) du_dialog_press(u, button); else du_dialog_release(u, button); return; }
    if (e->value) {
        /* A second button cannot steal an application's drawing capture. */
        if (u->capture >= 0 && (unsigned)u->capture < u->count &&
            u->widgets[u->capture].kind == DU_CANVAS && u->widgets[u->capture].drag) return;
        if (du_menu_press(u, button)) return;
        int hit = du_hit(u, u->mouse_x, u->mouse_y);
        if (hit < 0) {
            if (button == 2) du_emit(u, (struct du_event){.type = DU_EV_CONTEXT, .id = 0, .index = -1});
            return;
        }
        struct du_widget *w = &u->widgets[hit];
        if (du_focusable(w)) { if (u->focus != hit) { if (u->focus>=0) du_invalidate(u,u->widgets[u->focus].box); du_invalidate(u,w->box); } u->focus = hit; }
        if (button == 1 || (w->kind == DU_CANVAS && !w->drag) ||
            (button == 2 && (w->kind == DU_LISTVIEW || w->kind == DU_TREE) && w->file_drag)) u->capture = hit;
        du_widget_press(u, w, button, u->mouse_x, u->mouse_y);
    } else {
        if (du_menu_release(u, button)) return;
        if (u->capture >= 0 && (unsigned)u->capture < u->count) {
            int captured = u->capture;
            const struct du_widget *cw = &u->widgets[captured];
            int list_right = button == 2 && (cw->kind == DU_LISTVIEW || cw->kind == DU_TREE) && cw->file_drag;
            if ((cw->kind == DU_CANVAS && cw->drag == button) || (cw->kind != DU_CANVAS && button == 1) || list_right) u->capture = -1;
            else if (button != 1 && cw->kind != DU_CANVAS) return;
            du_widget_release(u, &u->widgets[captured], button, u->mouse_x, u->mouse_y);
        }
    }
}
