/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_APP_DESKTOP_H
#define CIUKI_APP_DESKTOP_H
#include "protocol.h"
#include <stddef.h>
#define DESK_CLIENTS 16
#define DESK_DAMAGE_MAX 32
#define DESK_TX_MAX 64
#define DESK_BAR 32
#define DESK_BORDER 3
#define DESK_TITLE 24
#define DESK_BACKGROUND 0x375564u
#define DESK_PORTRAIT_SIZE 256
struct desk_box { int32_t x, y, w, h; };
struct desk_damage { unsigned count; struct desk_box boxes[DESK_DAMAGE_MAX]; };
struct desk_client {
    int channel, surface, hello, window, unresponsive, ping_pending, resync_pending;
    int configure_pending, focus_pending;
    uint32_t pid, serial;
    uint64_t ping_sent_ns, next_ping_ns, order;
    struct ciuki_surface_info info;
    const uint32_t *pixels;
    struct desk_box frame;
    char title[DESK_TITLE_MAX + 1];
    struct ciuki_message tx[DESK_TX_MAX];
    unsigned tx_head, tx_count;
};
struct desk_ops {
    int (*send)(int, const struct ciuki_message *, uint32_t);
    int (*info)(int, struct ciuki_surface_info *);
    void *(*map)(int, int);
    int (*unmap)(void *, size_t);
    int (*close)(int);
    int (*present)(int, int, const struct ciuki_rect *);
};
struct desktop {
    struct desk_ops ops;
    int display, input, output, focus, drag;
    uint32_t width, height, *pixels;
    const uint32_t *portrait;
    int32_t mouse_x, mouse_y, drag_x, drag_y;
    uint8_t keys[513];
    uint32_t buttons, routed_buttons;
    uint64_t order, now_ns, clock_second, presents, inputs, received, replied;
    struct desk_damage damage;
    struct desk_client clients[DESK_CLIENTS];
};
int desk_intersect(struct desk_box a, struct desk_box b, struct desk_box *out);
struct desk_box desk_frame(uint32_t w, uint32_t h, int32_t x, int32_t y);
struct desk_box desk_content(const struct desk_client *c);
struct desk_box desk_close_box(const struct desk_client *c);
void desk_damage_add(struct desk_damage *, struct desk_box, uint32_t, uint32_t);
void desk_init(struct desktop *, const struct desk_ops *, uint32_t, uint32_t);
int desk_ignore_sigpipe(void);
int desk_add_client(struct desktop *, int channel);
void desk_drop_client(struct desktop *, int index);
void desk_receive(struct desktop *, int index, const struct ciuki_message *);
void desk_queue(struct desktop *, int index, const struct ciuki_message *);
void desk_flush(struct desktop *, int index);
void desk_heartbeat(struct desktop *, uint64_t now_ns);
void desk_focus(struct desktop *, int index);
void desk_move(struct desktop *, int index, int32_t x, int32_t y);
void desk_input(struct desktop *, const struct ciuki_input_event *);
void desk_composite(struct desktop *, struct desk_box);
void desk_text(struct desktop *, struct desk_box, int32_t, int32_t,
               const char *, int, uint32_t);
uint8_t desk_glyph_row(unsigned, unsigned);
int desk_redraw(struct desktop *);
int desk_load_portrait(uint32_t *pixels);
#endif
