/*
 * CIUKDEMO.COM — CiukiOS real-time graphics showcase (OT-DEMO-001).
 *
 * A dedicated ~30 second deterministic animated scene designed for short
 * video capture of the current CiukiOS state. Uses mode 0x13 (320x200x8)
 * and the gfx services ABI already in production (fadedmo / dosmode13 /
 * gfxdoom). No external assets. Five phases drive the run:
 *
 *   1. Title card        — "CIUKIOS" bitmap logo with sweeping highlight
 *   2. Plasma field      — XOR plasma shifted each frame (color cycling)
 *   3. Orbiting blocks   — 4 palette-indexed squares orbiting screen center
 *   4. Concentric rings  — distance-field rings expanding outward
 *   5. Fade to black     — palette_fade commits phase-out
 *
 * Each phase emits a deterministic serial marker (`[ciukdemo] phase N ...`)
 * so a gate or capture script can prove the main animation path was reached.
 * A final `[ciukdemo] OK` is emitted before clean INT 21h AH=4Ch return.
 *
 * The program is intentionally freestanding: no WAD parsing, no audio, no
 * extender logic. It just showcases the stage2 gfx/mode13 path.
 */

#include "services.h"

#define W 320U
#define H 200U

#define PHASE1_FRAMES 40U   /* title              */
#define PHASE2_FRAMES 60U   /* plasma             */
#define PHASE3_FRAMES 50U   /* orbits             */
#define PHASE4_FRAMES 50U   /* rings              */
#define PHASE5_FRAMES 24U   /* fade to black      */

/*
 * Per-frame pacing. The gfx->present() call already does a full plane blit
 * + upscale, which on the UEFI GOP path takes non-trivial wall-clock time
 * (same pattern used by fadedmo.COM). A small busy_delay keeps the demo
 * visually legible when captured off-screen without being painful in QEMU.
 */
#define FRAME_BUSY_DELAY 1500000U

/*
 * 5x7 bitmap font for "CIUKIOS". Each row is a 5-bit mask, bit 0 = leftmost
 * column, bit 4 = rightmost column.
 */
typedef struct glyph {
    unsigned char row[7];
} glyph_t;

static const glyph_t G_C = {{0x0F, 0x01, 0x01, 0x01, 0x01, 0x01, 0x0F}};
static const glyph_t G_I = {{0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}};
static const glyph_t G_U = {{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}};
static const glyph_t G_K = {{0x11, 0x09, 0x05, 0x03, 0x05, 0x09, 0x11}};
static const glyph_t G_O = {{0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}};
static const glyph_t G_S = {{0x1E, 0x01, 0x01, 0x0E, 0x10, 0x10, 0x0F}};

/*
 * Signed sine table. sin_tab[i] ~= sin(2*pi*i/64) * 64, amplitude [-64,64].
 * Used for orbit placement and ring animation. 64-entry period.
 */
static const signed char sin_tab[64] = {
      0,   6,  12,  18,  24,  30,  36,  41,
     45,  49,  53,  56,  58,  60,  62,  63,
     64,  63,  62,  60,  58,  56,  53,  49,
     45,  41,  36,  30,  24,  18,  12,   6,
      0,  -6, -12, -18, -24, -30, -36, -41,
    -45, -49, -53, -56, -58, -60, -62, -63,
    -64, -63, -62, -60, -58, -56, -53, -49,
    -45, -41, -36, -30, -24, -18, -12,  -6
};

static int isin(unsigned i) { return (int)sin_tab[i & 63U]; }
static int icos(unsigned i) { return (int)sin_tab[(i + 16U) & 63U]; }

static void print_line(ciuki_services_t *svc, const char *s) {
    svc->print(s);
}

static void busy_delay(unsigned cycles) {
    for (volatile unsigned i = 0; i < cycles; i++) { }
}

/*
 * Draw one glyph scaled by `scale` at logical (px, py) in the mode13 plane.
 * Uses gfx->mode13_fill_rect to get a single fast pixel block per lit cell.
 */
