/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include <string.h>
static void route(struct desktop *d, int i, unsigned op, const struct ciuki_input_event *e)
{
    struct ciuki_message m;
    desk_event_message(&m, op, e); desk_queue(d, i, &m);
}
/* A key edge the shell held back (Alt) and now delivers to the focus. */
void desk_route_key(struct desktop *d, int code, int value)
{
    struct ciuki_input_event e = { .type = CIUKI_INPUT_KEY, .monotonic_ns = d->now_ns, .code = code, .value = value };
    if (code < 0 || code > 0x200) return;
    d->keys[code] = (uint8_t)(value != 0 && d->focus >= 0);
    route(d, d->focus, DESK_KEY, &e);
}
static void release(struct desktop *d, const struct ciuki_input_event *base)
{
    struct ciuki_input_event e = *base;
    e.type = CIUKI_INPUT_KEY; e.value = e.value2 = 0; e.lost_count = 0;
    for (int k = 0; k <= 0x200; k++) if (d->keys[k]) {
        e.code = k; route(d, d->focus, DESK_KEY, &e); d->keys[k] = 0;
    }
    e.type = CIUKI_INPUT_BUTTON;
    for (int b = 1; b <= 3; b++) if (d->routed_buttons & (1u << (b-1))) {
        e.code = b; route(d, d->focus, DESK_BUTTON, &e);
    }
    d->routed_buttons = 0;
}
void desk_focus(struct desktop *d, int i)
{
    if (i >= 0 && (i >= DESK_CLIENTS || !d->clients[i].window || d->clients[i].minimized)) return;
    if (i == d->focus) return;
    struct ciuki_input_event e = { .monotonic_ns = d->now_ns };
    release(d, &e);
    d->drag=-1;
    struct ciuki_message m;
    desk_message(&m, DESK_FOCUS, 8);
    if (d->focus >= 0) {
        desk_queue(d, d->focus, &m);
        desk_damage_add(&d->damage, d->clients[d->focus].frame, d->width, d->height);
    }
    d->focus = i;
    if (i >= 0) {
        d->clients[i].order = ++d->order;
        desk_put32(m.data + 4, 1); desk_queue(d, i, &m);
        desk_damage_add(&d->damage, d->clients[i].frame, d->width, d->height);
    }
    if (d->shell) {
        desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,DESK_BAR},d->width,d->height);   /* top bar title */
    }
}
int desk_contains(struct desk_box b, int32_t x, int32_t y)
{ return x >= b.x && y >= b.y && (int64_t)x < (int64_t)b.x+b.w && (int64_t)y < (int64_t)b.y+b.h; }
int desk_hit(const struct desktop *d, int32_t x, int32_t y)
{
    int best = -1;
    for (int i = 0; i < DESK_CLIENTS; i++) if (d->clients[i].window && !d->clients[i].minimized &&
        desk_contains(d->clients[i].frame, x, y) &&
        (best < 0 || d->clients[i].order > d->clients[best].order)) best = i;
    return best;
}
static int32_t axis(int32_t old, int32_t delta, uint32_t limit)
{
    int64_t n = (int64_t)old + delta;
    if (n < 0) return 0;
    if (n >= limit) return (int32_t)limit - 1;
    return (int32_t)n;
}
void desk_input(struct desktop *d, const struct ciuki_input_event *in)
{
    d->inputs++;
    struct ciuki_input_event e = *in;
    switch (e.type) {
    case CIUKI_INPUT_RESYNC:
        release(d, &e); d->drag = -1;
        d->buttons = (uint32_t)e.value & 7u;
        route(d, d->focus, DESK_KEY, &e);
        break;
    case CIUKI_INPUT_KEY:
        if (e.code < 0 || e.code > 0x200 || e.value < 0 || e.value > 2) break;
        /* Shell shortcuts and menus (shell.c): Win, Ctrl+Esc, Alt+Tab, Alt+F4, menu keys, built-in windows. */
        if (d->shell && desk_shell_key(d, e.code, e.value)) break;
        d->keys[e.code] = (uint8_t)(e.value != 0 && d->focus >= 0);
        route(d, d->focus, DESK_KEY, &e);
        break;
    case CIUKI_INPUT_TEXT: route(d, d->focus, DESK_KEY, &e); break;
    case CIUKI_INPUT_MOTION:
        desk_damage_add(&d->damage, (struct desk_box){d->mouse_x,d->mouse_y,12,18}, d->width,d->height);
        {
            int32_t dx = e.value, dy = e.value2;
            desk_shell_scale_motion(d, &dx, &dy);   /* Control Panel mouse speed; 1:1 by default */
            d->mouse_x = axis(d->mouse_x, dx, d->width);
            d->mouse_y = axis(d->mouse_y, dy, d->height);
        }
        desk_damage_add(&d->damage, (struct desk_box){d->mouse_x,d->mouse_y,12,18}, d->width,d->height);
        if (d->shell) desk_shell_motion(d);
        if (d->drag >= 0) {
            desk_move(d, d->drag, d->mouse_x - d->drag_x, d->mouse_y - d->drag_y);
        } else if (d->focus >= 0) {
            struct desk_box content = desk_content(&d->clients[d->focus]);
            /* MOTION carries client-relative absolute position in value/value2;
             * original delta is intentionally replaced at the protocol boundary. */
            e.code = 0; e.value = d->mouse_x-content.x; e.value2 = d->mouse_y-content.y;
            route(d, d->focus, DESK_MOTION, &e);
        }
        break;
    case CIUKI_INPUT_BUTTON: {
        if (e.code < 1 || e.code > 3 || (e.value != 0 && e.value != 1)) break;
        uint32_t mask = 1u << (e.code-1);
        if (e.value) d->buttons |= mask; else d->buttons &= ~mask;
        if (!e.value && d->drag >= 0 && e.code == CIUKI_BUTTON_LEFT) {
            d->drag = -1; break;
        }
        int i = desk_hit(d,d->mouse_x,d->mouse_y);
        /* Start menu, taskbar, tray, desktop icons and context menus (shell.c). */
        if (d->shell && desk_shell_button(d, e.code, e.value)) break;
        if (e.value && e.code == CIUKI_BUTTON_LEFT) {
            desk_focus(d, i);
            if (i >= 0) {
                struct desk_client *c = &d->clients[i];
                if (desk_contains(desk_close_box(c), d->mouse_x, d->mouse_y)) {
                    if (d->shell) { desk_request_close(d,i); break; }
                    struct ciuki_message m;
                    desk_message(&m, DESK_CLOSED, 4);
                    d->ops.send(c->channel, &m, DONTWAIT);
                    desk_drop_client(d, i); break;
                }
                if (d->shell && desk_contains(desk_min_box(c),d->mouse_x,d->mouse_y)) {
                    desk_minimize(d,i); break;
                }
                if (d->mouse_y < c->frame.y + DESK_TITLE) {
                    d->drag = i; d->drag_x = d->mouse_x - c->frame.x;
                    d->drag_y = d->mouse_y - c->frame.y; break;
                }
                if (c->internal && desk_contains(desk_content(c), d->mouse_x, d->mouse_y)) {
                    desk_internal_click(d, i, d->mouse_x, d->mouse_y); break;
                }
            }
        }
        if (d->focus >= 0 && ((e.value && i == d->focus &&
            desk_contains(desk_content(&d->clients[i]), d->mouse_x, d->mouse_y)) ||
            (!e.value && (d->routed_buttons & mask)))) {
            if (e.value) d->routed_buttons |= mask; else d->routed_buttons &= ~mask;
            e.value2 = 0; route(d, d->focus, DESK_BUTTON, &e);
        }
        break;
    }
    default: break;
    }
}
