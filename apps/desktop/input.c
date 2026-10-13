/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include <string.h>
#include <errno.h>
int desk_drain_input(struct desktop *d, int (*read_events)(int, struct ciuki_input_event *, uint32_t))
{
    uint64_t begin=d->time_ns ? d->time_ns() : d->now_ns;
    int total=0, n;
    struct ciuki_input_event events[CIUKI_INPUT_READ_MAX];
    do {
        n=read_events(d->input,events,CIUKI_INPUT_READ_MAX);
        if (d->time_ns) d->now_ns=d->time_ns();
        for (int i=0;i<n;i++) desk_input(d,&events[i]);
        if (n>0) total+=n;
    } while (n>0);
    int error=errno;
    uint64_t elapsed=(d->time_ns ? d->time_ns() : d->now_ns)-begin;
    d->stats.input_ns+=elapsed; d->pending_input_ns+=elapsed;
    errno=error;
    return n<0 && error!=EAGAIN && error!=EINTR ? -1 : total;
}
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
    if (d->power.state && i>=0 && i<DESK_CLIENTS && d->clients[i].internal!=DESK_INTERNAL_POWER) return;
    if (i >= 0 && (i >= DESK_CLIENTS || !d->clients[i].window || d->clients[i].minimized)) return;
    if (i == d->focus) return;
    struct ciuki_input_event e = { .monotonic_ns = d->now_ns };
    release(d, &e);
    d->drag=-1;
    struct ciuki_message m;
    desk_message(&m, DESK_FOCUS, 8);
    if (d->focus >= 0) {
        desk_queue(d, d->focus, &m);
        desk_damage_add(&d->damage, desk_outer(&d->clients[d->focus]), d->width, d->height);
    }
    d->focus = i;
    if (i >= 0) {
        d->clients[i].order = ++d->order;
        desk_put32(m.data + 4, 1); desk_queue(d, i, &m);
        desk_damage_add(&d->damage, desk_outer(&d->clients[i]), d->width, d->height);
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
        (desk_contains(d->clients[i].frame, x, y) || desk_resize_hit(&d->clients[i], x, y)) &&
        (best < 0 || d->clients[i].order > d->clients[best].order)) best = i;
    return best;
}
unsigned desk_resize_hit(const struct desk_client *c, int32_t x, int32_t y)
{
    if (!c->resizable || c->internal || c->collapsed) return 0;
    struct desk_box f = c->frame;
    int m = DESK_RESIZE_MARGIN;
    if (!desk_contains((struct desk_box){f.x-m,f.y-m,f.w+2*m,f.h+2*m},x,y)) return 0;
    unsigned edge = 0;
    if (x < f.x+DESK_BORDER) edge |= DESK_EDGE_LEFT;
    if (x >= f.x+f.w-DESK_BORDER) edge |= DESK_EDGE_RIGHT;
    if (y < f.y+DESK_BORDER) edge |= DESK_EDGE_TOP;
    if (y >= f.y+f.h-DESK_BORDER) edge |= DESK_EDGE_BOTTOM;
    /* Keep the 12 px bottom-right grip below the title buttons, even for
     * short client surfaces. Title buttons are dispatched before edges. */
    if (x >= f.x+f.w-12 && y >= f.y+f.h-12 && y >= f.y+DESK_TITLE)
        edge = DESK_EDGE_RIGHT | DESK_EDGE_BOTTOM;
    return edge;
}
/* Title-button hover and press: the compositor paints them from the pointer
 * (desk_title_code); this repaints the boxes whose state changed. */
