#ifndef CIUKI_SERVICES_H
#define CIUKI_SERVICES_H

#include <stdint.h>

typedef enum ciuki_com_exit_reason {
    CIUKI_COM_EXIT_RETURN   = 0,
    CIUKI_COM_EXIT_INT20    = 1,
    CIUKI_COM_EXIT_INT21_4C = 2,
    CIUKI_COM_EXIT_API      = 3
} ciuki_com_exit_reason_t;

typedef struct ciuki_int21_regs {
    uint16_t ax;
    uint16_t bx;
    uint16_t cx;
    uint16_t dx;
    uint16_t si;
    uint16_t di;
    uint16_t ds;
    uint16_t es;
    uint8_t carry;
    uint8_t reserved[3];
} ciuki_int21_regs_t;

typedef struct ciuki_dos_context {
    void *boot_info;
    void *handoff;

    uint16_t psp_segment;
    uint16_t reserved0;
    uint32_t reserved1;

    uint64_t psp_linear;
    uint64_t image_linear;
    uint32_t image_size;
    uint8_t command_tail_len;
    uint8_t exit_code;
    uint8_t exit_reason;
    uint8_t reserved2;
    char command_tail[128];
} ciuki_dos_context_t;

/*
 * ciuki_services_t — function table passed by stage2 to every loaded COM.
 * COM binaries must not call stage2 functions directly (no stable ABI);
 * they must only use these pointers.
 */
typedef struct ciuki_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
} ciuki_fb_info_t;

/*
 * ciuki_gfx_services_t — stable 2D graphics ABI exposed to COM programs.
 * All callbacks write into the stage2 backbuffer and clip to fb bounds.
 * Caller must wrap batches in begin_frame / end_frame for atomic commits.
 * Color format: 0x00RRGGBB.
 */
typedef struct ciuki_gfx_services {
    void (*begin_frame)(void);
    void (*end_frame)(void);
    void (*put_pixel)(uint32_t x, uint32_t y, uint32_t rgb);
    void (*fill_rect)(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
    void (*rect)(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
    void (*line)(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t rgb);
    void (*circle)(int32_t cx, int32_t cy, uint32_t r, uint32_t rgb);
    void (*fill_circle)(int32_t cx, int32_t cy, uint32_t r, uint32_t rgb);
    void (*fill_tri)(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     int32_t x2, int32_t y2, uint32_t rgb);
    void (*blit)(const uint32_t *src, uint32_t sw, uint32_t sh,
                 uint32_t stride, uint32_t dx, uint32_t dy);
    void (*get_fb_info)(ciuki_fb_info_t *out);
    /* M-V2.4 — BIOS INT 10h + mode switch. */
    uint8_t (*set_mode)(uint8_t mode);       /* 0x03 text, 0x13 320x200x8 */
    uint8_t (*get_mode)(void);
    int     (*present)(void);                /* commit current plane -> fb */
    /* M-V2.5 — palette (256 x 6-bit RGB triples, VGA-compatible). */
    void (*set_palette)(uint32_t first, uint32_t count,
                        const uint8_t *rgb_triples_6bit);
    /* Mode 0x13 plane fast path (for DOOM-style linear fb writes). */
    uint8_t *(*mode13_plane)(void);
    void (*mode13_put_pixel)(uint32_t x, uint32_t y, uint8_t color_index);
    /* INT 10h dispatcher (for DOS binaries that explicitly trap). */
    void (*int10)(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs);
    /* M-V2.6 — DOOM-prep palette fade + mode13 bulk fills. */
    void (*palette_fade)(uint32_t target_rgb, uint32_t step, uint32_t total);
    void (*mode13_fill)(uint8_t color_index);
    void (*mode13_fill_rect)(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint8_t color_index);
    /* M-V2.7 — DOS universality: blit + column-draw + palette readback. */
    void (*mode13_blit_indexed)(const uint8_t *src, uint32_t sw, uint32_t sh,
                                uint32_t stride, uint32_t dx, uint32_t dy,
                                uint8_t use_transparent,
                                uint8_t transparent_idx);
    void (*mode13_draw_column)(uint32_t x, uint32_t y, uint32_t h,
                               const uint8_t *src);
    void (*palette_get_raw)(uint32_t first, uint32_t count,
                            uint8_t *rgb_triples_6bit_out);
    uint8_t reserved[32];
} ciuki_gfx_services_t;

typedef struct ciuki_services {
    void     (*print)(const char *s);
    void     (*print_hex64)(unsigned long long v);
    void     (*cls)(void);
    void     (*int21)(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs);
    void     (*int2f)(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs);
    void     (*int31)(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs);
    void     (*int20)(ciuki_dos_context_t *ctx);
    void     (*int21_4c)(ciuki_dos_context_t *ctx, uint8_t code);
    void     (*terminate)(ciuki_dos_context_t *ctx, uint8_t code);
    const ciuki_gfx_services_t *gfx;    /* M-V2.3: 2D graphics ABI */
} ciuki_services_t;

/* COM entry point convention */
typedef void (*com_entry_t)(ciuki_dos_context_t *ctx, ciuki_services_t *svc);

#endif
