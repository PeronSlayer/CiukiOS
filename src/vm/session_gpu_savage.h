#ifndef CIUKIOS_SESSION_GPU_SAVAGE_H
#define CIUKIOS_SESSION_GPU_SAVAGE_H
#include <stdint.h>

#define CVSAVAGE_CAP_FILL 1U
#define CVSAVAGE_CAP_TRIANGLE 2U
#define CVSAVAGE_CAP_COPY 4U
#define CVSAVAGE_CAP_PACKED2D 8U /* status path information; public caps stay &7 */
#define CVSAVAGE_SETUP_CHECKS 21U
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
/* Tagged stage 4: original CR50 in bits15:8, active CR50 in bits23:16;
 * reason in bits26:24: 1=PBD low, 2=PBD high, 3..6=private pixel0..3.
 * C-tag bit27 records original MM8144_9 (32-bit register access enabled).
 * PBD reasons are inspected only after the actual private pixel proof fails.
 * The existing probe pair contains this predicate's actual/expected dword;
 * last_status contains the foreground colour register readback. Legacy A
 * records used last_status as a duplicate of the failed predicate instead;
 * legacy B records lack the original register-access-width flag.
 * It is a fill diagnostic, not a completed triangle proof. */
#define CVSAVAGE_FILL_DIAG_MASK 0xf0000000UL
#define CVSAVAGE_FILL_DIAG_TAG 0xc0000000UL
#define CVSAVAGE_FILL_ORIGINAL_32B 0x08000000UL

/* Tagged rollback failure, low-byte stage 8: reason bits27:24 are 1 for
 * completing DWORD restoration, 2 for completing the misc word write,
 * 3 for persistent misc readback (RSF excluded), 4 for original CONTROL,
 * 5 for completing the final original RSF replay.
 * Bits23:12 retain expected MM8144
 * payload (reserved6/10 excluded); last_status is rollback actual/status.
 * The probe pair retains the original full error_stage and last_status.
 * It never denotes a completed fill/triangle proof or released ownership. */
#define CVSAVAGE_RELEASE_DIAG_MASK 0xf0000000UL
#define CVSAVAGE_RELEASE_DIAG_TAG 0xe0000000UL /* legacy D checked RSF too */
#define CVSAVAGE_PACKED_DIAG_TAG 0xf0000000UL /* private packed-MMIO proof */
/* F-tag stage4: reasons1..4 are private fill DWORD0..3; reason5 private
 * two-row copy; reasons6/7 are fill/copy timeouts (last_status engine poll,
 * probe pair zero). CR50/access-width fields match C-tag; probe pair contains
 * actual/expected and last_status foreground colour. Never a 3D proof. */

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
    /* Last triangle probe, tagged fill predicate, or rollback cause above. */
    uint32_t triangle_probe_pixel, triangle_probe_expected;
    uint32_t panel_width, panel_height, panel_flags, copy_selftests;
} cvsavage_status_info;
extern volatile cvsavage_status_info cvsavage_status;

/* Caller must have established and verified a BIOS true-LFB mode. */
/* bind: 0 unsupported, 1 ready, 2 retained ownership after hardware failure. */
uint32_t cvsavage_bind(uint32_t physical, uint32_t bytes,
                       uint32_t width, uint32_t height,
                       uint32_t pitch, uint32_t bpp, uint32_t usable_bytes);
/* Explicit qualification only: 1 MMIO, 2 setup, 3 private 2D, 4 retain
 * qualified 2D/3D, 5 retain independently re-proven packed2D only (no3D).
 * Stages 1..3 restore original state; same result convention.
 * 0101h..0115h execute successively longer, restored setup prefixes, so a
 * durable caller checkpoint can isolate a hardware access which never returns. */
uint32_t cvsavage_bind_step(uint32_t physical, uint32_t bytes,
                       uint32_t width, uint32_t height,
                       uint32_t pitch, uint32_t bpp, uint32_t usable_bytes,
                       uint32_t step);
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