static void draw_glyph(const ciuki_gfx_services_t *gfx, const glyph_t *g,
                       int px, int py, int scale, unsigned char color) {
    for (int r = 0; r < 7; r++) {
        unsigned char row = g->row[r];
        for (int c = 0; c < 5; c++) {
            if (row & (unsigned char)(1U << c)) {
                int x = px + c * scale;
                int y = py + r * scale;
                if (x < 0 || y < 0) continue;
                if ((unsigned)x >= W || (unsigned)y >= H) continue;
                gfx->mode13_fill_rect((unsigned)x, (unsigned)y,
                                      (unsigned)scale, (unsigned)scale,
                                      color);
            }
        }
    }
}

static void draw_title_string(const ciuki_gfx_services_t *gfx,
                              int scale, unsigned char color) {
    /* "CIUKIOS" — 7 glyphs × (5*scale + gap). */
    static const glyph_t *const word[7] = {
        &G_C, &G_I, &G_U, &G_K, &G_I, &G_O, &G_S
    };
    int glyph_w = 5 * scale;
    int gap = scale;
    int step = glyph_w + gap;
    int total_w = 7 * glyph_w + 6 * gap;
    int px = ((int)W - total_w) / 2;
    int py = ((int)H - 7 * scale) / 2 - 10;
    for (int i = 0; i < 7; i++) {
        draw_glyph(gfx, word[i], px + i * step, py, scale, color);
    }
}

/*
 * Phase 1 — title card. Dark blue background, centered CIUKIOS logo,
 * horizontal highlight bar sweeping across the screen.
 */
static void phase_title(ciuki_services_t *svc,
                        const ciuki_gfx_services_t *gfx) {
    print_line(svc, "[ciukdemo] phase 1 title\n");
    for (unsigned f = 0; f < PHASE1_FRAMES; f++) {
        /* Background. */
        gfx->mode13_fill(1U);
        /* Sweep bar — a wide horizontal band moving top-to-bottom and
         * bottom-to-top in a triangle wave. */
        int sweep_y = (int)((f * (H - 20U)) / PHASE1_FRAMES);
        gfx->mode13_fill_rect(0U, (unsigned)sweep_y, W, 20U, 63U);
        /* Draw logo. Scale 4 -> 20x28 per glyph. */
        draw_title_string(gfx, 4, 15U);
        gfx->present();
        busy_delay(FRAME_BUSY_DELAY);
    }
    print_line(svc, "[ciukdemo] phase 1 done\n");
}

/*
 * Phase 2 — XOR plasma. Deterministic per-frame shift. Writes are done via
 * the raw mode13 plane pointer for speed; palette is the default VGA one
 * (colorful across low indices).
 */
static void phase_plasma(ciuki_services_t *svc,
                         const ciuki_gfx_services_t *gfx) {
    print_line(svc, "[ciukdemo] phase 2 plasma\n");
    unsigned char *plane = gfx->mode13_plane();
    if (!plane) {
        print_line(svc, "[ciukdemo] phase 2 skipped (no plane)\n");
        return;
    }
    for (unsigned f = 0; f < PHASE2_FRAMES; f++) {
        unsigned off = f * 3U;
        for (unsigned y = 0; y < H; y++) {
            unsigned row = y * W;
            unsigned yv = (y + off) & 0xFFU;
            for (unsigned x = 0; x < W; x++) {
                unsigned xv = (x + off) & 0xFFU;
                plane[row + x] = (unsigned char)(xv ^ yv);
            }
        }
        gfx->present();
        busy_delay(FRAME_BUSY_DELAY);
    }
    print_line(svc, "[ciukdemo] phase 2 done\n");
}

/*
 * Phase 3 — four orbiting squares around screen center. Deterministic
 * rotation driven by sin_tab[]. Each square has a distinct palette color.
 */
static void phase_orbits(ciuki_services_t *svc,
                         const ciuki_gfx_services_t *gfx) {
    print_line(svc, "[ciukdemo] phase 3 orbits\n");
    int cx = (int)(W / 2U);
    int cy = (int)(H / 2U);
    int radius = 60;
    int size = 16;
    const unsigned char colors[4] = {44U, 124U, 204U, 40U};
    for (unsigned f = 0; f < PHASE3_FRAMES; f++) {
        gfx->mode13_fill(0U);
        /* Center marker. */
        gfx->mode13_fill_rect((unsigned)(cx - 3), (unsigned)(cy - 3),
                              6U, 6U, 15U);
        for (int k = 0; k < 4; k++) {
            unsigned phase = (f * 2U) + (unsigned)k * 16U;
            int ox = (icos(phase) * radius) / 64;
            int oy = (isin(phase) * radius) / 64;
            int x = cx + ox - size / 2;
            int y = cy + oy - size / 2;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x + size > (int)W) x = (int)W - size;
            if (y + size > (int)H) y = (int)H - size;
            gfx->mode13_fill_rect((unsigned)x, (unsigned)y,
                                  (unsigned)size, (unsigned)size,
                                  colors[k]);
        }
        gfx->present();
        busy_delay(FRAME_BUSY_DELAY);
    }
    print_line(svc, "[ciukdemo] phase 3 done\n");
}

