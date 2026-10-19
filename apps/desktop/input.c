/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include <string.h>
#include <errno.h>
#include <stdio.h>
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include <unistd.h>
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
/* Focus changes tint the caption and thin frame edges, never the client well. */
static void focus_damage(struct desktop *d, int i)
{
    struct desk_box f=d->clients[i].frame;
    struct desk_box parts[4]={{f.x,f.y,f.w,DESK_TITLE},
        {f.x,f.y+DESK_TITLE,DESK_BORDER,f.h-DESK_TITLE},
        {f.x+f.w-DESK_BORDER,f.y+DESK_TITLE,DESK_BORDER,f.h-DESK_TITLE},
        {f.x,f.y+f.h-DESK_BORDER,f.w,DESK_BORDER}};
    for (unsigned k=0;k<4;k++) desk_damage_add(&d->damage,parts[k],d->width,d->height);
    if (d->shell) desk_damage_add(&d->damage,desk_task_button(d,i),d->width,d->height);
}
int desk_modal_target(const struct desktop *d, int i)
{
    for (unsigned depth=0; i>=0 && i<DESK_CLIENTS && depth<DESK_CLIENTS; depth++) {
        int next=-1;
        for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].window && !d->clients[k].minimized &&
            (d->clients[k].version!=DESK_V3_VERSION || (d->clients[k].v3_style&DESK_V3_MODAL)) &&
            d->clients[k].owner_id && d->clients[k].owner_id==d->clients[i].window_id &&
            (next<0 || d->clients[k].order>d->clients[next].order)) next=k;
        if (next<0) break;
        i=next;
    }
    return i;
}
int desk_next_window(const struct desktop *d, int excluded, uint32_t pid, const char *application)
{
    int next=-1;
    for (int i=0;i<DESK_CLIENTS;i++) {
        const struct desk_client *c=&d->clients[i];
        if (i==excluded || !c->window || c->minimized) continue;
        int same=(pid && c->pid==pid) || (application && application[0] && !strcmp(application,c->application));
        int previous_same=next>=0 && ((pid && d->clients[next].pid==pid) ||
            (application && application[0] && !strcmp(application,d->clients[next].application)));
        if (next<0 || (same && !previous_same) || (same==previous_same && c->order>d->clients[next].order)) next=i;
    }
    return next;
}
void desk_focus(struct desktop *d, int i)
{
    i=desk_modal_target(d,i);
    if (d->power.state && i>=0 && i<DESK_CLIENTS && d->clients[i].internal!=DESK_INTERNAL_POWER) return;
    if (i >= 0 && (i >= DESK_CLIENTS || !d->clients[i].window || d->clients[i].minimized)) return;
    if (i == d->focus) return;
    if (d->v3_lease_state) desk_v3_revoke(d);
    desk_v3_pointer_release(d);
    struct ciuki_input_event e = { .monotonic_ns = d->now_ns };
    release(d, &e);
    if (d->keyboard_geometry) desk_geometry_end(d,1);
    d->drag=-1; d->resize_edges=0;
    struct ciuki_message m;
    desk_message(&m, DESK_FOCUS, 8);
    if (d->focus >= 0) {
        if (d->clients[d->focus].version==DESK_V3_VERSION) {
            struct desk_client *old=&d->clients[d->focus]; struct ciuki_message active;
            desk_v3_message(&active,DESK_V3_ACTIVATE,8,0,old->v3_id,old->v3_generation);
            desk_put32(active.data+20,i>=0 && d->clients[i].pid==old->pid);
            desk_queue(d,d->focus,&active);
        } else desk_queue(d, d->focus, &m);
        focus_damage(d,d->focus);
    }
    d->focus = i;
    /* Publish the new lease generation before activation can dispatch a
     * WM_ACTIVATEAPP handler that immediately calls DirectDraw Restore. */
    desk_v3_reactivate(d,i);
    if (i >= 0) {
        int raised=0;
        for (int k=0;k<DESK_CLIENTS;k++) if (k!=i && d->clients[k].window && !d->clients[k].minimized &&
            d->clients[k].order>d->clients[i].order) raised=1;
        d->clients[i].order = ++d->order;
        desk_put32(m.data + 4, 1); desk_queue(d, i, &m);
        if (raised) desk_damage_add(&d->damage,desk_outer(&d->clients[i]),d->width,d->height);
        focus_damage(d,i);
    }
    desk_v3_restack(d);
}
int desk_contains(struct desk_box b, int32_t x, int32_t y)
{ return x >= b.x && y >= b.y && (int64_t)x < (int64_t)b.x+b.w && (int64_t)y < (int64_t)b.y+b.h; }
int desk_hit(const struct desktop *d, int32_t x, int32_t y)
{
    if (d->shell && desk_start_contains(d,x,y)) return -1;
    int best = -1;
    for (int i = 0; i < DESK_CLIENTS; i++) if (d->clients[i].window && !d->clients[i].minimized &&
        (desk_contains(d->clients[i].frame, x, y) || desk_resize_hit(&d->clients[i], x, y)) &&
        (best < 0 || d->clients[i].order > d->clients[best].order)) best = i;
    return best;
}
unsigned desk_resize_hit(const struct desk_client *c, int32_t x, int32_t y)
{
    if (!c->resizable || c->internal || c->collapsed || c->zoomed) return 0;
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
void desk_cursor_update(struct desktop *d)
{
    unsigned shape=DESK_CURSOR_ARROW, edges=0;
    if (d->power.state) {
        /* The power dialog owns pointer feedback while it blocks clients. */
    } else if (d->drag>=0) {
        shape=DESK_CURSOR_MOVE; edges=d->resize_edges;
    } else if (d->routed_buttons && d->focus>=0) {
        shape=d->clients[d->focus].cursor;
    } else if (!d->sh.levels && !d->sh.switcher && !d->sh.volume_open && !d->sh.network_open &&
               !d->sh.media_open && !d->sh.notification.active) {
        int i=desk_hit(d,d->mouse_x,d->mouse_y);
        if (i>=0 && d->mouse_y>=DESK_BAR && (!d->shell || d->mouse_y<(int)d->height-DESK_BAR)) {
            struct desk_client *c=&d->clients[i];
            if (desk_modal_target(d,i)!=i) shape=DESK_CURSOR_FORBIDDEN;
            else if (c->unresponsive || c->closing) shape=DESK_CURSOR_BUSY;
            else {
                edges=desk_title_code(d,d->mouse_x,d->mouse_y) ? 0 : desk_resize_hit(c,d->mouse_x,d->mouse_y);
                if (!edges && desk_contains(desk_content(c),d->mouse_x,d->mouse_y)) {
                    shape=c->cursor;
                }
            }
        }
        if (shape==DESK_CURSOR_ARROW && !edges && d->shell) shape=desk_shell_cursor(d);
        if (shape==DESK_CURSOR_ARROW && !edges) for (int k=0;k<DESK_CLIENTS;k++) {
            const struct desk_client *c=&d->clients[k];
            if (c->awaiting_frame && d->now_ns>=c->launched_ns && d->now_ns-c->launched_ns<5000000000ull) {
                shape=DESK_CURSOR_ARROW_BUSY; break;
            }
        }
    }
    if (edges) {
        unsigned horizontal=edges&(DESK_EDGE_LEFT|DESK_EDGE_RIGHT), vertical=edges&(DESK_EDGE_TOP|DESK_EDGE_BOTTOM);
        if (!vertical) shape=DESK_CURSOR_RESIZE_EW;
        else if (!horizontal) shape=DESK_CURSOR_RESIZE_NS;
        else shape=edges==(DESK_EDGE_LEFT|DESK_EDGE_TOP) || edges==(DESK_EDGE_RIGHT|DESK_EDGE_BOTTOM) ?
            DESK_CURSOR_RESIZE_NWSE : DESK_CURSOR_RESIZE_NESW;
    }
    unsigned old_custom=d->v3_cursor_id, old_slot=d->v3_cursor_slot, old_hidden=d->v3_cursor_hidden;
    d->v3_cursor_id=0; d->v3_cursor_hidden=0;
    int vi=d->v3_pointer_window ? d->v3_pointer_window-1 : desk_hit(d,d->mouse_x,d->mouse_y);
    if (vi>=0 && !d->sh.levels && !d->sh.switcher && !d->power.state && d->drag<0) {
        struct desk_client *vc=&d->clients[vi];
        if (vc->version==DESK_V3_VERSION && (d->v3_pointer_window || desk_contains(desk_content(vc),d->mouse_x,d->mouse_y))) {
            d->v3_cursor_id=vc->v3_cursor; d->v3_cursor_slot=(unsigned)vi;
            d->v3_cursor_hidden=vc->v3_visibility<0 || (vc->v3_pointer&DESK_V3_HIDDEN);
        }
    }
    if (old_custom!=d->v3_cursor_id || old_slot!=d->v3_cursor_slot || old_hidden!=d->v3_cursor_hidden) d->cursor_dirty=1;
    if (shape>=DESK_CURSOR_COUNT) shape=DESK_CURSOR_ARROW;
    if (d->cursor_shape!=shape) { d->cursor_shape=shape; d->cursor_dirty=1; }
}
void desk_geometry_begin(struct desktop *d, int i, int size)
{
    if (i<0 || i>=DESK_CLIENTS || !d->clients[i].window || d->clients[i].minimized ||
        d->clients[i].zoomed || desk_modal_target(d,i)!=i || (size && !d->clients[i].resizable)) return;
    desk_focus(d,i);
    struct ciuki_input_event e={.monotonic_ns=d->now_ns};
    release(d,&e);
    d->drag=i; d->keyboard_geometry=size ? 2 : 1;
    d->resize_edges=size ? DESK_EDGE_RIGHT|DESK_EDGE_BOTTOM : 0;
    d->resize_start=d->clients[i].frame;
    d->geometry_zoomed=d->clients[i].zoomed; d->geometry_restore=d->clients[i].restore;
    desk_cursor_update(d);
}
void desk_geometry_end(struct desktop *d, int cancel)
{
    int i=d->drag;
    if (i>=0 && cancel && d->clients[i].window) {
        int size=d->keyboard_geometry==2 || d->resize_edges;
        d->resize_edges=0;
        if (size) desk_resize(d,i,d->resize_start);
        else desk_move(d,i,d->resize_start.x,d->resize_start.y);
        d->clients[i].zoomed=d->geometry_zoomed; d->clients[i].restore=d->geometry_restore;
    }
    d->keyboard_geometry=0; d->drag=-1; d->resize_edges=0;
    desk_cursor_update(d);
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
static void input(struct desktop *d, const struct ciuki_input_event *in)
{
    /* Preserve the timestamp of a deferred menu action across the remaining
     * input batch. Immediate icon/menu actions consume it in launch_program. */
    if (d->action < 0) d->launch_click_ns = in->monotonic_ns ? in->monotonic_ns : d->now_ns;
    uint64_t stamp=in->monotonic_ns ? in->monotonic_ns : d->now_ns;
    if (stamp && stamp<=d->now_ns && (!d->frame_input_ns || stamp<d->frame_input_ns))
        d->frame_input_ns=stamp;
    d->inputs++;
    if (d->input_pending) d->input_pending();
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
        desk_v3_pointer_release(d);
        /* The input stream was cut: keys are released below, and the screen may
         * have been owned by someone else meanwhile (a DOS full-screen session
         * restores the desktop mode and sends exactly this event). Since f2-65
         * the desktop presents only on damage, so repaint everything: the
         * display waits for a full frame after a restore before it allows the
         * next mode change (display_fullscreen_begin: awaiting_frame). */
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
        d->cursor_dirty = 1;
        /* With a 2D engine the compositor presents damage as row spans and
         * engine copies while the scanout is trusted; the kernel's full-frame
         * check needs one whole-screen present, so distrust the scanout. */
        d->scanout_valid = 0;
        if (d->keyboard_geometry) desk_geometry_end(d,1);
        release(d, &e); d->drag = -1; d->resize_edges=0;
        if (d->shell) {
            const int modifiers[]={0x38,0x138,0x1D,0x11D,0x2A,0x36};
            for (unsigned k=0;k<sizeof(modifiers)/sizeof(modifiers[0]);k++) desk_shell_key(d,modifiers[k],0);
            d->sh.logo_down=0;
        }
        d->buttons = (uint32_t)e.value & 7u;
        route(d, d->focus, DESK_KEY, &e);
        break;
    case CIUKI_INPUT_KEY:
        if (e.code < 0 || e.code > 0x200 || e.value < 0 || e.value > 2) break;
        if (d->drag>=0 && !d->keyboard_geometry && e.code==0x01 && e.value) { desk_geometry_end(d,1); break; }
        if (d->keyboard_geometry) {
            if (e.code==0x38 || e.code==0x138 || e.code==0x1d || e.code==0x11d || e.code==0x2a || e.code==0x36) {
                if ((e.code==0x38 || e.code==0x138) && !e.value) { d->sh.alt_used=1; d->sh.alt_routed=0; }
                if (d->shell) desk_shell_key(d,e.code,e.value);
                break;
            }
            if (!e.value) break;
            if (e.code==0x01 || e.code==0x1c) { desk_geometry_end(d,e.code==0x01); break; }
            int dx=e.code==0x14d ? 8 : e.code==0x14b ? -8 : 0;
            int dy=e.code==0x150 ? 8 : e.code==0x148 ? -8 : 0;
            struct desk_box f=d->clients[d->drag].frame;
            if (d->keyboard_geometry==2) {
                struct desk_client *c=&d->clients[d->drag];
                if (dx && c->step_width>8) dx=dx>0 ? (int)c->step_width : -(int)c->step_width;
                if (dy && c->step_height>8) dy=dy>0 ? (int)c->step_height : -(int)c->step_height;
                f.w+=dx; f.h+=dy; desk_resize(d,d->drag,f);
            }
            else desk_move(d,d->drag,f.x+dx,f.y+dy);
            break;
        }
        /* Shell shortcuts and menus (shell.c): Win, Ctrl+Esc, Alt+Tab, Alt+F4, menu keys, built-in windows. */
        if (d->shell && desk_shell_key(d, e.code, e.value)) break;
        d->keys[e.code] = (uint8_t)(e.value != 0 && d->focus >= 0);
        route(d, d->focus, DESK_KEY, &e);
        break;
    case CIUKI_INPUT_TEXT: route(d, d->focus, DESK_KEY, &e); break;
    case CIUKI_INPUT_WHEEL:
        route(d, d->focus, DESK_MOTION, &e);
        if (e.source==3 && e.value && d->usb_wheel_records<16) {
            char record[64];
            int n=snprintf(record,sizeof(record),"[desktop] usb wheel value=%d\n",e.value);
            d->usb_wheel_records++;
            /* One console publication, including on ordinary boots. No
             * stdio buffering/flush can hold back the consumption record. */
            if (n>0 && n<(int)sizeof(record)) (void)write(STDERR_FILENO,record,(size_t)n);
        }
        break;
    case CIUKI_INPUT_MOTION:
        if (d->v3_pointer_window) {
            int vi=d->v3_pointer_window-1;
            struct desk_client *vc=&d->clients[vi];
            if ((vc->v3_pointer&DESK_V3_RELATIVE) && d->focus>=0 && d->clients[d->focus].pid==vc->pid)
                route(d,vi,DESK_V3_RELATIVE_EVENT,&e);
        }
        d->cursor_dirty=1;
        {
            int32_t dx = e.value, dy = e.value2;
            desk_shell_scale_motion(d, &dx, &dy);   /* Control Panel mouse speed; 1:1 by default */
            d->mouse_x = axis(d->mouse_x, dx, d->width);
            d->mouse_y = axis(d->mouse_y, dy, d->height);
        }
        if (d->v3_pointer_window) {
            struct desk_client *vc=&d->clients[d->v3_pointer_window-1];
            if (vc->v3_pointer&DESK_V3_CLIP) {
                struct desk_box b=vc->v3_clip;
                if (d->mouse_x<b.x) d->mouse_x=b.x;
                if (d->mouse_x>=b.x+b.w) d->mouse_x=b.x+b.w-1;
                if (d->mouse_y<b.y) d->mouse_y=b.y;
                if (d->mouse_y>=b.y+b.h) d->mouse_y=b.y+b.h-1;
            }
        }
        if (d->shell) desk_shell_motion(d);
        if (d->drag >= 0 && !d->keyboard_geometry) {
            if (d->resize_edges) {
                struct desk_box f = d->resize_start;
                int dx = d->mouse_x-d->drag_x, dy = d->mouse_y-d->drag_y;
                if (d->resize_edges & DESK_EDGE_LEFT) { f.x += dx; f.w -= dx; }
                if (d->resize_edges & DESK_EDGE_RIGHT) f.w += dx;
                if (d->resize_edges & DESK_EDGE_TOP) { f.y += dy; f.h -= dy; }
                if (d->resize_edges & DESK_EDGE_BOTTOM) f.h += dy;
                desk_resize(d, d->drag, f);
            } else desk_move(d, d->drag, d->mouse_x - d->drag_x, d->mouse_y - d->drag_y);
        } else title_track(d);
        int target=d->v3_pointer_window && (d->clients[d->v3_pointer_window-1].v3_pointer&DESK_V3_CAPTURE) ?
            d->v3_pointer_window-1 : d->routed_buttons ? d->focus : desk_hit(d,d->mouse_x,d->mouse_y);
        if (d->drag < 0 && target>=0 && desk_modal_target(d,target)==target) {
            struct desk_box content = desk_content(&d->clients[target]);
            /* MOTION carries client-relative absolute position in value/value2;
             * original delta is intentionally replaced at the protocol boundary. */
            unsigned scale=d->clients[target].v3_scale ? d->clients[target].v3_scale : 1;
            e.code = 0; e.value = (d->mouse_x-content.x)/(int)scale; e.value2 = (d->mouse_y-content.y)/(int)scale;
            route(d, target, DESK_MOTION, &e);
        }
        break;
    case CIUKI_INPUT_BUTTON: {
        if (e.code < 1 || e.code > 3 || (e.value != 0 && e.value != 1)) break;
        uint32_t mask = 1u << (e.code-1);
        if (e.value) d->buttons |= mask; else d->buttons &= ~mask;
        if (!e.value && e.code == CIUKI_BUTTON_LEFT) title_press(d, 0);
        if (!e.value && d->drag >= 0 && !d->keyboard_geometry && e.code == CIUKI_BUTTON_LEFT) {
            desk_geometry_end(d,0); break;
        }
        if (e.value && d->keyboard_geometry && e.code==CIUKI_BUTTON_LEFT) { desk_geometry_end(d,0); break; }
        int i = desk_hit(d,d->mouse_x,d->mouse_y);
        if (d->v3_pointer_window && (d->clients[d->v3_pointer_window-1].v3_pointer&DESK_V3_CAPTURE)) {
            int vi=d->v3_pointer_window-1;
            e.value2=0; route(d,vi,DESK_BUTTON,&e); break;
        }
        /* Start menu, taskbar, tray, desktop icons and context menus (shell.c). */
        if (d->shell && desk_shell_button(d, e.code, e.value)) break;
        if (e.value && i>=0 && desk_modal_target(d,i)!=i) { desk_focus(d,i); break; }
        if (e.value && e.code!=CIUKI_BUTTON_LEFT && i>=0) desk_focus(d,i);
        if (e.value && d->shell && i>=0 && e.code==CIUKI_BUTTON_RIGHT &&
            d->mouse_y<d->clients[i].frame.y+DESK_TITLE) {
            d->sh.menu_owner=i; desk_menu_open(d,SH_MENU_TASK,d->mouse_x,d->mouse_y); break;
        }
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
                    d->geometry_zoomed=c->zoomed; d->geometry_restore=c->restore;
                    c->zoomed = 0; d->drag = i; d->resize_edges = edges;
                    d->resize_start = c->frame; d->drag_x = d->mouse_x; d->drag_y = d->mouse_y;
                    d->title_click_window = -1; break;
                }
                if ((c->v3_style&3)!=DESK_V3_POPUP && d->mouse_y < c->frame.y + DESK_TITLE) {
                    /* A stationary second title click toggles maximize. */
                    int32_t dx = d->mouse_x - d->title_click_x, dy = d->mouse_y - d->title_click_y;
                    uint64_t interval=(uint64_t)d->sh.cfg.double_click_ms*1000000ull;
                    if (d->shell && stamp && d->title_click_ns && d->title_click_window == i && stamp >= d->title_click_ns &&
                        stamp - d->title_click_ns <= interval && dx <= 4 && dx >= -4 && dy <= 4 && dy >= -4) {
                        d->title_click_window = -1; desk_zoom(d,i); break;
                    }
                    d->title_click_window = i; d->title_click_ns = stamp;
                    d->title_click_x = d->mouse_x; d->title_click_y = d->mouse_y;
                    if (c->zoomed) break;
                    d->resize_edges = 0;
                    d->resize_start=c->frame; d->geometry_zoomed=c->zoomed; d->geometry_restore=c->restore;
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
            if (e.value) {
                /* Activation can change the client after the last motion. */
                struct desk_box b=desk_content(&d->clients[i]);
                struct ciuki_input_event motion=e;
                motion.type=CIUKI_INPUT_MOTION; motion.code=0; motion.value=d->mouse_x-b.x; motion.value2=d->mouse_y-b.y;
                route(d,i,DESK_MOTION,&motion);
                d->routed_buttons |= mask;
            } else d->routed_buttons &= ~mask;
            e.value2 = 0; route(d, d->focus, DESK_BUTTON, &e);
        }
        break;
    }
    default: break;
    }
}
void desk_input(struct desktop *d, const struct ciuki_input_event *in)
{ input(d,in); desk_cursor_update(d); }
