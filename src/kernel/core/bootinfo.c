/* ciuki_boot_info v1 validation (docs/design/boot-memory.md).
 * Pure function, also compiled for host tests.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#include <stddef.h>
#include "ciuki/boot_info.h"

#define FAIL(msg) do { if (why) *why = (msg); return -22; } while (0)

static int all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

int bootinfo_validate(const struct ciuki_boot_info *bi, const char **why)
{
    if (why)
        *why = "ok";
    if (bi->magic != CIUKI_BOOT_INFO_MAGIC)
        FAIL("magic");
    if (bi->version != CIUKI_BOOT_INFO_VER)
        FAIL("version");
    if (bi->size != CIUKI_BOOT_INFO_SIZE)
        FAIL("size");
    if (bi->flags & ~CBI_F_DEFINED_MASK)
        FAIL("undefined_flag_bits");
    uint32_t a20 = (bi->flags & CBI_F_A20_MASK) >> CBI_F_A20_SHIFT;
    if (a20 < CBI_A20_ALREADY_ON || a20 > CBI_A20_PORT92)
        FAIL("a20_method");
    if (bi->reserved0 || !all_zero(bi->reserved1, sizeof(bi->reserved1)))
        FAIL("reserved_nonzero");
    if (bi->memmap_source != CBI_MEMMAP_E820)
        FAIL("memmap_source");
    if (bi->input_policy > CBI_INPUT_FIRMWARE)
        FAIL("input_policy");
    if (bi->flags & CBI_F_INPUT_FORCED) {
        uint32_t need = CBI_F_SMBIOS_QEMU | CBI_F_TEST_REQUEST;
        if ((bi->flags & need) != need || bi->input_policy != CBI_INPUT_FIRMWARE)
            FAIL("input_forced_invariants");
    }
    if (bi->test_request_len > CIUKI_TEST_REQ_MAX)
        FAIL("test_request_len");
    if (!!(bi->flags & CBI_F_TEST_REQUEST) != (bi->test_request_len != 0))
        FAIL("test_request_flag");
    if (bi->apm >> 16)
        FAIL("apm_high_bits");
    if (bi->loader_start >= bi->loader_end || bi->loader_end > 0x100000u)
        FAIL("loader_extent");
    if (bi->kernel_start < 0x100000u || bi->kernel_start >= bi->kernel_end ||
        bi->kernel_end > 0x1000000u)
        FAIL("kernel_extent");
    if (bi->kernel_entry_phys < bi->kernel_start || bi->kernel_entry_phys >= bi->kernel_end)
        FAIL("kernel_entry");
    if (bi->e820_count == 0 || bi->e820_count > CIUKI_E820_MAX)
        FAIL("e820_count");
    uint64_t prev_end = 0;
    int have_ram = 0;
    for (uint32_t i = 0; i < bi->e820_count; i++) {
        const struct ciuki_e820 *e = &bi->e820[i];
        if (e->length == 0)
            FAIL("e820_zero_length");
        if (e->base + e->length < e->base)
            FAIL("e820_overflow");
        if (e->type == 0)
            FAIL("e820_type");
        if (!(e->ext & 1))
            FAIL("e820_ext_disabled");
        if (i && e->base < prev_end)
            FAIL("e820_overlap_or_unsorted");
        if (e->type == 1) {
            if ((e->base & 0xFFF) || (e->length & 0xFFF))
                FAIL("e820_ram_not_page_aligned");
            have_ram = 1;
        }
        prev_end = e->base + e->length;
    }
    if (!have_ram)
        FAIL("e820_no_ram");
    if (!(bi->flags & CBI_F_TEXT_MODE)) {
        uint32_t bpp = bi->fb_bpp;
        if (!bi->fb_phys || !bi->fb_width || !bi->fb_height)
            FAIL("fb_missing");
        if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32)
            FAIL("fb_bpp");
        uint64_t row = (uint64_t)bi->fb_width * ((bpp + 7) / 8);
        if (bi->fb_pitch < row)
            FAIL("fb_pitch");
        uint64_t end = (uint64_t)bi->fb_phys + (uint64_t)bi->fb_pitch * bi->fb_height;
        if (end > 0x100000000ull)
            FAIL("fb_overflow");
        if (bi->fb_width < 320 || bi->fb_height < 200)
            FAIL("fb_geometry");
        /* The framebuffer must not alias RAM the kernel maps cacheable. */
        if (bi->fb_phys < 0x100000u)
            FAIL("fb_overlaps_low_memory");
        if (bi->fb_phys < bi->kernel_end && end > bi->kernel_start)
            FAIL("fb_overlaps_kernel");
        if (bpp == 8) {
            if (bi->fb_red_size || bi->fb_green_size || bi->fb_blue_size || bi->fb_rsvd_size)
                FAIL("fb_palette_masks");
        } else {
            uint32_t depth = bpp == 15 ? 16 : bpp, used = 0;
            const uint8_t comp[4][2] = {
                { bi->fb_red_size, bi->fb_red_pos }, { bi->fb_green_size, bi->fb_green_pos },
                { bi->fb_blue_size, bi->fb_blue_pos }, { bi->fb_rsvd_size, 0 },
            };
            for (int c = 0; c < 4; c++) {
                uint32_t size = comp[c][0], pos = comp[c][1];
                if (c < 3 && (size == 0 || size > 8))
                    FAIL("fb_mask_size");
                if (c == 3) {
                    /* The reserved component has no position field: it must
                     * fit in the bits the colour components leave free. */
                    uint32_t colour_bits = (uint32_t)bi->fb_red_size + bi->fb_green_size + bi->fb_blue_size;
                    if (size > 8 || colour_bits + size > depth)
                        FAIL("fb_reserved_capacity");
                    continue;
                }
                if (pos + size > depth)
                    FAIL("fb_mask_position");
                uint32_t mask = ((1u << size) - 1u) << pos;
                if (used & mask)
                    FAIL("fb_mask_overlap");
                used |= mask;
            }
        }
    } else if (bi->fb_phys) {
        FAIL("fb_in_text_mode");
    }
    return 0;
}
