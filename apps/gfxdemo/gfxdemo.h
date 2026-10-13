/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_GFXDEMO_H
#define CIUKI_GFXDEMO_H
#include "../libdeskui/deskui.h"
#define GFX_W 320
#define GFX_H 240
#define GFX_FRAME_NS 16666667ull
enum { GFX_TABS=1, GFX_RUN, GFX_SHADE, GFX_PAUSE };
struct gfx_vertex { int32_t x,y,inv,u,v,light; }; /* screen Q4; 1/z Q16; uv/z */
struct gfx_scene { uint32_t pixels[GFX_W*GFX_H], depth[GFX_W*GFX_H]; };
struct gfx_result { uint64_t bytes,ns,operations; unsigned samples; int error; };
struct gfx_app {
    struct du_ui *ui;
    struct gfx_scene scene;
    const uint32_t *portrait;
    struct du_item tabs[3];
    unsigned page, gouraud, paused;
    uint64_t elapsed_ns,last_ns,next_ns,frames,sample_begin,fps10,ms1000;
    uint64_t desktop_composite_ns,desktop_present_ns;
    struct ciuki_accel_info accelerator;
    uint32_t display_w,display_h,bpp;
    int info_error,have_info,have_desktop,bench_running;
    unsigned bench_index;
    struct gfx_result results[6];
    int (*request)(void *, unsigned opcode, unsigned index);
    void *request_ctx;
};
void gfx_triangle(struct gfx_scene *, struct gfx_vertex, struct gfx_vertex, struct gfx_vertex,
                  const uint32_t *portrait, int gouraud, unsigned flat_light);
void gfx_cube(struct gfx_scene *, uint64_t elapsed_ns, const uint32_t *portrait, int gouraud);
void gfx_sprites(struct gfx_scene *, uint64_t elapsed_ns, const uint32_t *portrait);
void gfx_attach(struct gfx_app *,struct du_ui *,const uint32_t *portrait);
void gfx_advance(struct gfx_app *,uint64_t now);
void gfx_receive(struct gfx_app *,const struct desk_packet *);
void gfx_paint(struct du_ui *,void *);
#endif
