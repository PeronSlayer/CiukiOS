/* SPDX-License-Identifier: MIT
 * Menu bar, drop-down and context menus with mnemonics and accelerators. */
#include "internal.h"
#include "../desktop/sounds.h"
#include <string.h>

#define ITEM_H DU_MENU_ITEM_H
#define SEP_H DU_MENU_SEP_H

static int label_width(const char *s) {
    struct desk_theme role; const struct desk_theme *saved=desk_theme_role(&role,AP_MENU);
    int width=du_label_width(s); desk_theme=saved; return width;
}

static unsigned fold(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static unsigned mnemonic(const char *s)
{
    for (; s && *s; s++) {
        if (*s == '&' && s[1] == '&') { s++; continue; }
        if (*s == '&' && s[1]) return fold((unsigned char)s[1]);
    }
    return 0;
}

struct du_rect du_menu_title(const struct du_ui *u, unsigned index)
{
    int x = 2;
    for (unsigned i = 0; i < index && i < u->menu_count; i++) x += label_width(u->menus[i].label) + 16;
    int w = index < u->menu_count ? label_width(u->menus[index].label) + 16 : 0;
    return (struct du_rect){x, 1, w, DU_MENUBAR - 2};
}

static int item_h(const struct du_menu_item *it) { return it->flags & DU_ITEM_SEPARATOR ? SEP_H : ITEM_H; }

static int accel_width(const struct du_menu_item *items, unsigned n, int *labels)
{
    int widest = 0; *labels = 0;
    for (unsigned i = 0; i < n; i++) {
        if (items[i].flags & DU_ITEM_SEPARATOR) continue;
        int w = label_width(items[i].label ? items[i].label : "");
        if (w > *labels) *labels = w;
        char key[32]; du_format_key(key, sizeof(key), items[i].key, items[i].modifiers);
        if (items[i].key && du_text_width(key) > widest) widest = du_text_width(key);
    }
    return widest;
}

struct du_rect du_menu_box(const struct du_ui *u, const struct du_menu_item *items, unsigned n, int x, int y)
{
    int labels, accel = accel_width(items, n, &labels), h = 6;
    for (unsigned i = 0; i < n; i++) h += item_h(&items[i]);
    int w = 3 + 20 + labels + (accel ? 20 + accel : 0) + 20 + 3;
    if (w < 110) w = 110;
    if (x + w > u->canvas.width) x = u->canvas.width - w;
    if (y + h > u->canvas.height) y = u->canvas.height - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    return (struct du_rect){x, y, w, h};
}

int du_menu_item_at(const struct du_menu_item *items, unsigned n, struct du_rect box, int y)
{
    int at = box.y + 3;
    for (unsigned i = 0; i < n; i++) {
        int h = item_h(&items[i]);
        if (y >= at && y < at + h) return items[i].flags & DU_ITEM_SEPARATOR ? -1 : (int)i;
        at += h;
    }
    return -1;
}

static void check(struct du_canvas *c, int x, int y, uint32_t ink, int radio)
{
    if (radio) {
        du_fill(c, (struct du_rect){x + 1, y + 1, 4, 6}, ink);
        du_fill(c, (struct du_rect){x, y + 2, 6, 4}, ink);
        return;
    }
    static const uint8_t rows[7] = {0x01, 0x03, 0x47, 0x6e, 0x7c, 0x38, 0x10};
    for (int r = 0; r < 7; r++) for (int col = 0; col < 7; col++)
        if (rows[r] & (1u << (6 - col))) du_fill(c, (struct du_rect){x + col, y + r, 1, 1}, ink);
}

static void draw_dropdown(struct du_ui *u, const struct du_menu_item *items, unsigned n, struct du_rect box)
{
    struct du_canvas *c = &u->canvas;
    int labels, accel = accel_width(items, n, &labels), platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM;
    if (platinum) {
        /* Platinum menu: light face, thin outline and a soft shadow. */
        du_shadow(c, box, 4);
        du_fill(c, box, desk_mix(DU_HILITE, DU_FACE, 110));
        du_outline(c, box, desk_mix(desk_theme->outline, DU_SHADOW, 120));
    } else { du_fill(c, box, DU_FACE); du_edge(c, box, DU_RAISED); }
    if(desk_appearance_active) du_fill(c,(struct du_rect){box.x+1,box.y+1,box.w-2,box.h-2},desk_appearance.item[AP_MENU].colour);
    int y = box.y + 3;
    for (unsigned i = 0; i < n; i++) {
        const struct du_menu_item *it = &items[i];
        int h = item_h(it);
        if (it->flags & DU_ITEM_SEPARATOR) {
            du_hline(c, box.x + 4, y + 3, box.w - 8, DU_SHADOW); du_hline(c, box.x + 4, y + 4, box.w - 8, DU_HILITE);
            y += h; continue;
        }
        int sel = (int)i == u->menu_item, disabled = (it->flags & DU_ITEM_DISABLED) != 0;
        uint32_t ink = sel ? DU_SELECT_INK : desk_appearance_active ? desk_appearance.item[AP_MENU].text : DU_INK;
        if (sel) {
            struct du_rect hl = {box.x + (platinum ? 1 : 3), y, box.w - (platinum ? 2 : 6), h};
            if (platinum) {
                struct desk_paint p = {c->pixels, c->width, c->height, c->clip.x, c->clip.y, c->clip.w, c->clip.h};
                desk_paint_vgradient(&p, hl.x, hl.y, hl.w, hl.h, desk_mix(0xffffffu, DU_SELECT, 50), DU_SELECT);
            } else du_fill(c, hl, DU_SELECT);
            if (disabled) ink = DU_GRAY;
        }
        int ty = y + (h - DU_GLYPH_H) / 2;
        char key[32] = "";
        if (it->key) du_format_key(key, sizeof(key), it->key, it->modifiers);
        int ax = box.x + 3 + 20 + labels + 20;
        if (disabled && !sel) {
            if (!platinum) du_label(c, box.x + 24, ty + 1, it->label, DU_HILITE, 1);
            du_label(c, box.x + 23, ty, it->label, DU_GRAY, 1);
            if (key[0]) { if (!platinum) du_text(c, ax + 1, ty + 1, key, DU_HILITE); du_text(c, ax, ty, key, DU_GRAY); }
        } else {
            du_label(c, box.x + 23, ty, it->label, ink, 1);
            if (key[0]) du_text(c, ax, ty, key, sel ? ink : desk_mix(ink, DU_FACE, 190));
        }
        if (it->flags & (DU_ITEM_CHECKED | DU_ITEM_RADIO) && it->flags & DU_ITEM_CHECKED)
            check(c, box.x + 9, ty + 1, ink, (it->flags & DU_ITEM_RADIO) != 0);
        (void)accel;
        y += h;
    }
}

static const struct du_menu_item *open_items(const struct du_ui *u, unsigned *n, struct du_rect *box)
{
    if (u->popup) { *n = u->popup_count; *box = du_menu_box(u, u->popup, u->popup_count, u->popup_x, u->popup_y); return u->popup; }
    if (u->menu >= 0 && (unsigned)u->menu < u->menu_count) {
        struct du_rect t = du_menu_title(u, (unsigned)u->menu);
        *n = u->menus[u->menu].count; *box = du_menu_box(u, u->menus[u->menu].items, *n, t.x, t.y + t.h);
        return u->menus[u->menu].items;
    }
    *n = 0; return NULL;
}

void du_draw_menus(struct du_ui *u)
{
    struct desk_theme role; const struct desk_theme *saved=desk_theme_role(&role,AP_MENU);
    struct du_canvas *c = &u->canvas;
    int platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM;
    if (u->menu_count && platinum) {
        struct desk_paint p = {c->pixels, c->width, c->height, c->clip.x, c->clip.y, c->clip.w, c->clip.h};
        desk_paint_vgradient(&p, 0, 0, c->width, DU_MENUBAR, desk_theme->face_top, desk_theme->face);
    }
    for (unsigned i = 0; i < u->menu_count; i++) {
        struct du_rect t = du_menu_title(u, i);
        int open = (int)i == u->menu && !u->popup;
        if (open && platinum) du_fill(c, t, DU_SELECT);
        else if (open) du_edge(c, t, DU_SUNKEN_THIN);
        int down = open && !platinum;
        du_label(c, t.x + 8 + down, t.y + (t.h - DU_GLYPH_H) / 2 + down, u->menus[i].label,
                 open && platinum ? DU_SELECT_INK : DU_INK, 1);
    }
    unsigned n; struct du_rect box;
    const struct du_menu_item *items = open_items(u, &n, &box);
    if (items) {
        struct du_rect saved=c->clip;
        if(u->menu_opened && u->now>=u->menu_opened && u->now-u->menu_opened<120000000ull) {
            int shown=(int)((u->now-u->menu_opened)/20000000ull+1)*box.h/6;
            c->clip=du_intersection(c->clip,(struct du_rect){box.x,box.y,box.w,shown});
        }
        draw_dropdown(u, items, n, box); c->clip=saved;
    }
    desk_theme=saved;
}

static void menu_damage(struct du_ui *u)
{
    unsigned count; struct du_rect box;
    if (open_items(u,&count,&box)) { box.x-=3; box.y-=2; box.w+=8; box.h+=8; du_invalidate(u,box); }
    if (u->menu>=0) du_invalidate(u,du_menu_title(u,(unsigned)u->menu));
}

void du_menu_open(struct du_ui *u, int menu, int keyboard)
{
    if (menu < 0 || (unsigned)menu >= u->menu_count) return;
    if (u->sound) u->sound(u->sound_ctx,SOUND_MENU_OPEN);
    menu_damage(u);
    u->menu_opened=desk_appearance_active && (desk_appearance.effects&AP_MENU_ANIMATION) ? u->now : 0;
    u->menu_animation_step=0;
    u->popup = NULL; u->menu = menu; u->menu_keyboard = keyboard; u->menu_track = 0; u->menu_item = -1;
    if (keyboard)
        for (unsigned i = 0; i < u->menus[menu].count; i++)
            if (!(u->menus[menu].items[i].flags & DU_ITEM_SEPARATOR)) { u->menu_item = (int)i; break; }
    menu_damage(u);
}

void du_menu_close(struct du_ui *u)
{
    menu_damage(u);
    u->menu = -1; u->popup = NULL; u->popup_count = 0; u->menu_item = -1; u->menu_track = 0;
}

void du_popup(struct du_ui *u, const struct du_menu_item *items, unsigned n, int x, int y, int owner)
{
    if (u->sound) u->sound(u->sound_ctx,SOUND_MENU_OPEN);
    menu_damage(u);
    u->menu_opened=desk_appearance_active && (desk_appearance.effects&AP_MENU_ANIMATION) ? u->now : 0;
    u->menu_animation_step=0;
    u->menu = -1; u->popup = items; u->popup_count = n; u->popup_x = x; u->popup_y = y;
    u->popup_owner = owner; u->menu_item = -1; u->menu_track = 0; menu_damage(u);
}

static void activate(struct du_ui *u, const struct du_menu_item *items, int index)
{
    if (index < 0) return;
    const struct du_menu_item *it = &items[index];
    if (it->flags & (DU_ITEM_DISABLED | DU_ITEM_SEPARATOR)) return;
    if (u->sound) u->sound(u->sound_ctx,SOUND_MENU_SELECT);
    int id = it->id, owner = u->popup ? u->popup_owner : -1;
    du_menu_close(u);
    du_emit(u, (struct du_event){.type = DU_EV_ACTIVATE, .id = id, .index = owner});
}

static int title_at(const struct du_ui *u, int x, int y)
{
    for (unsigned i = 0; i < u->menu_count; i++) if (du_contains(du_menu_title(u, i), x, y)) return (int)i;
    return -1;
}

int du_menu_press(struct du_ui *u, int button)
{
    unsigned n; struct du_rect box;
    const struct du_menu_item *items = open_items(u, &n, &box);
    int title = title_at(u, u->mouse_x, u->mouse_y);
    if (items) {
        if (du_contains(box, u->mouse_x, u->mouse_y)) { u->menu_track = 1; return 1; }
        if (!u->popup && title >= 0 && button == 1) {
            if (title == u->menu) du_menu_close(u); else du_menu_open(u, title, 0);
            return 1;
        }
        du_menu_close(u);
        return 1;
    }
    if (u->menu_count && u->mouse_y >= 0 && u->mouse_y < DU_MENUBAR) {
        if (title >= 0 && button == 1) du_menu_open(u, title, 0);
        return 1;
    }
    return 0;
}

int du_menu_release(struct du_ui *u, int button)
{
    (void)button;
    unsigned n; struct du_rect box;
    const struct du_menu_item *items = open_items(u, &n, &box);
    if (!items) return 0;
    if (u->menu_track && du_contains(box, u->mouse_x, u->mouse_y))
        activate(u, items, du_menu_item_at(items, n, box, u->mouse_y));
    return 1;
}

void du_menu_motion(struct du_ui *u)
{
    unsigned n; struct du_rect box;
    const struct du_menu_item *items = open_items(u, &n, &box);
    if (!items) return;
    int title = title_at(u, u->mouse_x, u->mouse_y);
    if (!u->popup && title >= 0 && title != u->menu) { du_menu_open(u, title, 0); return; }
    int item = du_contains(box, u->mouse_x, u->mouse_y) ? du_menu_item_at(items, n, box, u->mouse_y) : -1;
    if (item >= 0) u->menu_track = 1;
    if (item != u->menu_item && (item >= 0 || !u->menu_keyboard)) { u->menu_item = item; menu_damage(u); }
}

int du_menu_key(struct du_ui *u, unsigned code)
{
    unsigned n; struct du_rect box;
    const struct du_menu_item *items = open_items(u, &n, &box);
    if (!items) return 0;
    u->menu_keyboard = 1;
    if (code == DU_KEY_ESC || code == DU_KEY_F10) { du_menu_close(u); return 1; }
    if ((code == DU_KEY_UP || code == DU_KEY_DOWN) && n) {
        int i = u->menu_item;
        for (unsigned k = 0; k < n; k++) {
            i = code == DU_KEY_UP ? (i <= 0 ? (int)n - 1 : i - 1) : (i + 1) % (int)n;
            if (!(items[i].flags & DU_ITEM_SEPARATOR)) break;
        }
        u->menu_item = i; menu_damage(u); return 1;
    }
    if ((code == DU_KEY_LEFT || code == DU_KEY_RIGHT) && !u->popup && u->menu_count) {
        int m = (u->menu + (code == DU_KEY_LEFT ? (int)u->menu_count - 1 : 1)) % (int)u->menu_count;
        du_menu_open(u, m, 1); return 1;
    }
    if (code == DU_KEY_ENTER || code == DU_KEY_SPACE) { activate(u, items, u->menu_item); return 1; }
    unsigned saved = u->modifiers; u->modifiers &= ~DU_ALT;
    unsigned ch = fold(du_key_char(u, code)); u->modifiers = saved;
    if (ch) for (unsigned i = 0; i < n; i++)
        if (!(items[i].flags & DU_ITEM_SEPARATOR) && mnemonic(items[i].label) == ch) { activate(u, items, (int)i); return 1; }
    return 1;
}

int du_menu_accelerator(struct du_ui *u, unsigned code)
{
    for (unsigned m = 0; m < u->menu_count; m++) for (unsigned i = 0; i < u->menus[m].count; i++) {
        const struct du_menu_item *it = &u->menus[m].items[i];
        if (it->key == code && it->modifiers == u->modifiers && !(it->flags & (DU_ITEM_DISABLED | DU_ITEM_SEPARATOR))) {
            du_emit(u, (struct du_event){.type = DU_EV_ACTIVATE, .id = it->id, .index = -1, .keyboard = 1});
            return 1;
        }
    }
    return 0;
}

int du_menu_mnemonic(struct du_ui *u, unsigned ch)
{
    ch = fold(ch);
    for (unsigned i = 0; i < u->menu_count; i++) if (mnemonic(u->menus[i].label) == ch) { du_menu_open(u, (int)i, 1); return 1; }
    return 0;
}
