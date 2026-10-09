/* ciuki_boot_info version 1: loader -> Ciuki VMM handoff.
 * Frozen layout; the authority is docs/design/boot-memory.md.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef CIUKI_BOOT_INFO_H
#define CIUKI_BOOT_INFO_H

#include <stdint.h>

#define CIUKI_BOOT_MAGIC_EAX   0x4B554943u /* "CIUK" in EAX at kernel entry */
#define CIUKI_BOOT_INFO_MAGIC  0x31494243u /* "CBI1" */
#define CIUKI_BOOT_INFO_VER    1u
#define CIUKI_BOOT_INFO_SIZE   0x10F0u
#define CIUKI_E820_MAX         128u
#define CIUKI_TEST_REQ_MAX     64u

/* flags */
#define CBI_F_SAFE_MODE        (1u << 0)
#define CBI_F_SERIAL_LOG       (1u << 1)
#define CBI_F_EDID_VALID       (1u << 2)
#define CBI_F_VBE_CTRL_VALID   (1u << 3)
#define CBI_F_TEXT_MODE        (1u << 4)
#define CBI_F_SMBIOS_QEMU      (1u << 5)
#define CBI_F_TEST_REQUEST     (1u << 6)
#define CBI_F_INPUT_FORCED     (1u << 7)
#define CBI_F_A20_SHIFT        8u
#define CBI_F_A20_MASK         (7u << CBI_F_A20_SHIFT)
#define CBI_F_DEFINED_MASK     0x000007FFu

#define CBI_A20_ALREADY_ON     1u
#define CBI_A20_INT15          2u
#define CBI_A20_KBC            3u
#define CBI_A20_PORT92         4u

#define CBI_MEMMAP_E820        1u
#define CBI_INPUT_NATIVE       0u
#define CBI_INPUT_FIRMWARE     1u

#define CBI_E820_RAM           1u

struct ciuki_e820 {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t ext;
} __attribute__((packed));

struct ciuki_boot_info {
    uint32_t magic;                 /* 0x000 */
    uint16_t version;               /* 0x004 */
    uint16_t size;                  /* 0x006 */
    uint32_t flags;                 /* 0x008 */
    uint8_t  boot_drive;            /* 0x00C */
    uint8_t  memmap_source;         /* 0x00D */
    uint8_t  input_policy;          /* 0x00E */
    uint8_t  reserved0;             /* 0x00F */
    uint32_t partition_lba;         /* 0x010 */
    uint32_t loader_start;          /* 0x014 */
    uint32_t loader_end;            /* 0x018 */
    uint32_t kernel_start;          /* 0x01C */
    uint32_t kernel_end;            /* 0x020 */
    uint32_t kernel_entry_phys;     /* 0x024 */
    uint32_t e820_count;            /* 0x028 */
    uint32_t e801_low_kib;          /* 0x02C */
    uint32_t e801_high_64k;         /* 0x030 */
    uint32_t int88_kib;             /* 0x034 */
    uint32_t uart_base;             /* 0x038 */
    uint32_t uart_divisor;          /* 0x03C */
    uint32_t pci_bios;              /* 0x040 */
    uint32_t apm;                   /* 0x044 */
    uint32_t acpi_rsdp_phys;        /* 0x048 */
    uint32_t smbios_entry_phys;     /* 0x04C */
    uint32_t fb_phys;               /* 0x050 */
    uint32_t fb_pitch;              /* 0x054 */
    uint16_t fb_width;              /* 0x058 */
    uint16_t fb_height;             /* 0x05A */
    uint8_t  fb_bpp;                /* 0x05C */
    uint8_t  fb_red_size;           /* 0x05D */
    uint8_t  fb_red_pos;            /* 0x05E */
    uint8_t  fb_green_size;         /* 0x05F */
    uint8_t  fb_green_pos;          /* 0x060 */
    uint8_t  fb_blue_size;          /* 0x061 */
    uint8_t  fb_blue_pos;           /* 0x062 */
    uint8_t  fb_rsvd_size;          /* 0x063 */
    uint16_t vbe_mode;              /* 0x064 */
    uint16_t test_request_len;      /* 0x066 */
    char     test_request[64];      /* 0x068 */
    char     options[128];          /* 0x0A8 */
    uint8_t  edid[128];             /* 0x128 */
    uint8_t  edd[66];               /* 0x1A8 */
    uint8_t  reserved1[6];          /* 0x1EA */
    uint8_t  vbe_ctrl[512];         /* 0x1F0 */
    uint8_t  vbe_mode_info[256];    /* 0x3F0 */
    struct ciuki_e820 e820[128];    /* 0x4F0 */
} __attribute__((packed));

#define CBI_ASSERT_OFF(f, o) \
    _Static_assert(__builtin_offsetof(struct ciuki_boot_info, f) == (o), #f)
CBI_ASSERT_OFF(flags, 0x008);
CBI_ASSERT_OFF(partition_lba, 0x010);
CBI_ASSERT_OFF(kernel_entry_phys, 0x024);
CBI_ASSERT_OFF(uart_base, 0x038);
CBI_ASSERT_OFF(fb_phys, 0x050);
CBI_ASSERT_OFF(fb_bpp, 0x05C);
CBI_ASSERT_OFF(vbe_mode, 0x064);
CBI_ASSERT_OFF(test_request, 0x068);
CBI_ASSERT_OFF(options, 0x0A8);
CBI_ASSERT_OFF(edid, 0x128);
CBI_ASSERT_OFF(edd, 0x1A8);
CBI_ASSERT_OFF(vbe_ctrl, 0x1F0);
CBI_ASSERT_OFF(vbe_mode_info, 0x3F0);
CBI_ASSERT_OFF(e820, 0x4F0);
_Static_assert(sizeof(struct ciuki_e820) == 24, "e820 entry");
_Static_assert(sizeof(struct ciuki_boot_info) == CIUKI_BOOT_INFO_SIZE, "size");

#endif
