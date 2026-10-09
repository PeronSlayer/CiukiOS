/* T0 host tests for kernel library code and the ciuki_boot_info validator.
 * Build: scripts/test/host_kernel_tests.sh
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

/* Kernel string routines collide with libc: rename them. */
#define memcpy k_memcpy
#define memmove k_memmove
#define memset k_memset
#define memcmp k_memcmp
#define strlen k_strlen
#define strncmp k_strncmp
#include "../../src/kernel/lib/string.c"
#undef memcpy
#undef memmove
#undef memset
#undef memcmp
#undef strlen
#undef strncmp

#define __udivdi3 t_udivdi3
#define __umoddi3 t_umoddi3
#define __divdi3 t_divdi3
#define __moddi3 t_moddi3
#include "../../src/kernel/lib/divdi3.c"

#define strlen k_strlen
#include "../../src/kernel/lib/fmt.c"
#undef strlen

#include "../../src/kernel/core/bootinfo.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void test_div(void)
{
    uint64_t vals[] = { 0, 1, 7, 10, 0xFFFFFFFFull, 0x100000000ull, 0x123456789ABCDEFull, ~0ull };
    uint64_t divs[] = { 1, 3, 10, 0xFFFFFFFFull, 0x100000001ull, ~0ull };
    for (unsigned i = 0; i < sizeof(vals) / 8; i++)
        for (unsigned j = 0; j < sizeof(divs) / 8; j++) {
            CHECK(t_udivdi3(vals[i], divs[j]) == vals[i] / divs[j]);
            CHECK(t_umoddi3(vals[i], divs[j]) == vals[i] % divs[j]);
        }
    CHECK(t_divdi3(-7, 2) == -3);
    CHECK(t_moddi3(-7, 2) == -1);
    CHECK(t_divdi3(7, -2) == -3);
}

static void test_fmt(void)
{
    char b[64];
    ksnprintf(b, sizeof b, "%08x|%u|%d|%s|%llu", 0xABCu, 42u, -5, "x", 12345678901ull);
    CHECK(strcmp(b, "00000abc|42|-5|x|12345678901") == 0);
    int n = ksnprintf(b, 8, "%s", "0123456789");
    CHECK(n == 10 && strcmp(b, "0123456") == 0);
    ksnprintf(b, sizeof b, "%016llx", 0xFFFFFFFFFFFFF000ull);
    CHECK(strcmp(b, "fffffffffffff000") == 0);
}

static void good_info(struct ciuki_boot_info *bi)
{
    memset(bi, 0, sizeof(*bi));
    bi->magic = CIUKI_BOOT_INFO_MAGIC;
    bi->version = 1;
    bi->size = CIUKI_BOOT_INFO_SIZE;
    bi->flags = CBI_A20_INT15 << CBI_F_A20_SHIFT;
    bi->memmap_source = 1;
    bi->loader_start = 0x8000;
    bi->loader_end = 0x20000;
    bi->kernel_start = 0x100000;
    bi->kernel_end = 0x13A000;
    bi->kernel_entry_phys = 0x100000;
    bi->e820_count = 3;
    bi->e820[0] = (struct ciuki_e820){ 0, 0x9F000, 1, 1 };
    bi->e820[1] = (struct ciuki_e820){ 0xF0000, 0x10000, 2, 1 };
    bi->e820[2] = (struct ciuki_e820){ 0x100000, 0x7EF0000, 1, 1 };
    bi->fb_phys = 0xFD000000;
    bi->fb_width = 1024;
    bi->fb_height = 768;
    bi->fb_bpp = 32;
    bi->fb_pitch = 4096;
    bi->fb_red_size = 8; bi->fb_red_pos = 16;
    bi->fb_green_size = 8; bi->fb_green_pos = 8;
    bi->fb_blue_size = 8; bi->fb_blue_pos = 0;
    bi->fb_rsvd_size = 8;
}

static void test_bootinfo(void)
{
    struct ciuki_boot_info bi;
    const char *why;
    good_info(&bi);
    CHECK(bootinfo_validate(&bi, &why) == 0);
#define NEG(stmt, expect) do { good_info(&bi); stmt; int r = bootinfo_validate(&bi, &why); \
        CHECK(r != 0); CHECK(strcmp(why, expect) == 0); } while (0)
    NEG(bi.version = 2, "version");
    NEG(bi.size = 0x1000, "size");
    NEG(bi.flags |= 1u << 20, "undefined_flag_bits");
    NEG(bi.flags &= ~CBI_F_A20_MASK, "a20_method");
    NEG(bi.reserved1[3] = 1, "reserved_nonzero");
    NEG(bi.e820_count = 129, "e820_count");
    NEG(bi.e820[0].length = 0, "e820_zero_length");
    NEG((bi.e820[0].base = 0xFFFFFFFFFFFFF000ull, bi.e820[0].length = 0x2000), "e820_overflow");
    NEG(bi.e820[1].base = 0x10000, "e820_overlap_or_unsorted");
    NEG(bi.e820[2].ext = 0, "e820_ext_disabled");
    NEG(bi.e820[2].length = 0x7EF0001, "e820_ram_not_page_aligned");
    NEG(bi.kernel_entry_phys = 0x200000, "kernel_entry");
    NEG(bi.kernel_end = 0x2000000, "kernel_extent");
    NEG(bi.fb_pitch = 1000, "fb_pitch");
    NEG(bi.fb_bpp = 12, "fb_bpp");
    NEG(bi.flags |= CBI_F_TEXT_MODE, "fb_in_text_mode");
    NEG(bi.test_request_len = 65, "test_request_len");
    NEG(bi.test_request_len = 5, "test_request_flag");
    NEG((bi.flags |= CBI_F_INPUT_FORCED, bi.input_policy = 1), "input_forced_invariants");
    NEG(bi.apm = 0x10102, "apm_high_bits");
    NEG(bi.fb_width = 300, "fb_geometry");
    NEG(bi.fb_green_pos = 12, "fb_mask_overlap");
    NEG(bi.fb_blue_size = 0, "fb_mask_size");
    NEG(bi.fb_red_pos = 25, "fb_mask_position");
    NEG((bi.fb_bpp = 8, bi.fb_pitch = 1024), "fb_palette_masks");
    good_info(&bi);
    bi.fb_bpp = 16; bi.fb_pitch = 2048;
    bi.fb_red_size = 5; bi.fb_red_pos = 11; bi.fb_green_size = 6; bi.fb_green_pos = 5;
    bi.fb_blue_size = 5; bi.fb_blue_pos = 0; bi.fb_rsvd_size = 0;
    CHECK(bootinfo_validate(&bi, &why) == 0);
    good_info(&bi);
    bi.flags |= CBI_F_TEXT_MODE;
    bi.fb_phys = 0;
    CHECK(bootinfo_validate(&bi, &why) == 0);
}

int main(void)
{
    test_div();
    test_fmt();
    test_bootinfo();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("kernel host tests: PASS\n");
    return 0;
}
