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
    nsfb_bbox_t damage;
    int mouse_x, mouse_y;
    unsigned buttons;
    unsigned keys[0x201];
    nsfb_event_t events[CIUKI_BROWSER_EVENTS];
    unsigned head, count;
};
int ciuki_browser_open(struct ciuki_browser_surface *,const struct ciuki_browser_ops *,int,int,int);
void ciuki_browser_close(struct ciuki_browser_surface *);
void ciuki_browser_damage(struct ciuki_browser_surface *,nsfb_bbox_t);
int ciuki_browser_present(struct ciuki_browser_surface *);
int ciuki_browser_packet(struct ciuki_browser_surface *,const struct desk_packet *);
bool ciuki_browser_event(struct ciuki_browser_surface *,nsfb_event_t *);
void ciuki_browser_input(struct ciuki_browser_surface *,const struct ciuki_input_event *);
void ciuki_browser_release(struct ciuki_browser_surface *);
/* Bound to the framebuffer frontend's sole UI thread. */
extern struct ciuki_browser_surface *ciuki_browser_active;
extern int ciuki_browser_channel;
void ciuki_chrome_init(struct ciuki_browser_surface *);
void ciuki_chrome_resize(void);
void ciuki_chrome_render(void);
bool ciuki_chrome_input(const struct ciuki_input_event *);
void ciuki_chrome_release(void);
#endif