/*
 * Phase 4 — concentric rings expanding outward via a distance field. Written
 * directly to the mode13 plane.
 */
static void phase_rings(ciuki_services_t *svc,
                        const ciuki_gfx_services_t *gfx) {
    print_line(svc, "[ciukdemo] phase 4 rings\n");
    unsigned char *plane = gfx->mode13_plane();
    if (!plane) {
        print_line(svc, "[ciukdemo] phase 4 skipped (no plane)\n");
        return;
    }
    int cx = (int)(W / 2U);
    int cy = (int)(H / 2U);
    for (unsigned f = 0; f < PHASE4_FRAMES; f++) {
        unsigned shift = f * 6U;
        for (unsigned y = 0; y < H; y++) {
            unsigned row = y * W;
            int dy = (int)y - cy;
            int dy2 = dy * dy;
            for (unsigned x = 0; x < W; x++) {
                int dx = (int)x - cx;
                unsigned d2 = (unsigned)(dx * dx + dy2);
                /* Cheap radial index: d2 >> 6 bands similar enough to a real
                 * Euclidean distance for visible rings, at a tiny cost. */
                unsigned dist = d2 >> 6;
                plane[row + x] = (unsigned char)((dist + shift) & 0xFFU);
            }
        }
        gfx->present();
        busy_delay(FRAME_BUSY_DELAY);
    }
    print_line(svc, "[ciukdemo] phase 4 done\n");
}

/*
 * Phase 5 — fade entire palette to black. Uses the services palette_fade
 * primitive (DOOM-style screen wipe). Scene underneath remains static.
 */
static void phase_fadeout(ciuki_services_t *svc,
                          const ciuki_gfx_services_t *gfx) {
    print_line(svc, "[ciukdemo] phase 5 fadeout\n");
    /* Freeze last rings frame; the fade will dim it smoothly. */
    for (unsigned s = 0; s <= PHASE5_FRAMES; s++) {
        gfx->palette_fade(0x00000000U, s, PHASE5_FRAMES);
        gfx->present();
        busy_delay(FRAME_BUSY_DELAY);
    }
    print_line(svc, "[ciukdemo] phase 5 done\n");
}

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    const ciuki_gfx_services_t *gfx = svc->gfx;
    ciuki_int21_regs_t regs;

    print_line(svc, "[ciukdemo] start\n");

    if (!gfx || !gfx->set_mode || !gfx->mode13_fill ||
        !gfx->mode13_fill_rect || !gfx->present ||
        !gfx->palette_fade || !gfx->mode13_plane) {
        print_line(svc, "[ciukdemo] FAIL: gfx services incomplete\n");
        goto exit;
    }

    if (!gfx->set_mode(0x13U)) {
        print_line(svc, "[ciukdemo] FAIL: set_mode 0x13\n");
        goto exit;
    }

    phase_title(svc, gfx);
    phase_plasma(svc, gfx);
    phase_orbits(svc, gfx);
    phase_rings(svc, gfx);
    phase_fadeout(svc, gfx);

    print_line(svc, "[ciukdemo] OK\n");

exit:
    regs.ax = 0x4C00U;
    regs.bx = 0U;
    regs.cx = 0U;
    regs.dx = 0U;
    regs.si = 0U;
    regs.di = 0U;
    regs.ds = 0U;
    regs.es = 0U;
    regs.carry = 0U;
    regs.reserved[0] = 0U;
    regs.reserved[1] = 0U;
    regs.reserved[2] = 0U;
    if (svc->int21) {
        svc->int21(ctx, &regs);
    } else if (svc->int21_4c) {
        svc->int21_4c(ctx, 0x00);
    } else {
        svc->terminate(ctx, 0x00);
    }
}
