/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_DESKUI_H
#define CIUKI_DESKUI_H
#include <stdint.h>
#include <stddef.h>
#include "../desktop/protocol.h"
#define DU_WIDGETS 96
#define DU_TIMERS 8
#define DU_TEXT_MAX 1040
#define DU_BG 0xe9e5dcu
#define DU_INK 0x213c4cu
#define DU_SELECTED 0x375564u
#define DU_WHITE 0xffffffu
struct du_rect { int x,y,w,h; };
struct du_canvas { uint32_t *pixels; unsigned width,height; struct du_rect clip; };
enum du_kind { DU_BUTTON, DU_EDIT, DU_LIST, DU_TREE, DU_SCROLLBAR, DU_STATUS,
               DU_SPLITTER, DU_PROGRESS, DU_LABEL };
enum du_view { DU_DETAILS, DU_ICONS, DU_LIST_MODE };
enum du_event_kind { DU_ACTIVATE, DU_CHANGE, DU_SELECT, DU_SORT, DU_CONTEXT,
                     DU_CLOSE, DU_TICK };
struct du_event { enum du_event_kind type; int id,index,x,y; unsigned modifiers; };
#define DU_SHIFT 1u
#define DU_CTRL 2u
#define DU_ALT 4u
struct du_row { const char *name,*size,*type,*modified; const uint32_t *icon;
                unsigned selected,depth,expanded; };
struct du_widget {
    enum du_kind kind; int id; struct du_rect box; int disabled,hidden;
    char text[DU_TEXT_MAX]; size_t cursor,anchor,offset;
    const uint32_t *icon;
    struct du_row *rows; unsigned count,first,active; enum du_view view;
    unsigned value,total,page;
};
struct du_menu_item { const char *label; int id; unsigned key,modifiers; int disabled; };
struct du_menu { const char *label; const struct du_menu_item *items; unsigned count; };
enum du_dialog_kind { DU_MESSAGE, DU_INPUT, DU_OPEN, DU_SAVE, DU_JOB };
struct du_dialog { int visible; enum du_dialog_kind kind; int id,result,focus;
    char title[80],message[512]; struct du_widget edit;
    const char *buttons[4]; unsigned count,progress,total; };
struct du_timer { uint64_t deadline,interval; int id,active; };
struct du_ui {
    struct du_canvas canvas; struct du_widget widgets[DU_WIDGETS]; unsigned count;
    const struct du_menu *menus; unsigned menu_count; int menu,menu_item,menu_keyboard;
    const struct du_menu_item *context; unsigned context_count; int context_x,context_y;
    int focus,capture,mouse_x,mouse_y; unsigned modifiers;
    uint8_t keys[513]; unsigned buttons;
    int dirty; struct du_dialog dialog; struct du_timer timers[DU_TIMERS];
    void (*event)(struct du_ui *,const struct du_event *,void *); void *user;
};
struct du_window {
    int channel,surface,configured,closing,closed,error; uint32_t server_pid;
    struct ciuki_surface_info info; uint32_t *pixels;
    struct ciuki_message tx[16]; unsigned head,count;
    struct du_rect damage; int damaged;
    struct du_ui *ui;
    int (*close_request)(struct du_window *,void *); void *user;
};
int du_contains(struct du_rect,int,int);
struct du_rect du_intersection(struct du_rect,struct du_rect);
void du_fill(struct du_canvas *,struct du_rect,uint32_t);
void du_text(struct du_canvas *,int,int,const char *,uint32_t);
void du_icon(struct du_canvas *,int,int,const uint32_t *,unsigned);
void du_bevel(struct du_canvas *,struct du_rect,int);
void du_init(struct du_ui *,uint32_t *,unsigned,unsigned);
struct du_widget *du_add(struct du_ui *,enum du_kind,int,struct du_rect,const char *);
struct du_widget *du_get(struct du_ui *,int);
int du_hit(const struct du_ui *,int,int);
int du_list_hit(const struct du_widget *,int,int);
void du_render(struct du_ui *);
void du_input(struct du_ui *,const struct ciuki_input_event *);
void du_release(struct du_ui *);
void du_focus(struct du_ui *,int);
void du_tick(struct du_ui *,uint64_t);
int du_timer_start(struct du_ui *,int,uint64_t,uint64_t);
void du_timer_stop(struct du_ui *,int);
void du_context(struct du_ui *,const struct du_menu_item *,unsigned,int,int);
void du_dialog_show(struct du_ui *,enum du_dialog_kind,int,const char *,const char *,const char *);
void du_dialog_buttons(struct du_ui *,const char *const *,unsigned);
void du_clipboard_set(const char *);
const char *du_clipboard_get(void);
int du_window_open(struct du_window *,struct du_ui *,int,unsigned,unsigned,const char *);
void du_window_damage(struct du_window *,struct du_rect);
int du_window_turn(struct du_window *,uint64_t);
int du_window_ping(struct du_window *,uint32_t);
int du_window_move(struct du_window *,int,int);
void du_window_close(struct du_window *);
void du_window_destroy(struct du_window *);
uint64_t du_now(void);
#endif
