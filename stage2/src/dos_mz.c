#include "dos_mz.h"

static u16 read_le16(const u8 *p) {
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static void write_le16(u8 *p, u16 v) {
    p[0] = (u8)(v & 0xFFU);
    p[1] = (u8)((v >> 8) & 0xFFU);
}

int dos_mz_parse(const u8 *file, u32 file_size, dos_mz_info_t *out) {
    u32 header_size;
    u32 declared_file_size;
    u32 module_size;
    u32 entry_offset;
    u32 stack_offset;
    u32 stack_top_offset;
    u32 runtime_required;
    u32 reloc_end;

    if (!file || !out || file_size < 0x1CU) {
        return 0;
    }

    if (file[0] != 'M' || file[1] != 'Z') {
        return 0;
    }

    out->bytes_in_last_page = read_le16(file + 0x02);
    out->total_pages = read_le16(file + 0x04);
    out->relocation_count = read_le16(file + 0x06);
    out->header_paragraphs = read_le16(file + 0x08);
    out->min_alloc_paragraphs = read_le16(file + 0x0A);
    out->max_alloc_paragraphs = read_le16(file + 0x0C);
    out->ss = read_le16(file + 0x0E);
    out->sp = read_le16(file + 0x10);
    out->ip = read_le16(file + 0x14);
    out->cs = read_le16(file + 0x16);
    out->relocation_table_offset = read_le16(file + 0x18);

    if (out->total_pages == 0U) {
        return 0;
    }

    if (out->bytes_in_last_page > 512U) {
        return 0;
    }

    if (out->header_paragraphs == 0U) {
        return 0;
    }

    header_size = (u32)out->header_paragraphs * 16U;
    if (header_size > file_size || header_size < 0x1CU) {
        return 0;
    }

    if (out->bytes_in_last_page == 0U) {
        declared_file_size = (u32)out->total_pages * 512U;
    } else {
        declared_file_size = ((u32)(out->total_pages - 1U) * 512U) + (u32)out->bytes_in_last_page;
    }

    if (declared_file_size < header_size || declared_file_size > file_size) {
        return 0;
    }

    module_size = declared_file_size - header_size;
    if (module_size == 0U) {
        return 0;
    }

    if ((u32)out->relocation_table_offset < 0x1CU) {
        return 0;
    }

    reloc_end = (u32)out->relocation_table_offset + ((u32)out->relocation_count * 4U);
    if (reloc_end > header_size || reloc_end > file_size) {
        return 0;
    }

    entry_offset = ((u32)out->cs * 16U) + (u32)out->ip;
    if (entry_offset >= module_size) {
        return 0;
    }

    stack_offset = ((u32)out->ss * 16U) + (u32)out->sp;
    stack_top_offset = stack_offset + 2U;
    runtime_required = module_size;
    if (stack_top_offset > runtime_required) {
        runtime_required = stack_top_offset;
    }

    out->header_size_bytes = header_size;
    out->module_size_bytes = module_size;
    out->entry_offset = entry_offset;
    out->stack_offset = stack_offset;
    out->stack_top_offset = stack_top_offset;
    out->runtime_required_bytes = runtime_required;

    return 1;
}

int dos_mz_build_loaded_image(
    u8 *file_buf,
    u32 file_size,
    u16 load_segment,
    dos_mz_info_t *info_out,
    u32 *loaded_size_out,
    u32 *reloc_applied_out
) {
    dos_mz_info_t info;
    u32 reloc_applied = 0;
    u32 image_end;

    if (!file_buf || !info_out || !loaded_size_out || !reloc_applied_out) {
        return 0;
    }

    if (!dos_mz_parse(file_buf, file_size, &info)) {
        return 0;
    }

    image_end = info.header_size_bytes + info.module_size_bytes;

    for (u32 i = 0; i < (u32)info.relocation_count; i++) {
        u32 reloc_off = (u32)info.relocation_table_offset + (i * 4U);
        u16 fixup_off = read_le16(file_buf + reloc_off);
        u16 fixup_seg = read_le16(file_buf + reloc_off + 2U);
        u32 target_in_file = info.header_size_bytes + ((u32)fixup_seg * 16U) + (u32)fixup_off;
        u16 original;
        u16 patched;

        if ((target_in_file + 1U) >= image_end) {
            return 0;
        }

        original = read_le16(file_buf + target_in_file);
        patched = (u16)(original + load_segment);
        write_le16(file_buf + target_in_file, patched);
        reloc_applied++;
    }

    for (u32 i = 0; i < info.module_size_bytes; i++) {
        file_buf[i] = file_buf[info.header_size_bytes + i];
    }

    /*
     * Keep runtime deterministic when SS:SP points beyond the module bytes:
     * clear the additional runtime span so stack-adjacent memory is stable.
     */
    for (u32 i = info.module_size_bytes; i < info.runtime_required_bytes; i++) {
        file_buf[i] = 0U;
    }

    info_out->bytes_in_last_page = info.bytes_in_last_page;
    info_out->total_pages = info.total_pages;
    info_out->relocation_count = info.relocation_count;
    info_out->header_paragraphs = info.header_paragraphs;
    info_out->min_alloc_paragraphs = info.min_alloc_paragraphs;
    info_out->max_alloc_paragraphs = info.max_alloc_paragraphs;
    info_out->ss = info.ss;
    info_out->sp = info.sp;
    info_out->ip = info.ip;
    info_out->cs = info.cs;
    info_out->relocation_table_offset = info.relocation_table_offset;
    info_out->header_size_bytes = info.header_size_bytes;
    info_out->module_size_bytes = info.module_size_bytes;
    info_out->entry_offset = info.entry_offset;
    info_out->stack_offset = info.stack_offset;
    info_out->stack_top_offset = info.stack_top_offset;
    info_out->runtime_required_bytes = info.runtime_required_bytes;
    *loaded_size_out = info.module_size_bytes;
    *reloc_applied_out = reloc_applied;
    return 1;
}
