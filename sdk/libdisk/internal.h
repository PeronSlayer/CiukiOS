/* libdisk internals shared by format.c, disk.c and the native backend.
 * SPDX-License-Identifier: MIT */
#ifndef LIBDISK_INTERNAL_H
#define LIBDISK_INTERNAL_H
#include <libdisk.h>
#include <ciuki/abi.h>
#define DISK_FAT32_CLEAN 0x08000000u   /* FAT[1] bit 27: volume cleanly dismounted */
#define DISK_FAT32_NO_ERROR 0x04000000u /* FAT[1] bit 26: no hard error seen */
static inline void disk_wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void disk_wr32(uint8_t *p, uint32_t v) { disk_wr16(p, v); disk_wr16(p + 2, v >> 16); }
static inline uint32_t disk_rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t disk_rd32(const uint8_t *p) { return disk_rd16(p) | disk_rd16(p + 2) << 16; }
uint32_t disk_crc32(const void *, size_t);
void disk_fmt_table(uint8_t table[64], uint64_t start, uint32_t count);
void disk_fmt_mbr(uint8_t out[512], const uint8_t code[440], uint32_t signature, const uint8_t table[64]);
void disk_fmt_boot(uint8_t out[512], const struct disk_geometry *, uint32_t volume_id);
void disk_fmt_fsinfo(uint8_t out[512], const struct disk_geometry *);
void disk_fmt_blackbox(uint8_t out[512]);
void disk_fmt_fat_first(uint8_t out[512], bool clean);
void disk_fmt_root_first(uint8_t out[512], uint16_t date, uint16_t time);
/* Content of one planned sector: lba is relative to the range start. */
void disk_range_sector(const struct disk_plan *, const struct disk_range *, uint32_t index,
                       const void *loader, uint8_t out[512]);
#endif
