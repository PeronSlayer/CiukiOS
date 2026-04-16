#ifndef CIUKIOS_VIDEO_LIMITS_H
#define CIUKIOS_VIDEO_LIMITS_H

/*
 * Maximum resolution the stage2 video driver can double-buffer.
 * BSS budget: VIDEO_DRIVER_MAX_W * VIDEO_DRIVER_MAX_H * 4 bytes.
 * 800 * 600 * 4 = 1,920,000 bytes (~1.83 MB).
 * Stage2 loads at 3 MB, QEMU OVMF ceiling ~8 MB total.
 */
#define VIDEO_DRIVER_MAX_W    800U
#define VIDEO_DRIVER_MAX_H    600U
#define VIDEO_DRIVER_MAX_BPP  4U   /* bytes per pixel (32bpp) */

/* GOP mode catalog: max entries passed from loader to stage2 */
#define VIDEO_GOP_CATALOG_MAX 64U

#endif
