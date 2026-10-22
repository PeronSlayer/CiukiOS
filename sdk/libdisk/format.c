/* Byte builders for the whole-disk layout. Oracles: scripts/build_image.py
 * (MBR entry with saturated CHS markers, initialize_blackbox) and mkfs.fat
 * 4.2 (dosfstools, 2021-01-31) run as `mkfs.fat -a -F 32 -S 512 -s 8 -f 2
 * -R 160 -b 6 -h 2048 -n CIUKIOS` on a partition-sized file. Field layout:
 * Microsoft "FAT: General Overview of On-Disk Format" v1.03 (fatgen103),
 * sections 3.1-3.3 (BPB), 4 (FAT[0]/FAT[1], clean and hard-error bits),
 * 5 (FSInfo) and 6 (directory entry). SPDX-License-Identifier: MIT */
#include "internal.h"
#include <string.h>

uint32_t disk_crc32(const void *data, size_t n)
{
    /* zlib/IEEE 802.3 CRC-32, reflected polynomial 0xEDB88320. */
    const uint8_t *p = data;
    uint32_t crc = 0xffffffffu;
    while (n--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++) crc = crc >> 1 ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

int disk_geometry_whole(uint64_t capacity, struct disk_geometry *g)
{
    memset(g, 0, sizeof(*g));
    uint64_t end = capacity < DISK_LBA28 ? capacity : DISK_LBA28;
    if (end <= DISK_PART_LBA) return DISK_EINTERNAL;
    uint64_t total = end - DISK_PART_LBA;
    g->part_start = DISK_PART_LBA;
    g->part_sectors = (uint32_t)total;
    g->reserved = DISK_RESERVED;
    g->root_cluster = 2; g->fsinfo = 1; g->backup = 6;
    /* mkfs.fat 4.2 setup_tables(), FAT32 branch without alignment (-a):
     * clusters = (fatdata * 512 + nr_fats * 8) / (spc * 512 + nr_fats * 4),
     * fat = ceil((clusters + 2) * 4 / 512), then the clusters that really
     * fit after both FATs. */
    uint64_t fatdata = total - DISK_RESERVED;
    uint64_t clusters = (fatdata * 512u + DISK_FATS * 8u) / (DISK_SPC * 512u + DISK_FATS * 4u);
    uint64_t fat = ((clusters + 2) * 4 + 511) / 512;
    if (fatdata <= DISK_FATS * fat) return DISK_EINTERNAL;
    clusters = (fatdata - DISK_FATS * fat) / DISK_SPC;
    if (fat * 128u < clusters + 2) {
        /* mkfs.fat would leave the last cluster without a FAT entry here;
         * ciukldr and fat.c refuse such a volume. One more FAT sector. */
        fat++;
        if (fatdata <= DISK_FATS * fat) return DISK_EINTERNAL;
        clusters = (fatdata - DISK_FATS * fat) / DISK_SPC;
    }
    g->fat_sectors = (uint32_t)fat;
    g->clusters = (uint32_t)clusters;
    g->data_sector = DISK_RESERVED + DISK_FATS * g->fat_sectors;
    /* fatgen103 p. 21: TmpVal1 = DskSize - ResvdSecCnt (no root directory
     * sectors on FAT32); TmpVal2 = (256 * SecPerClus + NumFATs) / 2;
     * FATSz = ceil(TmpVal1 / TmpVal2). It may exceed the exact need. */
    uint64_t tmp2 = (256u * DISK_SPC + DISK_FATS) / 2;
    g->fat_spec_sectors = (uint32_t)((fatdata + tmp2 - 1) / tmp2);
    /* mkfs.fat 4.2 establish_params() for a device without geometry: the SD
     * card CHS rule below 256 MiB, LBA-assist translation above it. */
    if (total <= 524287) {
        g->heads = total <= 32767 ? 2 : total <= 65535 ? 4 : total <= 262143 ? 8 : 16;
        g->sectors_per_track = total <= 4095 ? 16 : 32;
    } else {
        g->heads = total <= 16u * 63 * 1024 ? 16 : total <= 32u * 63 * 1024 ? 32 :
                   total <= 64u * 63 * 1024 ? 64 : total <= 128u * 63 * 1024 ? 128 : 255;
        g->sectors_per_track = 63;
    }
    /* Loader (ciukldr/disk.inc): >= 65525 clusters and fat * 128 >= clusters + 2;
     * mkfs.fat itself refuses FAT32 below 65529 clusters. */
    if (g->clusters < DISK_MIN_CLUSTERS || (uint64_t)g->fat_sectors * 128u < (uint64_t)g->clusters + 2)
        return DISK_EINTERNAL;
    return 0;
}

void disk_fmt_table(uint8_t t[64], uint64_t start, uint32_t count)
{
    /* build_image.py: struct.pack_into("<B3sB3sII", mbr, 446, 0x80,
     * b"\x20\x21\0", 0x0C, b"\xfe\xff\xff", PART_LBA, count). The start CHS
     * is LBA 2048 under 255x63; the end is the saturated 1023/254/63 marker. */
    static const uint8_t head[8] = { 0x80, 0x20, 0x21, 0x00, 0x0c, 0xfe, 0xff, 0xff };
    memset(t, 0, 64);
    memcpy(t, head, sizeof(head));
    disk_wr32(t + 8, (uint32_t)start);
    disk_wr32(t + 12, count);
}

void disk_fmt_mbr(uint8_t out[512], const uint8_t code[440], uint32_t signature, const uint8_t table[64])
{
    memset(out, 0, 512);
    memcpy(out, code, 440);
    disk_wr32(out + 440, signature);  /* bytes 444-445 stay zero as in mbr.asm */
    memcpy(out + 446, table, 64);
    out[510] = 0x55; out[511] = 0xaa;
}

/* mkfs.fat 4.2's FAT32 boot program (jump EB 58 90 targets offset 0x5A): it
 * prints the message with INT 10h and reboots through INT 19h. Reproduced
 * byte for byte so that the formatted volume equals the build_image.py
 * oracle; CiukiOS never executes it (the MBR chains to ciukldr). */
static const uint8_t mkfs_boot_program[129] = {
    0x0e, 0x1f, 0xbe, 0x77, 0x7c, 0xac, 0x22, 0xc0, 0x74, 0x0b, 0x56, 0xb4, 0x0e, 0xbb, 0x07, 0x00,
    0xcd, 0x10, 0x5e, 0xeb, 0xf0, 0x32, 0xe4, 0xcd, 0x16, 0xcd, 0x19, 0xeb, 0xfe,
    'T','h','i','s',' ','i','s',' ','n','o','t',' ','a',' ','b','o','o','t','a','b','l','e',' ',
    'd','i','s','k','.',' ',' ','P','l','e','a','s','e',' ','i','n','s','e','r','t',' ','a',' ',
    'b','o','o','t','a','b','l','e',' ','f','l','o','p','p','y',' ','a','n','d','\r','\n',
    'p','r','e','s','s',' ','a','n','y',' ','k','e','y',' ','t','o',' ','t','r','y',' ',
    'a','g','a','i','n',' ','.','.','.',' ','\r','\n'
};

void disk_fmt_boot(uint8_t b[512], const struct disk_geometry *g, uint32_t volume_id)
{
    memset(b, 0, 512);
    b[0] = 0xeb; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "mkfs.fat", 8);           /* OEM name, informative only */
    disk_wr16(b + 11, DISK_SECTOR);
    b[13] = DISK_SPC;
    disk_wr16(b + 14, g->reserved);
    b[16] = DISK_FATS;                      /* root entries, total16, fat16: 0 */
    b[21] = 0xf8;                           /* fixed-disk media descriptor */
    disk_wr16(b + 24, g->sectors_per_track);
    disk_wr16(b + 26, g->heads);
    disk_wr32(b + 28, (uint32_t)g->part_start); /* hidden sectors */
    disk_wr32(b + 32, g->part_sectors);
    disk_wr32(b + 36, g->fat_sectors);      /* ext flags 0: both FATs mirrored */
    disk_wr32(b + 44, g->root_cluster);
    disk_wr16(b + 48, g->fsinfo);
    disk_wr16(b + 50, g->backup);
    b[64] = 0x80;                           /* BIOS drive number */
    b[66] = 0x29;                           /* extended boot signature */
    disk_wr32(b + 67, volume_id);
    memcpy(b + 71, "CIUKIOS    ", 11);
    memcpy(b + 82, "FAT32   ", 8);
    memcpy(b + 90, mkfs_boot_program, sizeof(mkfs_boot_program));
    b[510] = 0x55; b[511] = 0xaa;
}

