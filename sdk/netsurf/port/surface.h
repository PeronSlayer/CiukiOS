/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_NETSURF_SURFACE_H
#define CIUKI_NETSURF_SURFACE_H
#include <stdbool.h>
#include <libnsfb.h>
#include <libnsfb_event.h>
#include "apps/libdeskui/v3.h"
#define CIUKI_BROWSER_TOP 36
#define CIUKI_BROWSER_STATUS 22
#define CIUKI_BROWSER_EVENTS 544
/* f6-20: packets read from the desktop channel while the UI thread is busy
 * (long layout or first paint), kept unprocessed until the next UI turn.
 * The desktop drops a v3 client whose 64-message queue fills; consecutive
 * pointer motion is coalesced, so this ring rarely holds more than input
 * edges, pings and replies. Each UI turn drains at most CIUKI_BROWSER_DRAIN. */
#define CIUKI_BROWSER_HELD 64
#define CIUKI_BROWSER_DRAIN 256
/* Every desktop launch maps the window channel at fd 3 (apps/desktop launch_program,
 * the SDK convention of sdk/samples/HelloWindow); --channel-fd=N overrides it. */
#define CIUKI_BROWSER_CHANNEL_FD 3
struct ciuki_browser_storage {
    int fd;
    struct ciuki_surface_info info;
    uint32_t *map, *back;
};
struct ciuki_browser_ops {
    struct du3_transport transport;
    int (*create)(uint32_t,uint32_t,uint32_t);
    void *(*map)(int,int);
    int (*unmap)(void *,size_t);
};
struct ciuki_browser_surface {
    struct du3_connection connection;
    struct ciuki_browser_ops ops;
    struct ciuki_browser_storage current, pending;
    uint32_t window, generation, create_request, swap_request, ping;
    uint32_t configure, last_configure, want_width, want_height;
    bool ready, closed, dirty, resized;
    /* f6-20: the first reason the surface closed (static string) and its
     * errno/result detail; event-queue overflows resynchronise, never close. */
    const char *close_reason;
    int close_detail;
    unsigned overflows;
    nsfb_bbox_t damage;
    int mouse_x, mouse_y;
    unsigned buttons;
    unsigned cursor_shape;
    bool cursor_valid;
    unsigned keys[0x201];
    nsfb_event_t events[CIUKI_BROWSER_EVENTS];
    unsigned head, count;
    struct desk_packet held[CIUKI_BROWSER_HELD];
    unsigned held_head, held_count, pumps;
    bool in_input;
};
int ciuki_browser_open(struct ciuki_browser_surface *,const struct ciuki_browser_ops *,int,int,int);
void ciuki_browser_close(struct ciuki_browser_surface *);
void ciuki_browser_damage(struct ciuki_browser_surface *,nsfb_bbox_t);
int ciuki_browser_present(struct ciuki_browser_surface *);
int ciuki_browser_cursor(struct ciuki_browser_surface *,unsigned shape);
int ciuki_browser_packet(struct ciuki_browser_surface *,const struct desk_packet *);
bool ciuki_browser_event(struct ciuki_browser_surface *,nsfb_event_t *);
void ciuki_browser_input(struct ciuki_browser_surface *,const struct ciuki_input_event *);
void ciuki_browser_release(struct ciuki_browser_surface *);
void ciuki_browser_mark_closed(struct ciuki_browser_surface *,const char *reason,int detail);
/* Hold a packet for the next UI turn (false: ring full); take it back in order. */
bool ciuki_browser_hold(struct ciuki_browser_surface *,const struct desk_packet *);
bool ciuki_browser_unhold(struct ciuki_browser_surface *,struct desk_packet *);
/* Bound to the framebuffer frontend's sole UI thread. */
extern struct ciuki_browser_surface *ciuki_browser_active;
extern int ciuki_browser_channel;   /* --channel-fd=N, or -1: use CIUKI_BROWSER_CHANNEL_FD */
void ciuki_chrome_init(struct ciuki_browser_surface *);
void ciuki_chrome_resize(void);
void ciuki_chrome_render(void);
bool ciuki_chrome_input(const struct ciuki_input_event *);
void ciuki_chrome_release(void);
#endif
