#ifndef CIUKIOS_SESSION_GPU_SAVAGE_H
#define CIUKIOS_SESSION_GPU_SAVAGE_H
#include <stdint.h>

#define CVSAVAGE_CAP_FILL 1U
#define CVSAVAGE_CAP_TRIANGLE 2U
#define CVSAVAGE_CAP_COPY 4U
#define CVSAVAGE_TRIANGLE_MAGIC 0x33545643UL /* CVT3 */

/* error_stage low byte remains the operation stage. Preflight reason bits
 * apply only to stage 1; last_status then holds the failed predicate's detail
 * (see native-supersavage-bci.md), not an MMIO status-register read. */
#define CVSAVAGE_STAGE_MASK 0xffUL
#define CVSAVAGE_PREFLIGHT_FORMAT 0x00000100UL
#define CVSAVAGE_PREFLIGHT_GEOMETRY 0x00000200UL
#define CVSAVAGE_PREFLIGHT_PITCH 0x00000400UL
#define CVSAVAGE_PREFLIGHT_EXTENT 0x00000800UL
#define CVSAVAGE_PREFLIGHT_MISSING 0x00001000UL
#define CVSAVAGE_PREFLIGHT_CLASS 0x00002000UL
#define CVSAVAGE_PREFLIGHT_DECODE 0x00004000UL
#define CVSAVAGE_PREFLIGHT_BAR0 0x00008000UL
#define CVSAVAGE_PREFLIGHT_BAR1 0x00010000UL
#define CVSAVAGE_PREFLIGHT_BAR2 0x00020000UL
#define CVSAVAGE_PREFLIGHT_PHYSICAL 0x00040000UL
#define CVSAVAGE_PREFLIGHT_MMIO_SPAN 0x00080000UL
#define CVSAVAGE_PREFLIGHT_MASK 0x000fff00UL

typedef struct {
    uint32_t x, y, z, argb; /* IEEE754 screen coordinates, packed ARGB8888. */
} cvsavage_vertex;
typedef struct {
    uint32_t magic, size, flags, reserved;
    cvsavage_vertex vertex[3];
} cvsavage_triangle_packet;

/* Fixed 128-byte diagnostic snapshot. Counts are completed GPU operations. */
typedef struct {
    uint32_t ready, caps, error_stage, device_id;
    uint32_t mmio_physical, framebuffer_physical, aperture_physical, vram_bytes;
    uint32_t scratch_offset, scratch_bytes, pitch, bpp;
    uint32_t fills, blits, triangles, fifo_timeouts;
    uint32_t idle_timeouts, last_status, binds, selftests;
    uint32_t width, height, command_words, owns_engine;
    uint32_t triangle_selftests, triangle_probe_failures;
    uint32_t triangle_probe_pixel, triangle_probe_expected;
    uint32_t panel_width, panel_height, panel_flags, copy_selftests;
} cvsavage_status_info;
extern volatile cvsavage_status_info cvsavage_status;

/* Caller must have established and verified a BIOS true-LFB mode. */
/* bind: 0 unsupported, 1 ready, 2 retained ownership after hardware failure. */
uint32_t cvsavage_bind(uint32_t physical, uint32_t bytes,
                       uint32_t width, uint32_t height,
                       uint32_t pitch, uint32_t bpp, uint32_t usable_bytes);
uint32_t cvsavage_owned(void);
/* Refresh read-only LCD size in status; 1 valid, 0 unavailable. */
uint32_t cvsavage_panel_probe(void);
/* sync/release: zero success; nonzero means CPU VRAM access is prohibited. */
uint32_t cvsavage_sync(void);
uint32_t cvsavage_release(void);
/* 0 completed, 1 inapplicable/invalid with no writes, 2 hardware failure. */
uint32_t cvsavage_fill(uint32_t offset, uint32_t pixels,
                       uint32_t colour, uint32_t pixelbytes);
uint32_t cvsavage_triangle(const cvsavage_triangle_packet *packet);
uint32_t cvsavage_page_copy(uint32_t source, uint32_t dest, uint32_t rowbytes,
                            uint32_t rows, uint32_t pitch);
#endif
