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

/*
 * Budget tiers by resolution class (P1-V2).
 * Each tier defines the max framebuffer bytes for that class.
 * Double-buffering is allowed when the frame fits the corresponding tier.
 * Tier 0 (baseline): 1024x768x4 = 3,145,728
 * Tier 1 (HD):       1280x800x4 = 4,096,000
 * Tier 2 (HD+):      1600x900x4 = 5,760,000
 * Tier 3 (FHD):      1920x1080x4 = 8,294,400  (VIDEO_DRIVER_MAX)
 * Tier 4 (QHD):     2560x1440x4 = 14,745,600  (single-buffer only)
 * Tier 5 (4K):      3840x2160x4 = 33,177,600  (single-buffer only)
 */
#define VIDEO_BUDGET_TIER_BASELINE_BYTES  (1024U * 768U * 4U)
#define VIDEO_BUDGET_TIER_HD_BYTES        (1280U * 800U * 4U)
#define VIDEO_BUDGET_TIER_HDP_BYTES       (1600U * 900U * 4U)
#define VIDEO_BUDGET_TIER_FHD_BYTES       (1920U * 1080U * 4U)
#define VIDEO_BUDGET_TIER_QHD_BYTES       (2560U * 1440U * 4U)
#define VIDEO_BUDGET_TIER_4K_BYTES        (3840U * 2160U * 4U)

/* Safe ceiling: never allocate beyond this in the framebuffer path */
#define VIDEO_BUDGET_SAFE_CEILING         VIDEO_BUDGET_TIER_FHD_BYTES

#endif
