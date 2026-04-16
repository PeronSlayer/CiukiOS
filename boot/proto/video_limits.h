#ifndef CIUKIOS_VIDEO_LIMITS_H
#define CIUKIOS_VIDEO_LIMITS_H

/*
 * Maximum resolution the stage2 video driver can double-buffer.
 * BSS budget: VIDEO_DRIVER_MAX_W * VIDEO_DRIVER_MAX_H * 4 bytes.
 * 1024 * 768 * 4 = 3,145,728 bytes (~3.00 MB).
 * Stage2 loads at 3 MB, QEMU OVMF ceiling ~8 MB total.
 */
#define VIDEO_DRIVER_MAX_W    1024U
#define VIDEO_DRIVER_MAX_H    768U
#define VIDEO_DRIVER_MAX_BPP  4U   /* bytes per pixel (32bpp) */

/* GOP mode catalog: max entries passed from loader to stage2 */
#define VIDEO_GOP_CATALOG_MAX 64U

#endif
