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

    if (info.stack_offset != 0x0010U || info.stack_top_offset != 0x0012U || info.runtime_required_bytes != 0x20U) {
        fprintf(stderr, "[FAIL] runtime contract info mismatch\n");
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

static int test_stack_outside_module_contract(void) {
    u8 mz[96];
    dos_mz_info_t info;

    fill_valid_mz(mz, (u32)sizeof(mz));

    /* Stack base at offset 0x30, outside 0x20-byte module. */
    write_le16(mz + 0x0E, 0x0003); /* ss */
    write_le16(mz + 0x10, 0x0000); /* sp */

    if (!dos_mz_parse(mz, (u32)sizeof(mz), &info)) {
        fprintf(stderr, "[FAIL] parse stack outside module contract\n");
        return 0;
    }

    if (info.stack_offset != 0x30U || info.stack_top_offset != 0x32U || info.runtime_required_bytes != 0x32U) {
        fprintf(stderr, "[FAIL] runtime required bytes mismatch for stack contract\n");
        return 0;
    }

    return 1;
}

static int test_invalid_page_math_rejected(void) {
    u8 mz[64];
    dos_mz_info_t info;

    fill_valid_mz(mz, (u32)sizeof(mz));

    /* Declared file size becomes 0x10 (< header 0x20): invalid page math. */
    write_le16(mz + 0x02, 0x0010); /* bytes in last page */
    write_le16(mz + 0x04, 0x0001); /* total pages */

    if (dos_mz_parse(mz, (u32)sizeof(mz), &info)) {
        fprintf(stderr, "[FAIL] accepted invalid declared page math\n");
        return 0;
    }

    return 1;
}

static int test_reloc_table_overlap_fixed_header_rejected(void) {
    u8 mz[64];
    dos_mz_info_t info;

    fill_valid_mz(mz, (u32)sizeof(mz));

    /* Relocation table offset overlaps fixed header bytes [0x00..0x1B]. */
    write_le16(mz + 0x18, 0x0018);

    if (dos_mz_parse(mz, (u32)sizeof(mz), &info)) {
        fprintf(stderr, "[FAIL] accepted relocation table overlapping fixed header\n");
        return 0;
    }

    return 1;
}

static int test_entry_last_byte_allowed(void) {
    u8 mz[64];
    dos_mz_info_t info;

    fill_valid_mz(mz, (u32)sizeof(mz));

    /* Entry exactly at last byte of 0x20-byte module is valid. */
    write_le16(mz + 0x14, 0x001F); /* ip */
    write_le16(mz + 0x16, 0x0000); /* cs */

    if (!dos_mz_parse(mz, (u32)sizeof(mz), &info)) {
        fprintf(stderr, "[FAIL] rejected valid last-byte entry\n");
        return 0;
    }

    if (info.entry_offset != 0x1FU) {
        fprintf(stderr, "[FAIL] last-byte entry offset mismatch\n");
        return 0;
    }

    return 1;
}

static int test_entry_equal_module_size_rejected(void) {
    u8 mz[64];
    dos_mz_info_t info;

    fill_valid_mz(mz, (u32)sizeof(mz));

    /* Entry at module_size (0x20) is out-of-bounds. */
    write_le16(mz + 0x14, 0x0020); /* ip */
    write_le16(mz + 0x16, 0x0000); /* cs */

    if (dos_mz_parse(mz, (u32)sizeof(mz), &info)) {
        fprintf(stderr, "[FAIL] accepted out-of-bounds entry offset\n");
        return 0;
    }

    return 1;
}

static int test_stack_top_exact_module_end_contract(void) {
    u8 mz[64];
    dos_mz_info_t info;

    fill_valid_mz(mz, (u32)sizeof(mz));

    /* stack_offset=0x1E -> stack_top=0x20 exactly at module end boundary. */
    write_le16(mz + 0x0E, 0x0000); /* ss */
    write_le16(mz + 0x10, 0x001E); /* sp */

    if (!dos_mz_parse(mz, (u32)sizeof(mz), &info)) {
        fprintf(stderr, "[FAIL] rejected stack exact-end contract\n");
        return 0;
    }

    if (info.stack_offset != 0x1EU || info.stack_top_offset != 0x20U || info.runtime_required_bytes != 0x20U) {
        fprintf(stderr, "[FAIL] stack exact-end contract mismatch\n");
        return 0;
    }

    return 1;
}

static int test_multi_reloc_and_carry_wrap(void) {
    u8 mz[128];
    dos_mz_info_t info;
    u32 loaded_size = 0;
    u32 reloc_applied = 0;

    fill_valid_mz(mz, (u32)sizeof(mz));

    /* Enlarge header to 0x30 so relocation table can host 2 entries. */
    write_le16(mz + 0x08, 0x0003); /* header paragraphs */
    write_le16(mz + 0x02, 0x0080); /* bytes in last page */
    write_le16(mz + 0x04, 0x0001); /* total pages */

    /* Two relocation entries at module offsets 0x0004 and 0x0006. */
    write_le16(mz + 0x06, 0x0002); /* relocation count */
    write_le16(mz + 0x1C, 0x0004);
    write_le16(mz + 0x1E, 0x0000);
    write_le16(mz + 0x20, 0x0006);
    write_le16(mz + 0x22, 0x0000);

    /* First value normal add, second value exercises 16-bit carry wrap. */
    write_le16(mz + 0x30 + 0x04, 0x1000);
    write_le16(mz + 0x30 + 0x06, 0xFFFE);

    if (!dos_mz_build_loaded_image(mz, (u32)sizeof(mz), 0x0200U, &info, &loaded_size, &reloc_applied)) {
        fprintf(stderr, "[FAIL] multi-reloc build failed\n");
        return 0;
    }

    if (reloc_applied != 2U) {
        fprintf(stderr, "[FAIL] multi-reloc applied count mismatch\n");
        return 0;
    }

    if (read_le16(mz + 0x04) != 0x1200U) {
        fprintf(stderr, "[FAIL] first relocation value mismatch\n");
        return 0;
    }

    if (read_le16(mz + 0x06) != 0x01FEU) {
        fprintf(stderr, "[FAIL] relocation carry-wrap mismatch\n");
        return 0;
    }

    return 1;
}

static int test_runtime_span_zero_filled(void) {
    u8 mz[96];
    dos_mz_info_t info;
    u32 loaded_size = 0;
    u32 reloc_applied = 0;

    fill_valid_mz(mz, (u32)sizeof(mz));

    /* stack_offset=0x30, stack_top=0x32 so runtime span extends beyond module_size=0x20. */
    write_le16(mz + 0x0E, 0x0003); /* ss */
    write_le16(mz + 0x10, 0x0000); /* sp */

    /* Fill overlay with non-zero noise to verify deterministic clearing. */
    memset(mz + 64, 0xA5, 32);

    if (!dos_mz_build_loaded_image(mz, (u32)sizeof(mz), 0x0100U, &info, &loaded_size, &reloc_applied)) {
        fprintf(stderr, "[FAIL] runtime span build failed\n");
        return 0;
    }

    if (info.runtime_required_bytes != 0x32U || loaded_size != 0x20U) {
        fprintf(stderr, "[FAIL] runtime span metadata mismatch\n");
        return 0;
    }

    for (u32 i = loaded_size; i < info.runtime_required_bytes; i++) {
        if (mz[i] != 0U) {
            fprintf(stderr, "[FAIL] runtime span not zero-filled\n");
            return 0;
        }
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
    if (!test_stack_outside_module_contract()) {
        return 1;
    }
    if (!test_invalid_page_math_rejected()) {
        return 1;
    }
    if (!test_reloc_table_overlap_fixed_header_rejected()) {
        return 1;
    }
    if (!test_entry_last_byte_allowed()) {
        return 1;
    }
    if (!test_entry_equal_module_size_rejected()) {
        return 1;
    }
    if (!test_stack_top_exact_module_end_contract()) {
        return 1;
    }
    if (!test_multi_reloc_and_carry_wrap()) {
        return 1;
    }
    if (!test_runtime_span_zero_filled()) {
        return 1;
    }

    printf("[PASS] deterministic MZ regression suite\n");
    return 0;
}
