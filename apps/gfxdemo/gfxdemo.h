/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_GFXDEMO_H
#define CIUKI_GFXDEMO_H
#include "../libdeskui/deskui.h"
#include <ciuki/gl.h>
#define GFX_W 320
#define GFX_H 240
#define GFX_MAX_W 640
#define GFX_MAX_H 480
/* f7-14: full-screen targets follow the native GL bind limit. */
#define GFX_FULL_MAX_W 1600
#define GFX_FULL_MAX_H 1200
#define GFX_FRAME_NS 16666667ull
enum { GFX_TABS=1, GFX_RUN, GFX_SHADE, GFX_PAUSE, GFX_BACKEND, GFX_NATIVE, GFX_FULLSCREEN };
/* Windowed scenes render into pixels. A full-screen scene renders into the
 * packed caller target (width x height) and, with a lease, presents through
 * ciuki_gl_present instead of finishing into RAM; presented holds its result
 * (1 GPU copy, 0 CPU copy, negative errno). */
struct gfx_scene {
    uint32_t pixels[GFX_MAX_W*GFX_MAX_H]; unsigned width,height; int error;
    uint32_t *target; const struct ciuki_gl_lease *lease; int presented;
};
/* Full-screen state of the common present lease (DESK_LEASE). */
enum { GFX_WINDOWED, GFX_FULL_REQUESTED, GFX_FULL_ACTIVE };
struct gfx_result { uint64_t bytes,ns,operations; unsigned samples; int error; };
struct gfx_app {
    struct du_ui *ui;
    struct gfx_scene scene;
    const uint32_t *portrait;
    struct du_item tabs[3];
    unsigned page, gouraud, paused;
    int deferred, animation_due, scene_ready;
    struct du_rect viewport, scene_damage;
    uint64_t elapsed_ns,last_ns,next_ns,frames,sample_begin,fps10,ms1000;
    uint64_t desktop_composite_ns,desktop_present_ns;
    uint64_t render_ns,submit_ns,wait_ns,convert_ns,copy_ns,slot_ns,refresh_ns;
    uint64_t scene_render_ns;
    uint64_t total_frames;
    unsigned stats_page,stats_paused;
    int native_resolution;
    uint64_t (*time_ns)(void); /* runtime clock; deterministic previews need none */
    int have_timing;
    struct ciuki_accel_info accelerator;
    struct ciuki_display_engine_info engine;
    uint32_t display_w,display_h,bpp;
    int info_error,have_info,have_desktop,bench_running;
    unsigned bench_index;
    struct gfx_result results[6];
    int (*request)(void *, unsigned opcode, unsigned index);
    void *request_ctx;
    /* f7-14: backend policy (CIUKI_GL_BACKEND_*), its last result, the
     * engine's completion accounting after the latest frame, and the
     * full-screen state (GFX_WINDOWED...). */
    unsigned backend, full_width, full_height;
    int backend_error, full_state, full_error;
    struct ciuki_gl_status gl;
};
void gfx_set_backend(struct gfx_app *,unsigned policy);
void gfx_invalidate_status(struct gfx_app *);
void gfx_gl_status(struct gfx_app *);
const char *gfx_backend_name(unsigned policy);
const char *gfx_present_name(const struct gfx_app *);
int gfx_status_line(const struct gfx_app *,char *out,size_t cap);
void gfx_cube(struct gfx_scene *, uint64_t elapsed_ns, const uint32_t *portrait, int gouraud);
void gfx_sprites(struct gfx_scene *, uint64_t elapsed_ns, const uint32_t *portrait);
void gfx_attach(struct gfx_app *,struct du_ui *,const uint32_t *portrait);
void gfx_advance(struct gfx_app *,uint64_t now);
void gfx_presented(struct gfx_app *,uint64_t now);
static inline uint64_t gfx_wait_ns(uint64_t now,uint64_t slot)
{
    if (slot>now && slot-now<1000000ull) return 0;
    return 1000000ull;
}
void gfx_update_scene(struct gfx_app *);
void gfx_prepare_scene(struct gfx_app *,const uint32_t *front,unsigned stride);
void gfx_diff_scene(struct gfx_app *,const uint32_t *front,unsigned stride);
void gfx_submit_scene(struct gfx_app *,uint32_t *front,unsigned stride);
void gfx_store_scene(struct gfx_app *);
void gfx_receive(struct gfx_app *,const struct desk_packet *);
void gfx_paint(struct du_ui *,void *);
#endif
