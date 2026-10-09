#ifndef CIUKIOS_SESSION_GPU_MACH64_H
#define CIUKIOS_SESSION_GPU_MACH64_H
#include <stdint.h>

#define CVMACH64_CAP_FILL 1U
#define CVMACH64_CAP_COPY 4U
/* Layout-compatible with the fixed 128-byte native GPU diagnostic packet.
 * triangle fields stay zero: this is a hardware 2D implementation only. */
typedef struct {
    uint32_t ready, caps, error_stage, device_id;
    uint32_t mmio_physical, framebuffer_physical, aperture_physical, vram_bytes;
    uint32_t scratch_offset, scratch_bytes, pitch, bpp;
    uint32_t fills, blits, triangles, fifo_timeouts;
    uint32_t idle_timeouts, last_status, binds, selftests;
    uint32_t width, height, command_words, owns_engine;
    uint32_t triangle_selftests, triangle_probe_failures;
    uint32_t probe_actual, probe_expected;
    uint32_t panel_width, panel_height, panel_flags, copy_selftests;
} cvmach64_status_info;
extern volatile cvmach64_status_info cvmach64_status;

/* Automatic bind is read-only: checks PCI and the firmware mode, then unmaps.
 * Result 1 means probe completed; ready/caps remain zero until explicit step4. */
uint32_t cvmach64_bind(uint32_t physical, uint32_t bytes, uint32_t width,
                      uint32_t height, uint32_t pitch, uint32_t bpp,
                      uint32_t usable_bytes);
/* Explicit phases:1 read-only probe,2 setup+restore,3 private pixel proof+
 * restore,4 retain qualified fill/copy. 0 safe rejection/failure,1 completed,
 * 2 unsafe restoration: ownership retained and CPU VRAM access prohibited. */
uint32_t cvmach64_bind_step(uint32_t physical, uint32_t bytes, uint32_t width,
                           uint32_t height, uint32_t pitch, uint32_t bpp,
                           uint32_t usable_bytes, uint32_t step);
uint32_t cvmach64_owned(void);
/* sync/release return zero on success; nonzero prohibits CPU VRAM access. */
uint32_t cvmach64_sync(void);
uint32_t cvmach64_release(void);
/* 0 completed,1 inapplicable without writes,2 hardware failure. */
uint32_t cvmach64_fill(uint32_t offset, uint32_t pixels,
                     uint32_t colour, uint32_t pixelbytes);
uint32_t cvmach64_page_copy(uint32_t source, uint32_t dest,
                          uint32_t rowbytes, uint32_t rows, uint32_t pitch);
#endif
