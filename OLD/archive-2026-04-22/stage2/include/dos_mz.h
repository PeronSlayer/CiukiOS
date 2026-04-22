#ifndef STAGE2_DOS_MZ_H
#define STAGE2_DOS_MZ_H

#include "types.h"

typedef struct dos_mz_info {
    u16 bytes_in_last_page;
    u16 total_pages;
    u16 relocation_count;
    u16 header_paragraphs;
    u16 min_alloc_paragraphs;
    u16 max_alloc_paragraphs;
    u16 ss;
    u16 sp;
    u16 ip;
    u16 cs;
    u16 relocation_table_offset;

    u32 header_size_bytes;
    u32 module_size_bytes;
    u32 entry_offset;
    u32 stack_offset;
    u32 stack_top_offset;
    u32 runtime_required_bytes;
} dos_mz_info_t;

/* Parse DOS MZ executable header from file buffer. */
int dos_mz_parse(const u8 *file, u32 file_size, dos_mz_info_t *out);

/*
 * Transform MZ file in-place into a loaded module image:
 * 1) apply relocations with given load_segment
 * 2) strip header by moving module bytes to buffer base
 */
int dos_mz_build_loaded_image(
    u8 *file_buf,
    u32 file_size,
    u16 load_segment,
    dos_mz_info_t *info_out,
    u32 *loaded_size_out,
    u32 *reloc_applied_out
);

#endif
