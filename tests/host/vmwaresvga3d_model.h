/* Independent SVGA3D (legacy VGPU9) host model: FIFO decoder, surface and
 * context tables, DMA, fixed-function rasterizer. Shared by the backend,
 * owner and GL H1 tests. GPL-2.0-only. */
#ifndef VMWARESVGA3D_MODEL_H
#define VMWARESVGA3D_MODEL_H
#include "../../src/kernel/drivers/video/vmwaresvga/vmwaresvga3d.h"
/* Host semantics the model reproduces (docs/design/f7-family-vmware-svga3d.md):
 * VMWARE: D3D9 texture-stage combiners, TEXTURE_OPS reported.
 * VBOX_GL: VirtualBox legacy OpenGL backend: COLOROP/ALPHAOP ignored
 *   (DevVGA-SVGA3d-ogl.cpp:5687-5698), GL MODULATE per bound unit,
 *   SVGA3D_DEVCAP_TEXTURE_OPS == 0 (:1526-1527).
 * VBOX_DX: VirtualBox VGPU10 backend: VGPU9 context/draw/clear entry points
 *   are stubs (DevVGA-SVGA3d-dx-dx11.cpp:5578-5760); DMA still works.
 * QEMU: no SVGA_CAP_3D (QEMU v9.2.0 hw/display/vmware_vga.c). */
enum svga_host { SVGA_HOST_VMWARE, SVGA_HOST_VBOX_GL, SVGA_HOST_VBOX_DX, SVGA_HOST_QEMU };
#define MODEL_SURFACES 64u
#define MODEL_FIFO_BYTES (64u * 1024u)
#define MODEL_VRAM_BYTES (32u * 1024u * 1024u)
#define MODEL_FB_PHYS 0xe0000000u
struct svga_model_surface {
    bool live;
    uint32_t format, flags, levels, w[11], h[11], bpp;
    uint8_t *data[11];
};
struct vmsvga3d_model {
    struct vmsvga3d_device device;
    struct vmsvga_engine engine;
    struct fb_device fb;
    enum svga_host host;
    uint32_t regs[64], fifo[MODEL_FIFO_BYTES / 4];
    uint8_t *vram;
    struct svga_model_surface surfaces[MODEL_SURFACES];
    bool context;
    uint32_t rs[100], ts[2][33], rt_color, rt_depth, viewport[4];
    float zrange[2], transforms[4][16];
    uint64_t clock;
    unsigned locks, max_live, delay_calls;
    uint64_t batches, commands, draws, fragments, dmas, defines, destroys, clears, copies;
    uint64_t fences, fifo_writes, memory_writes, register_writes, context_defines, context_destroys;
    uint64_t live_after_batch;
    uint32_t texture_ops, hwversion, max_textures, max_texture_size, caps_override;
    uint32_t dev_cap_index, dev_cap_reads;
    /* gb_objects: SVGA_CAP_GBOBJECTS host with an empty FIFO caps block and
     * devcaps only through SVGA_REG_DEV_CAP (Linux v6.1 vmwgfx_devcaps.c). */
    bool hang, wrong_fence, drop_draws, corrupt_guard, revoked, stuck_clock, bad_caps, gb_objects;
    bool fail_context_destroy;
};
extern struct vmsvga3d_model svga_model;
void svga_model_init(unsigned width, unsigned height, enum svga_host host);
/* Re-run engine init (registers/FIFO caps) after changing host knobs. */
int svga_model_engine(void);
void svga_model_destroy(void);
void svga_model_resources(struct render3d_service *, unsigned, unsigned, bool hardware, bool full);
void *svga_model_alloc(void *, size_t);
void svga_model_free(void *, void *);
uint64_t svga_model_now(void *);
unsigned svga_model_live(void);
#endif
