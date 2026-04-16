#include <stdio.h>
#include <string.h>

#include "dos_mz.h"

static void write_le16(u8 *p, u16 v) {
    p[0] = (u8)(v & 0xFFU);
    p[1] = (u8)((v >> 8) & 0xFFU);
}

static u16 read_le16(const u8 *p) {
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static void fill_valid_mz(u8 *buf, u32 total_size) {
    memset(buf, 0, (size_t)total_size);

    /* MZ header with 0x20-byte header and 0x20-byte loadable module. */
    buf[0] = 'M';
    buf[1] = 'Z';
    write_le16(buf + 0x02, 0x0040); /* bytes in last page */
    write_le16(buf + 0x04, 0x0001); /* total pages */
    write_le16(buf + 0x06, 0x0001); /* relocation count */
    write_le16(buf + 0x08, 0x0002); /* header paragraphs */
    write_le16(buf + 0x0A, 0x0000); /* min alloc */
    write_le16(buf + 0x0C, 0xFFFF); /* max alloc */
    write_le16(buf + 0x0E, 0x0000); /* ss */
    write_le16(buf + 0x10, 0x0010); /* sp */
    write_le16(buf + 0x14, 0x0000); /* ip */
    write_le16(buf + 0x16, 0x0000); /* cs */
    write_le16(buf + 0x18, 0x001C); /* relocation table offset */

    /* One relocation entry: offset=0x0004, segment=0x0000. */
    write_le16(buf + 0x1C, 0x0004);
    write_le16(buf + 0x1E, 0x0000);

    /* Word to patch at loadable module offset 0x0004. */
    write_le16(buf + 0x20 + 0x04, 0x1234);
}

static int test_parse_and_relocate(void) {
    u8 mz[64];
    dos_mz_info_t info;
    u32 loaded_size;
    u32 reloc_applied;

    fill_valid_mz(mz, (u32)sizeof(mz));

    if (!dos_mz_parse(mz, (u32)sizeof(mz), &info)) {
        fprintf(stderr, "[FAIL] parse valid MZ\n");
        return 0;
    }

    if (info.header_size_bytes != 0x20U || info.module_size_bytes != 0x20U || info.entry_offset != 0U) {
        fprintf(stderr, "[FAIL] parse info mismatch\n");
        return 0;
    }

    if (!dos_mz_build_loaded_image(mz, (u32)sizeof(mz), 0x0200U, &info, &loaded_size, &reloc_applied)) {
        fprintf(stderr, "[FAIL] build loaded image failed\n");
        return 0;
    }

    if (loaded_size != 0x20U || reloc_applied != 1U) {
        fprintf(stderr, "[FAIL] load output mismatch\n");
        return 0;
    }

    if (read_le16(mz + 0x04) != 0x1434U) {
        fprintf(stderr, "[FAIL] relocation patch mismatch\n");
        return 0;
    }

    return 1;
}

static int test_overlay_ignored(void) {
    u8 mz[96];
    dos_mz_info_t info;

    fill_valid_mz(mz, (u32)sizeof(mz));
    memset(mz + 64, 0xA5, 32);

    if (!dos_mz_parse(mz, (u32)sizeof(mz), &info)) {
        fprintf(stderr, "[FAIL] parse overlay case\n");
        return 0;
    }

    if (info.module_size_bytes != 0x20U) {
        fprintf(stderr, "[FAIL] overlay included in module size\n");
        return 0;
    }

    return 1;
}

static int test_reloc_outside_module_rejected(void) {
    u8 mz[96];
    dos_mz_info_t info;
    u32 loaded_size = 0;
    u32 reloc_applied = 0;

    fill_valid_mz(mz, (u32)sizeof(mz));

    /* Point relocation target at module offset 0x30, outside declared 0x20-byte module,
     * but still inside physical file thanks to overlay bytes. */
    write_le16(mz + 0x1C, 0x0030);
    memset(mz + 64, 0xCC, 32);

    if (dos_mz_build_loaded_image(mz, (u32)sizeof(mz), 0x0100U, &info, &loaded_size, &reloc_applied)) {
        fprintf(stderr, "[FAIL] accepted relocation outside declared module\n");
        return 0;
    }

    return 1;
}

int main(void) {
    if (!test_parse_and_relocate()) {
        return 1;
    }
    if (!test_overlay_ignored()) {
        return 1;
    }
    if (!test_reloc_outside_module_rejected()) {
        return 1;
    }

    printf("[PASS] deterministic MZ regression suite\n");
    return 0;
}
