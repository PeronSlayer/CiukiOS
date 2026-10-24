/* SPDX-License-Identifier: MIT
 * Private libdeskui interfaces shared by the widget modules. */
#ifndef CIUKI_DESKUI_INTERNAL_H
#define CIUKI_DESKUI_INTERNAL_H
#include "deskui.h"

#define DU_MENUBAR 20
#define DU_DOUBLE_CLICK_NS 500000000ull

void du_region_add(struct du_rect *, unsigned *, struct du_rect);
void du_emit(struct du_ui *, struct du_event);
int du_focusable(const struct du_widget *);
void du_draw_widget(struct du_ui *, struct du_widget *, int focused);
int du_widget_key(struct du_ui *, struct du_widget *, unsigned code);
int du_widget_wheel(struct du_ui *, struct du_widget *, int detents);
int du_widget_press(struct du_ui *, struct du_widget *, int button, int x, int y);
void du_widget_motion(struct du_ui *, struct du_widget *, int x, int y);
void du_widget_release(struct du_ui *, struct du_widget *, int button, int x, int y);
void du_edit_insert(struct du_widget *, const char *);
int du_edit_key(struct du_ui *, struct du_widget *, unsigned code);
void du_edit_press(struct du_widget *, int x, int extend);
void du_draw_edit(struct du_canvas *, struct du_widget *, int focused);
void du_draw_button(struct du_canvas *, const struct du_widget *, int focused);

/* list.c */
void du_draw_list(struct du_canvas *, struct du_widget *, int focused, int window_focused);
void du_draw_tree(struct du_canvas *, struct du_widget *, int focused, int window_focused);
int du_list_key(struct du_ui *, struct du_widget *, unsigned code);
int du_list_press(struct du_ui *, struct du_widget *, int button, int x, int y);
void du_list_motion(struct du_ui *, struct du_widget *, int x, int y);
void du_list_release(struct du_ui *, struct du_widget *);
#define DU_TIMER_BAND (-0x4642)   /* fx-19: toolkit-owned marquee auto-scroll timer */
void du_list_band_tick(struct du_ui *, struct du_widget *);
void du_list_scrolled(struct du_ui *, struct du_widget *, int old_v, int old_h);
int du_list_wheel(struct du_widget *, int detents);

/* Scrollbar geometry shared by standalone and embedded scrollbars. */
struct du_sb { struct du_rect box; int vertical, value, total, page; };
void du_sb_draw(struct du_canvas *, const struct du_sb *, int pressed_part);
int du_sb_part(const struct du_sb *, int x, int y);   /* 1 dec, 2 page dec, 3 thumb, 4 page inc, 5 inc */
int du_sb_apply(const struct du_sb *, int part);
int du_sb_drag(const struct du_sb *, int x, int y, int grab);
int du_sb_grab(const struct du_sb *, int x, int y);
int du_sb_clamp(int value, int total, int page);
int du_wheel_value(int value, int detents, int total, int page);

/* menu.c */
void du_draw_menus(struct du_ui *);
int du_menu_key(struct du_ui *, unsigned code);
int du_menu_press(struct du_ui *, int button);
void du_menu_motion(struct du_ui *);
int du_menu_release(struct du_ui *, int button);
int du_menu_accelerator(struct du_ui *, unsigned code);
int du_menu_mnemonic(struct du_ui *, unsigned ch);

/* controls.c (G16) */
#define DU_TIMER_REPEAT (-0x4650)   /* held up-down arrow or trackbar page click */
void du_draw_combo(struct du_canvas *, struct du_widget *, int focused, int window_focused);
int du_combo_key(struct du_ui *, struct du_widget *, unsigned code);
int du_combo_press(struct du_ui *, struct du_widget *, int button, int x, int y);
void du_combo_overlay_key(struct du_ui *, unsigned code);
void du_combo_overlay_motion(struct du_ui *);
void du_combo_overlay_button(struct du_ui *, int button, int down);
void du_combo_overlay_wheel(struct du_ui *, int detents);
void du_draw_trackbar(struct du_canvas *, struct du_widget *, int focused);
int du_trackbar_key(struct du_ui *, struct du_widget *, unsigned code);
int du_trackbar_wheel(struct du_ui *, struct du_widget *, int detents);
int du_trackbar_press(struct du_ui *, struct du_widget *, int button, int x, int y);
void du_trackbar_motion(struct du_ui *, struct du_widget *, int x, int y);
void du_trackbar_release(struct du_ui *, struct du_widget *);
void du_draw_updown(struct du_canvas *, struct du_widget *);
int du_updown_press(struct du_ui *, struct du_widget *, int button, int x, int y);
void du_updown_motion(struct du_ui *, struct du_widget *, int x, int y);
void du_updown_release(struct du_ui *, struct du_widget *);
int du_updown_buddy_key(struct du_ui *, struct du_widget *edit, unsigned code);
void du_repeat_tick(struct du_ui *);
void du_undo_note(struct du_ui *, const struct du_widget *, const char *old_text, size_t old_cursor, size_t old_anchor, int kind);
void du_tip_hide(struct du_ui *);
void du_tip_motion(struct du_ui *, struct du_widget *under);
void du_tip_tick(struct du_ui *);
uint64_t du_tip_deadline(const struct du_ui *);
void du_draw_overlays(struct du_ui *);
void du_overlays_forget(struct du_ui *, const struct du_widget *items, unsigned count);

/* dialog.c */
/* Run fn(u, w, arg) with the dialog's event capture, as for a control of the dialog. */
void du_dialog_route(struct du_ui *, void (*fn)(struct du_ui *, struct du_widget *, int), struct du_widget *, int arg);
struct du_widget *du_dialog_item(struct du_ui *, unsigned index, enum du_kind, int id, const char *text);
void du_dialog_begin(struct du_ui *, enum du_dialog_kind, int id, const char *title, const char *text);
void du_dialog_place(struct du_ui *, int w, int h);
void du_dialog_damage(struct du_ui *);
int du_dialog_body_y(const struct du_dialog *);
void du_dialog_finish(struct du_ui *, int index, const char *text);   /* close, then DU_EV_DIALOG */
void du_draw_dialog(struct du_ui *);
void du_dialog_key(struct du_ui *, unsigned code);
void du_dialog_wheel(struct du_ui *, int detents);
void du_dialog_press(struct du_ui *, int button);
void du_dialog_motion(struct du_ui *);
void du_dialog_release(struct du_ui *, int button);
void du_dialog_tick(struct du_ui *);
uint64_t du_dialog_deadline(const struct du_ui *);
#endif