void disk_fmt_fsinfo(uint8_t b[512], const struct disk_geometry *g)
{
    memset(b, 0, 512);
    disk_wr32(b, 0x41615252u);
    disk_wr32(b + 484, 0x61417272u);
    disk_wr32(b + 488, g->clusters - 1);    /* the root directory owns cluster 2 */
    disk_wr32(b + 492, 2);                  /* mkfs.fat 4.2 writes the hint 2 */
    disk_wr32(b + 508, 0xaa550000u);
}

void disk_fmt_blackbox(uint8_t b[512])
{
    /* build_image.py initialize_blackbox(): "<4sIII" CBB1, version 1, first
     * ring sector 32, 128 sectors, zlib.crc32 of bytes 0..507 at 508. */
    memset(b, 0, 512);
    memcpy(b, "CBB1", 4);
    disk_wr32(b + 4, 1);
    disk_wr32(b + 8, DISK_BLACKBOX_FIRST);
    disk_wr32(b + 12, DISK_BLACKBOX_SECTORS);
    disk_wr32(b + 508, disk_crc32(b, 508));
}

void disk_fmt_fat_first(uint8_t b[512], bool clean)
{
    memset(b, 0, 512);
    disk_wr32(b, 0x0ffffff8u);              /* FAT[0]: media F8 in the low byte */
    disk_wr32(b + 4, clean ? 0x0fffffffu : 0x0fffffffu & ~DISK_FAT32_CLEAN);
    disk_wr32(b + 8, 0x0ffffff8u);          /* FAT[2]: root directory, end of chain */
}

void disk_fmt_root_first(uint8_t b[512], uint16_t date, uint16_t time)
{
    memset(b, 0, 512);
    memcpy(b, "CIUKIOS    ", 11);
    b[11] = 0x08;                           /* ATTR_VOLUME_ID */
    disk_wr16(b + 14, time); disk_wr16(b + 16, date); disk_wr16(b + 18, date);
    disk_wr16(b + 22, time); disk_wr16(b + 24, date);
}
