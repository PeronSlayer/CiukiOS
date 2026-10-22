/* f9-01: Setup-mode recognition from the boot options (plan cd-setup-plan.md
 * section 3.2) and the Setup memory refusal (D11). The parsers are pure so
 * the host suite (scripts/test/cd_media_tests.py) checks the exact rules.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SETUP_MODE_H
#define CIUKI_SETUP_MODE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ciuki/boot_info.h>

/* The lead-reserved BOOT.CFG token (docs/design/boot-memory.md grammar). */
#define SETUP_MODE_TOKEN "setup=1"
/* Plan 3.2: "Less than 128 MiB usable RAM (E820 type 1 sum)" is refused.
 * A 128 MB machine never reports 128 MiB of type-1 memory: the 384 KiB
 * legacy hole and the firmware/ACPI reservations are type 2 (QEMU -m 512
 * reports 523,260 KiB, 1,028 KiB below the installed size: validation
 * record 2026-10-11-integrated-image-f0-regression.md), and chipsets with a
 * shared frame buffer report less. The threshold therefore allows 8 MiB for
 * firmware reservations (lead decision requested in the f9-01 report). */
#define SETUP_MIN_USABLE_KIB (120u * 1024u)
#define SETUP_MEMORY_TEXT "CiukiOS Setup needs at least 128 MB of memory"
#define SETUP_MEDIA_TEXT "Insert the CiukiOS Setup CD and restart"

/* Exact token match in a NUL-terminated or size-bounded option string;
 * tokens are separated by spaces, CR or LF, as in the loader's parser. */
static inline bool setup_option_token(const char *options, size_t size, const char *token)
{
    size_t length = 0;
    while (token[length]) length++;
    for (size_t i = 0; i < size && options[i];) {
        while (i < size && (options[i] == ' ' || options[i] == '\r' || options[i] == '\n')) i++;
        size_t start = i;
        while (i < size && options[i] && options[i] != ' ' && options[i] != '\r' && options[i] != '\n') i++;
        if (i - start == length) {
            size_t k = 0;
            while (k < length && options[start + k] == token[k]) k++;
            if (k == length) return true;
        }
    }
    return false;
}

/* CBI_F_TEST_REQUEST boots never enter Setup mode (plan 3.2). */
static inline bool setup_mode_parse(uint32_t boot_flags, const char *options, size_t size)
{
    return !(boot_flags & CBI_F_TEST_REQUEST) && setup_option_token(options, size, SETUP_MODE_TOKEN);
}

/* Sum of E820 type-1 lengths in KiB; entries are the loader's normalized
 * copy (boot-memory.md: type-1 ranges already rounded inward to 4 KiB). */
static inline uint64_t setup_usable_kib(const struct ciuki_e820 *map, uint32_t count)
{
    uint64_t kib = 0;
    if (count > CIUKI_E820_MAX) count = CIUKI_E820_MAX;
    for (uint32_t i = 0; i < count; i++)
        if (map[i].type == CBI_E820_RAM) kib += map[i].length >> 10;
    return kib;
}

#ifndef FS_HOST
/* Kernel flag, set once from g_boot before storage_init. */
void setup_mode_init(void);
bool setup_mode(void);
/* In Setup mode with too little memory: console text, serial record, and
 * true (the caller stops before any driver or process). False otherwise. */
bool setup_memory_refused(void);
/* The running kernel's build identity, compared with \SYSTEM\VMM.ELF on
 * the boot image (plan 3.3). */
extern const char setup_build_identity[];
uint32_t setup_build_identity_length(void);
#endif
#endif