static void title_track(struct desktop *d)
{
    int hot = desk_title_code(d, d->mouse_x, d->mouse_y);
    if (hot == d->title_hot) return;
    desk_damage_add(&d->damage, desk_title_code_box(d, d->title_hot), d->width, d->height);
    desk_damage_add(&d->damage, desk_title_code_box(d, hot), d->width, d->height);
    d->title_hot = hot;
}
static void title_press(struct desktop *d, int code)
{
    if (code == d->title_down) return;
    desk_damage_add(&d->damage, desk_title_code_box(d, d->title_down), d->width, d->height);
    desk_damage_add(&d->damage, desk_title_code_box(d, code), d->width, d->height);
    d->title_down = code;
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
    /* Preserve the timestamp of a deferred menu action across the remaining
     * input batch. Immediate icon/menu actions consume it in launch_program. */
    if (d->action < 0) d->launch_click_ns = in->monotonic_ns ? in->monotonic_ns : d->now_ns;
    d->inputs++;
    if (in->monotonic_ns && d->now_ns>=in->monotonic_ns &&
        d->now_ns-in->monotonic_ns>d->stats.input_lag_ns)
        d->stats.input_lag_ns=d->now_ns-in->monotonic_ns;
    if (in->monotonic_ns && d->now_ns>=in->monotonic_ns &&
        d->now_ns-in->monotonic_ns>d->pending_input_lag_ns)
        d->pending_input_lag_ns=d->now_ns-in->monotonic_ns;
    struct ciuki_input_event e = *in;
    if (d->sh.want_power && !d->sh.gate && !d->power.state) return;
    if (d->power.state) {
        if (e.type==CIUKI_INPUT_MOTION) {
            d->cursor_dirty=1;
            int32_t dx=e.value, dy=e.value2;
            desk_shell_scale_motion(d,&dx,&dy);
            d->mouse_x=axis(d->mouse_x,dx,d->width); d->mouse_y=axis(d->mouse_y,dy,d->height);
        }
        desk_power_input(d,&e);
        return;
    }
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
        d->cursor_dirty=1;
        {
            int32_t dx = e.value, dy = e.value2;
            desk_shell_scale_motion(d, &dx, &dy);   /* Control Panel mouse speed; 1:1 by default */
            d->mouse_x = axis(d->mouse_x, dx, d->width);
            d->mouse_y = axis(d->mouse_y, dy, d->height);
        }
        if (d->shell) desk_shell_motion(d);
        if (d->drag >= 0) {
            if (d->resize_edges) {
                struct desk_box f = d->resize_start;
                int dx = d->mouse_x-d->drag_x, dy = d->mouse_y-d->drag_y;
                if (d->resize_edges & DESK_EDGE_LEFT) { f.x += dx; f.w -= dx; }
                if (d->resize_edges & DESK_EDGE_RIGHT) f.w += dx;
                if (d->resize_edges & DESK_EDGE_TOP) { f.y += dy; f.h -= dy; }
                if (d->resize_edges & DESK_EDGE_BOTTOM) f.h += dy;
                if (f.x < 0) { f.w += f.x; f.x = 0; }
                if (f.y < DESK_BAR) { f.h -= DESK_BAR-f.y; f.y = DESK_BAR; }
                int32_t bottom = (int32_t)d->height-(d->shell?DESK_BAR:0);
                if (f.x+f.w > (int32_t)d->width) f.w = (int32_t)d->width-f.x;
                if (f.y+f.h > bottom) f.h = bottom-f.y;
                desk_resize(d, d->drag, f);
            } else desk_move(d, d->drag, d->mouse_x - d->drag_x, d->mouse_y - d->drag_y);
        } else title_track(d);
        if (d->drag < 0 && d->focus >= 0) {
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
        if (!e.value && e.code == CIUKI_BUTTON_LEFT) title_press(d, 0);
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
                title_press(d, desk_title_code(d, d->mouse_x, d->mouse_y));
                if (desk_contains(desk_close_box(c), d->mouse_x, d->mouse_y)) {
                    if (d->shell) { desk_request_close(d,i); break; }
                    struct ciuki_message m;
                    desk_message(&m, DESK_CLOSED, 4);
                    m.data[0] = (uint8_t)c->version; m.data[2] = (uint8_t)c->minor;
                    d->ops.send(c->channel, &m, DONTWAIT);
                    desk_drop_client(d, i); break;
                }
                if (d->shell && desk_contains(desk_min_box(c),d->mouse_x,d->mouse_y)) {
                    desk_minimize(d,i); break;
                }
                if (d->shell && desk_contains(desk_shade_box(c),d->mouse_x,d->mouse_y)) {
                    desk_zoom(d,i); break;
                }
                unsigned edges = desk_resize_hit(c,d->mouse_x,d->mouse_y);
                if (edges) {
                    c->zoomed = 0; d->drag = i; d->resize_edges = edges;
                    d->resize_start = c->frame; d->drag_x = d->mouse_x; d->drag_y = d->mouse_y;
                    d->title_click_window = -1; break;
                }
                if (d->mouse_y < c->frame.y + DESK_TITLE) {
                    /* Shell: a double-click on the title bar rolls the window
                     * up to its title (Mac OS 8/9 windowshade) or back down. */
                    int32_t dx = d->mouse_x - d->title_click_x, dy = d->mouse_y - d->title_click_y;
                    if (d->shell && d->title_click_window == i && e.monotonic_ns >= d->title_click_ns &&
                        e.monotonic_ns - d->title_click_ns < 500000000ull && dx <= 4 && dx >= -4 && dy <= 4 && dy >= -4) {
                        d->title_click_window = -1; desk_collapse(d,i); break;
                    }
                    d->title_click_window = i; d->title_click_ns = e.monotonic_ns;
                    d->title_click_x = d->mouse_x; d->title_click_y = d->mouse_y;
                    c->zoomed = 0; d->resize_edges = 0;
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
