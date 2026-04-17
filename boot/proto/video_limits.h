#ifndef CIUKIOS_VIDEO_LIMITS_H
#define CIUKIOS_VIDEO_LIMITS_H

/*
 * Maximum resolution the stage2 video driver can double-buffer.
 * BSS budget: VIDEO_DRIVER_MAX_W * VIDEO_DRIVER_MAX_H * VIDEO_DRIVER_MAX_BPP bytes.
 * 1920 * 1080 * 4 = 8,294,400 bytes (~7.92 MB).
 * Stage2 loads at 128 MB (linker.ld), QEMU runs with 512 MB RAM.
 * Direct rendering fallback for modes exceeding this budget.
 */
#define VIDEO_DRIVER_MAX_W    1920U
#define VIDEO_DRIVER_MAX_H    1080U
#define VIDEO_DRIVER_MAX_BPP  4U   /* bytes per pixel (32bpp) */

/* Baseline policy: 1024x768 must always remain available and selectable. */
#define VIDEO_POLICY_BASELINE_W  1024U
#define VIDEO_POLICY_BASELINE_H  768U

/* GOP mode catalog: max entries passed from loader to stage2 */
#define VIDEO_GOP_CATALOG_MAX 64U

#endif
