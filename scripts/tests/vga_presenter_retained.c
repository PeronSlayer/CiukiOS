/* Host-only source regression for the retained VGA band presenter.
 * Build with virtual_vga.c and vga_presenter.c; this file intentionally has
 * no dependency on the OS image, DOS, or a display device. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/vm/vga_presenter.h"

#define LOGICAL_WIDTH 4u
#define LOGICAL_HEIGHT 400u
#define BAND_WIDTH 4u
#define INITIAL_BAND_HEIGHT 100u

static cvga_state live;
static cvp_retained_state retained;
static uint8_t dirty[4][CVGA_DIRTY_BYTES];
static uint8_t band[ BAND_WIDTH * INITIAL_BAND_HEIGHT * 4u ];

static int fail(const char *what, int line)
{
    fprintf(stderr, "retained presenter regression failed at line %d: %s\n", line, what);
    return 1;
}

#define CHECK(condition, what) do { if (!(condition)) return fail((what), __LINE__); } while (0)

static void output_format(cvp_format *f, unsigned width, unsigned height)
{
    memset(f, 0, sizeof(*f));
    f->pitch = width * 2u;
    f->width = (uint16_t)width;
    f->height = (uint16_t)height;
    f->bytes = 2;
    f->red_size = 5; f->red_pos = 11;
    f->green_size = 6; f->green_pos = 5;
    f->blue_size = 5; f->blue_pos = 0;
}

static uint16_t pixel16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static unsigned dirty_rows(const cvga_state *v,
                           const uint8_t rows[4][CVGA_DIRTY_BYTES])
{
    cvga_geometry g;
    unsigned y, n = 0;
    if (!cvga_get_geometry(v, &g)) return 0;
    for (y = 0; y < g.height; ++y)
        if (cvga_row_reads_dirty(v, y, rows)) ++n;
    return n;
}

static int capture(cvp_format *logical, void *storage, size_t capacity,
                   const uint8_t rows[4][CVGA_DIRTY_BYTES], int force)
{
    return cvp_retained_capture(&retained, &live, logical,
                               LOGICAL_WIDTH, LOGICAL_HEIGHT,
                               CVP_BLINK_VISIBLE | CVP_CURSOR_VISIBLE,
                               storage, capacity, rows, force);
}

static int blit_all(unsigned band_height, const cvp_request *q)
{
    cvp_format target_format;
    cvp_stats stats;
    unsigned top;
    output_format(&target_format, BAND_WIDTH, band_height);
    for (top = 0; top < LOGICAL_HEIGHT; top += band_height) {
        memset(band, 0x5a, sizeof(band));
        if (cvp_retained_blit(&retained, &target_format, q, 0, (int32_t)top,
                              band, &stats) != CVP_OK)
            return 0;
    }
    return 1;
}

int main(void)
{
    cvp_format logical, target_format, bad;
    cvp_request request;
    cvp_stats stats;
    cvga_geometry geometry;
    uint16_t saved_x[LOGICAL_WIDTH], saved_y[LOGICAL_HEIGHT];
    size_t storage_bytes;
    void *storage;
    unsigned expected;

    cvga_init(&live);
    CHECK(cvga_set_bios_mode(&live, 0x13, 0), "mode 13h setup");
    CHECK(cvga_get_geometry(&live, &geometry), "mode 13h geometry");
    CHECK(geometry.width == 320 && geometry.height == 200,
          "mode 13h source geometry");
    live.dac[0][0] = live.dac[0][1] = live.dac[0][2] = 0;
    live.dac[1][0] = 63; live.dac[1][1] = live.dac[1][2] = 0;
    live.dac[2][0] = live.dac[2][2] = 0; live.dac[2][1] = 63;
    live.dac[3][0] = 63; live.dac[3][1] = 63; live.dac[3][2] = 0;
    ++live.display_changes;
    cvga_store_plane(&live, 0, 0, 1);  /* first Mode 13h pixel uses palette 1 */
    memset(live.dirty, 0, sizeof(live.dirty));

    output_format(&logical, LOGICAL_WIDTH, LOGICAL_HEIGHT);
    storage_bytes = cvp_retained_bytes(&live, LOGICAL_WIDTH, logical.bytes);
    CHECK(storage_bytes != 0, "retained storage size");
    storage = malloc(storage_bytes);
    CHECK(storage != 0, "retained storage allocation");
    memset(&retained, 0, sizeof(retained));
    memset(&request, 0, sizeof(request));
    request.window.left = 0; request.window.top = 0;
    request.window.right = LOGICAL_WIDTH; request.window.bottom = LOGICAL_HEIGHT;

    CHECK(capture(&logical, storage, storage_bytes, 0, 1) == CVP_OK,
          "initial capture");
    cvga_store_plane(&live, 0, 0, 2);  /* mutate after capture, before any blit */
    output_format(&target_format, BAND_WIDTH, INITIAL_BAND_HEIGHT);
    memset(band, 0x5a, sizeof(band));
    CHECK(cvp_retained_blit(&retained, &target_format, &request, 0, 0,
                            band, &stats) == CVP_OK, "first frozen band");
    CHECK(pixel16(band) == 0xf800, "band must show captured red pixel");

    /* The next image includes the live write. Each output source row appears
     * twice at 2x vertical scaling, and is requested through four bands. */
    CHECK(capture(&logical, storage, storage_bytes, 0, 1) == CVP_OK,
          "capture changed image");
    memset(live.dirty, 0, sizeof(live.dirty));
    CHECK(blit_all(INITIAL_BAND_HEIGHT, &request), "four 100-row bands");
    CHECK(retained.source_rows_converted == geometry.height,
          "each source row converted once across scaled bands");

    /* A different scratch-band height must not invalidate the stable logical
     * maps, palette, or already-converted rows. */
    memcpy(saved_x, retained.x_map, sizeof(saved_x));
    memcpy(saved_y, retained.y_map, sizeof(saved_y));
    memset(dirty, 0, sizeof(dirty));
    CHECK(capture(&logical, storage, storage_bytes, dirty, 0) == CVP_OK,
          "unchanged capture");
    CHECK(blit_all(50, &request), "eight 50-row bands");
    CHECK(retained.source_rows_converted == 0,
          "unchanged image reuses converted rows across band shapes");
    CHECK(!memcmp(saved_x, retained.x_map, sizeof(saved_x)) &&
          !memcmp(saved_y, retained.y_map, sizeof(saved_y)),
          "logical maps remain stable across scratch-band shapes");

    /* Change one source granule. Capture receives the caller's dirty snapshot;
     * all unchanged rows remain cached through the following band blits. */
    cvga_store_plane(&live, 0, 0, 3);
    memcpy(dirty, live.dirty, sizeof(dirty));
    memset(live.dirty, 0, sizeof(live.dirty));
    expected = dirty_rows(&live, (const uint8_t (*)[CVGA_DIRTY_BYTES])dirty);
    CHECK(expected != 0 && expected < geometry.height,
          "single granule affects only a subset of source rows");
    CHECK(capture(&logical, storage, storage_bytes,
                  (const uint8_t (*)[CVGA_DIRTY_BYTES])dirty, 0) == CVP_OK,
          "dirty-row capture");
    CHECK(blit_all(50, &request), "dirty image bands");
    CHECK(retained.source_rows_converted == expected,
          "only rows reading changed granules are reconverted");

    /* Occlusion and negative window origin: only the visible clip is copied
     * into the band, and guest x is translated relative to the offscreen left. */
    output_format(&target_format, BAND_WIDTH, 8);
    request.window.left = -1; request.window.top = -3;
    request.window.right = 3; request.window.bottom = 397;
    request.clip_count = 1;
    request.clip[0].left = 0; request.clip[0].top = 0;
    request.clip[0].right = 2; request.clip[0].bottom = 2;
    memset(band, 0x5a, sizeof(band));
    CHECK(cvp_retained_blit(&retained, &target_format, &request, 0, 0,
                            band, &stats) == CVP_OK, "clipped offscreen band");
    CHECK(band[4] == 0x5a && band[5] == 0x5a &&
          band[6] == 0x5a && band[7] == 0x5a,
          "occluded pixels remain untouched");
    CHECK(band[0] != 0x5a || band[1] != 0x5a,
          "visible clip pixels are copied");

    /* Invalid channel overlap, undersized stride, overflowing extent and
     * insufficient caller storage are rejected before touching the frame. */
    request.window.left = 0; request.window.top = 0;
    request.window.right = LOGICAL_WIDTH; request.window.bottom = LOGICAL_HEIGHT;
    request.clip_count = 0;
    bad = logical; bad.pitch = LOGICAL_WIDTH * bad.bytes - 1;
    CHECK(capture(&bad, storage, storage_bytes, 0, 0) == CVP_BAD_FORMAT,
          "undersized logical stride rejected");
    bad = logical; bad.pitch = 0xffffffffUL;
    CHECK(capture(&bad, storage, storage_bytes, 0, 0) == CVP_BAD_FORMAT,
          "overflowing logical extent rejected");
    bad = logical; bad.blue_pos = bad.red_pos;
    CHECK(capture(&bad, storage, storage_bytes, 0, 0) == CVP_BAD_FORMAT,
          "overlapping color channels rejected");
    CHECK(capture(&logical, storage, storage_bytes - 1, 0, 0) == CVP_BAD_STORAGE,
          "short snapshot storage rejected");
    output_format(&target_format, BAND_WIDTH, 8);
    target_format.pitch = BAND_WIDTH * target_format.bytes - 1;
    CHECK(cvp_retained_blit(&retained, &target_format, &request, 0, 0,
                            band, &stats) == CVP_BAD_FORMAT,
          "undersized band stride rejected");
    output_format(&target_format, BAND_WIDTH, 8);
    target_format.pitch = 0xffffffffUL;
    CHECK(cvp_retained_blit(&retained, &target_format, &request, 0, 0,
                            band, &stats) == CVP_BAD_FORMAT,
          "overflowing band extent rejected");

    free(storage);
    /* Actual DOS-window client dimensions, including the default 32-bit
     * desktop format. A 60-KiB scratch band must never cause repeat source
     * conversion when the same source row appears in two output bands. */
    for (unsigned bytes = 3; bytes <= 4; ++bytes) {
        uint8_t *scratch;
        const unsigned width = 674, height = 434, rows = 22;
        unsigned top;
        output_format(&logical, width, height);
        logical.bytes = (uint8_t)bytes;
        logical.pitch = width * bytes;
        logical.red_size = logical.green_size = logical.blue_size = 8;
        logical.red_pos = 16; logical.green_pos = 8; logical.blue_pos = 0;
        storage_bytes = cvp_retained_bytes(&live, width, bytes);
        storage = malloc(storage_bytes);
        scratch = malloc(width * rows * bytes);
        CHECK(storage && scratch, "DOS-window raster allocation");
        memset(&retained, 0, sizeof(retained));
        memset(&request, 0, sizeof(request));
        request.window.left = 5; request.window.top = 33;
        request.window.right = 5 + width; request.window.bottom = 33 + height;
        request.flags = CVP_NO_DAMAGE;
        CHECK(cvp_retained_capture(&retained, &live, &logical, width, height, 0,
                                  storage, storage_bytes, 0, 1) == CVP_OK,
              "capture actual DOS-window dimensions");
        target_format = logical;
        target_format.height = rows;
        for (top = 0; top < height; top += rows) {
            memset(scratch, 0x5a, width * rows * bytes);
            CHECK(cvp_retained_blit(&retained, &target_format, &request, 5, 33 + top,
                                    scratch, &stats) == CVP_OK,
                  "24/32-bit actual DOS-window band");
        }
        CHECK(retained.source_rows_converted == 200,
              "one conversion per source row through 20 DOS-window bands");
        CHECK(cvp_retained_blit(&retained, &target_format, &request, 5, 33,
                                scratch, &stats) == CVP_OK,
              "repeat already converted first band");
        CHECK(scratch[0] == 0 && scratch[1] == 255 && scratch[2] == 255,
              "24/32-bit packed yellow pixel");
        if (bytes == 4) CHECK(scratch[3] == 0, "32-bit unused channel");
        CHECK(retained.source_rows_converted == 200,
              "repaint reuses the retained DOS-window rows");
        free(scratch);
        free(storage);
    }
    puts("retained presenter regression: PASS");
    return 0;
}
