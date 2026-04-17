#include <efi.h>
#include <efilib.h>
#include "elf64.h"
#include "../proto/bootinfo.h"
#include "../proto/handoff.h"
#include "../proto/bootcfg.h"
#include "../proto/video_limits.h"

#define PAGE_SIZE           4096ULL
#define LARGE_PAGE_SIZE     0x200000ULL
#define LOW4G_MAX_ADDR      0xFFFFFFFFULL

#define PT_PRESENT          0x001ULL
#define PT_RW               0x002ULL
#define PT_PS               0x080ULL

typedef void (*stage_entry_t)(void);

__attribute__((noreturn))
void efi_handoff(
    UINT64 new_cr3,
    stage_entry_t entry,
    boot_info_t *boot_info,
    handoff_v0_t *handoff
);

static UINT64 align_down(UINT64 value, UINT64 align) {
    return value & ~(align - 1);
}

static UINT64 align_up(UINT64 value, UINT64 align) {
    return (value + align - 1) & ~(align - 1);
}

static UINTN bytes_to_pages(UINTN size) {
    return (size + PAGE_SIZE - 1) / PAGE_SIZE;
}

static VOID mem_zero(VOID *buffer, UINTN size) {
    volatile UINT8 *dst = (volatile UINT8 *)buffer;
    for (UINTN i = 0; i < size; i++) {
        dst[i] = 0;
    }
}

static VOID mem_copy(VOID *destination, const VOID *source, UINTN size) {
    volatile UINT8 *dst = (volatile UINT8 *)destination;
    const UINT8 *src = (const UINT8 *)source;
    for (UINTN i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

static CHAR16 hex_digit(UINT8 v) {
    return (v < 10) ? (CHAR16)(L'0' + v) : (CHAR16)(L'A' + (v - 10));
}

static VOID print_hex8(UINT8 v) {
    CHAR16 s[3];
    s[0] = hex_digit((v >> 4) & 0xF);
    s[1] = hex_digit(v & 0xF);
    s[2] = L'\0';
    Print(s);
}

static VOID dump_kernel_entry_bytes(stage_entry_t entry) {
    UINT8 *p = (UINT8 *)(UINTN)entry;

    Print(L"entry bytes: ");
    for (UINTN i = 0; i < 16; i++) {
        print_hex8(p[i]);
        Print(L" ");
    }
    Print(L"\r\n");
}

static UINT32 highest_set_bit_plus_one(UINT32 value) {
    UINT32 bits = 0;
    while (value != 0) {
        bits++;
        value >>= 1;
    }
    return bits;
}

static UINT32 gop_detect_bpp(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info) {
    UINT32 max_bits = 0;

    if (!info) {
        return 32;
    }

    if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
        info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
        return 32;
    }

    if (info->PixelFormat == PixelBitMask) {
        UINT32 r = highest_set_bit_plus_one(info->PixelInformation.RedMask);
        UINT32 g = highest_set_bit_plus_one(info->PixelInformation.GreenMask);
        UINT32 b = highest_set_bit_plus_one(info->PixelInformation.BlueMask);
        UINT32 x = highest_set_bit_plus_one(info->PixelInformation.ReservedMask);

        if (r > max_bits) { max_bits = r; }
        if (g > max_bits) { max_bits = g; }
        if (b > max_bits) { max_bits = b; }
        if (x > max_bits) { max_bits = x; }

        if (max_bits <= 16U && max_bits != 0U) {
            return 16;
        }
        if (max_bits <= 24U && max_bits != 0U) {
            return 24;
        }
        return 32;
    }

    return 32;
}

static CHAR16 ascii_upper_char16(CHAR16 ch) {
    if (ch >= L'a' && ch <= L'z') {
        return (CHAR16)(ch - (L'a' - L'A'));
    }
    return ch;
}

static BOOLEAN char16_ends_with_com(const CHAR16 *name) {
    UINTN len = 0;
    while (name[len] != 0) {
        len++;
    }

    if (len < 4) {
        return FALSE;
    }

    return ascii_upper_char16(name[len - 4]) == L'.' &&
           ascii_upper_char16(name[len - 3]) == L'C' &&
           ascii_upper_char16(name[len - 2]) == L'O' &&
           ascii_upper_char16(name[len - 1]) == L'M';
}

static BOOLEAN char16_to_ascii_upper(const CHAR16 *src, CHAR8 *dst, UINTN dst_size) {
    UINTN i = 0;

    if (!src || !dst || dst_size == 0) {
        return FALSE;
    }

    while (src[i] != 0) {
        CHAR16 ch = ascii_upper_char16(src[i]);
        if (ch > 0x7F || (i + 1) >= dst_size) {
            return FALSE;
        }
        dst[i] = (CHAR8)ch;
        i++;
    }

    dst[i] = '\0';
    return i > 0;
}

static BOOLEAN ascii_eq(const CHAR8 *a, const CHAR8 *b) {
    UINTN i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return FALSE;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static BOOLEAN build_ciukios_path(const CHAR16 *file_name, CHAR16 *out_path, UINTN out_cap) {
    static const CHAR16 prefix[] = L"\\EFI\\CiukiOS\\";
    UINTN i = 0;
    UINTN j = 0;

    if (!file_name || !out_path || out_cap == 0) {
        return FALSE;
    }

    while (prefix[i] != 0) {
        if ((j + 1) >= out_cap) {
            return FALSE;
        }
        out_path[j++] = prefix[i++];
    }

    i = 0;
    while (file_name[i] != 0) {
        if ((j + 1) >= out_cap) {
            return FALSE;
        }
        out_path[j++] = file_name[i++];
    }

    out_path[j] = 0;
    return TRUE;
}

static EFI_STATUS alloc_pages_below_4g(
    UINTN pages,
    EFI_MEMORY_TYPE type,
    EFI_PHYSICAL_ADDRESS *addr_out
) {
    EFI_PHYSICAL_ADDRESS addr = LOW4G_MAX_ADDR;
    EFI_STATUS status = uefi_call_wrapper(
        BS->AllocatePages,
        4,
        AllocateMaxAddress,
        type,
        pages,
        &addr
    );
    if (EFI_ERROR(status)) {
        return status;
    }

    *addr_out = addr;
    return EFI_SUCCESS;
}

static EFI_STATUS alloc_staging_pages_preferred(
    UINTN bytes,
    EFI_MEMORY_TYPE type,
    EFI_PHYSICAL_ADDRESS *addr_out
) {
    static const UINT64 preferred_bases[] = {
        384ULL * 1024ULL * 1024ULL,
        320ULL * 1024ULL * 1024ULL,
        256ULL * 1024ULL * 1024ULL,
        192ULL * 1024ULL * 1024ULL,
        128ULL * 1024ULL * 1024ULL,
        64ULL  * 1024ULL * 1024ULL,
    };
    UINTN pages = bytes_to_pages(bytes);

    for (UINTN i = 0; i < sizeof(preferred_bases) / sizeof(preferred_bases[0]); i++) {
        EFI_PHYSICAL_ADDRESS addr = align_down(preferred_bases[i], PAGE_SIZE);
        EFI_STATUS status = uefi_call_wrapper(
            BS->AllocatePages,
            4,
            AllocateAddress,
            type,
            pages,
            &addr
        );
        if (!EFI_ERROR(status)) {
            *addr_out = addr;
            return EFI_SUCCESS;
        }
    }

    return EFI_NOT_FOUND;
}

static EFI_STATUS build_bootstrap_paging(UINT64 *cr3_out) {
    EFI_STATUS status;
    EFI_PHYSICAL_ADDRESS tables_base = 0;
    UINT64 *pml4;
    UINT64 *pdpt;
    UINT64 *pd0;
    UINT64 *pd1;
    UINT64 *pd2;
    UINT64 *pd3;

    status = alloc_pages_below_4g(6, EfiLoaderData, &tables_base);
    if (EFI_ERROR(status)) {
        return status;
    }

    mem_zero((VOID *)(UINTN)tables_base, 6 * PAGE_SIZE);

    pml4 = (UINT64 *)(UINTN)(tables_base + 0 * PAGE_SIZE);
    pdpt = (UINT64 *)(UINTN)(tables_base + 1 * PAGE_SIZE);
    pd0  = (UINT64 *)(UINTN)(tables_base + 2 * PAGE_SIZE);
    pd1  = (UINT64 *)(UINTN)(tables_base + 3 * PAGE_SIZE);
    pd2  = (UINT64 *)(UINTN)(tables_base + 4 * PAGE_SIZE);
    pd3  = (UINT64 *)(UINTN)(tables_base + 5 * PAGE_SIZE);

    pml4[0] = (tables_base + 1 * PAGE_SIZE) | PT_PRESENT | PT_RW;

    pdpt[0] = (tables_base + 2 * PAGE_SIZE) | PT_PRESENT | PT_RW;
    pdpt[1] = (tables_base + 3 * PAGE_SIZE) | PT_PRESENT | PT_RW;
    pdpt[2] = (tables_base + 4 * PAGE_SIZE) | PT_PRESENT | PT_RW;
    pdpt[3] = (tables_base + 5 * PAGE_SIZE) | PT_PRESENT | PT_RW;

    for (UINTN i = 0; i < 512; i++) {
        pd0[i] = (0ULL * 512ULL + i) * LARGE_PAGE_SIZE | PT_PRESENT | PT_RW | PT_PS;
        pd1[i] = (1ULL * 512ULL + i) * LARGE_PAGE_SIZE | PT_PRESENT | PT_RW | PT_PS;
        pd2[i] = (2ULL * 512ULL + i) * LARGE_PAGE_SIZE | PT_PRESENT | PT_RW | PT_PS;
        pd3[i] = (3ULL * 512ULL + i) * LARGE_PAGE_SIZE | PT_PRESENT | PT_RW | PT_PS;
    }

    *cr3_out = (UINT64)tables_base;
    return EFI_SUCCESS;
}

static EFI_STATUS open_root_dir(EFI_HANDLE image, EFI_FILE_PROTOCOL **root) {
    EFI_STATUS status;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;

    status = uefi_call_wrapper(
        BS->HandleProtocol,
        3,
        image,
        &LoadedImageProtocol,
        (void **)&loaded_image
    );
    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(
        BS->HandleProtocol,
        3,
        loaded_image->DeviceHandle,
        &FileSystemProtocol,
        (void **)&fs
    );
    if (EFI_ERROR(status)) {
        return status;
    }

    if (!fs) {
        Print(L"Error: filesystem protocol not found\r\n");
        return EFI_NOT_FOUND;
    }

    return uefi_call_wrapper(fs->OpenVolume, 2, fs, root);
}

static EFI_STATUS open_ciukios_dir(EFI_HANDLE image, EFI_FILE_PROTOCOL **dir) {
    EFI_STATUS status;
    EFI_FILE_PROTOCOL *root = NULL;

    status = open_root_dir(image, &root);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(
        root->Open,
        5,
        root,
        dir,
        L"\\EFI\\CiukiOS",
        EFI_FILE_MODE_READ,
        0
    );

    uefi_call_wrapper(root->Close, 1, root);
    return status;
}

static EFI_STATUS open_boot_block_io(EFI_HANDLE image, EFI_BLOCK_IO_PROTOCOL **block_io) {
    EFI_STATUS status;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;

    status = uefi_call_wrapper(
        BS->HandleProtocol,
        3,
        image,
        &LoadedImageProtocol,
        (void **)&loaded_image
    );
    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(
        BS->HandleProtocol,
        3,
        loaded_image->DeviceHandle,
        &BlockIoProtocol,
        (void **)block_io
    );
    return status;
}

static EFI_STATUS read_file(
    EFI_HANDLE image,
    CHAR16 *path,
    VOID **buffer,
    UINTN *buffer_size
) {
    EFI_STATUS status;
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_FILE_PROTOCOL *file = NULL;
    EFI_FILE_INFO *file_info = NULL;

    status = open_root_dir(image, &root);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(
        root->Open,
        5,
        root,
        &file,
        path,
        EFI_FILE_MODE_READ,
        0
    );
    if (EFI_ERROR(status)) {
        return status;
    }

    file_info = LibFileInfo(file);
    if (!file_info) {
        uefi_call_wrapper(file->Close, 1, file);
        return EFI_NOT_FOUND;
    }

    *buffer_size = (UINTN)file_info->FileSize;

    status = uefi_call_wrapper(
        BS->AllocatePool,
        3,
        EfiLoaderData,
        *buffer_size,
        buffer
    );
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(BS->FreePool, 1, file_info);
        uefi_call_wrapper(file->Close, 1, file);
        return status;
    }

    status = uefi_call_wrapper(file->Read, 3, file, buffer_size, *buffer);

    uefi_call_wrapper(BS->FreePool, 1, file_info);
    uefi_call_wrapper(file->Close, 1, file);

    return status;
}

static BOOLEAN handoff_add_com_entry(
    handoff_v0_t *handoff,
    const CHAR8 *name,
    EFI_PHYSICAL_ADDRESS base,
    UINTN size
) {
    UINTN idx = (UINTN)handoff->com_count;
    handoff_com_entry_t *entry;
    UINTN i = 0;

    if (idx >= HANDOFF_COM_MAX) {
        return FALSE;
    }

    entry = &handoff->com_entries[idx];
    mem_zero(entry, sizeof(*entry));

    while (name[i] != '\0' && i < HANDOFF_COM_NAME_MAX) {
        entry->name[i] = (char)name[i];
        i++;
    }
    entry->name[i] = '\0';
    entry->phys_base = (UINT64)base;
    entry->size = (UINT64)size;

    handoff->com_count = (UINT64)(idx + 1);

    if (handoff->com_phys_base == 0 || ascii_eq(name, (const CHAR8 *)"INIT.COM")) {
        handoff->com_phys_base = (UINT64)base;
        handoff->com_phys_size = (UINT64)size;
    }

    return TRUE;
}

static EFI_STATUS load_com_catalog(EFI_HANDLE image, handoff_v0_t *handoff) {
    EFI_STATUS status;
    EFI_FILE_PROTOCOL *dir = NULL;
    UINT8 info_buffer[SIZE_OF_EFI_FILE_INFO + 512 * sizeof(CHAR16)];

    handoff->com_phys_base = 0;
    handoff->com_phys_size = 0;
    handoff->com_count = 0;
    mem_zero(handoff->com_entries, sizeof(handoff->com_entries));

    status = open_ciukios_dir(image, &dir);
    if (EFI_ERROR(status)) {
        Print(L"Warning: cannot open \\EFI\\CiukiOS (%r)\r\n", status);
        return EFI_SUCCESS;
    }

    for (;;) {
        UINTN info_size = sizeof(info_buffer);
        EFI_FILE_INFO *info;

        status = uefi_call_wrapper(dir->Read, 3, dir, &info_size, info_buffer);
        if (EFI_ERROR(status)) {
            Print(L"Warning: directory read failed (%r)\r\n", status);
            break;
        }
        if (info_size == 0) {
            break;
        }

        info = (EFI_FILE_INFO *)(VOID *)info_buffer;
        if ((info->Attribute & EFI_FILE_DIRECTORY) != 0) {
            continue;
        }
        if (!char16_ends_with_com(info->FileName)) {
            continue;
        }

        if (handoff->com_count >= HANDOFF_COM_MAX) {
            Print(L"Warning: COM catalog full (max %d entries)\r\n", HANDOFF_COM_MAX);
            break;
        }

        {
            CHAR8 com_name[HANDOFF_COM_NAME_MAX + 1];
            CHAR16 path[260];
            VOID *com_buffer = NULL;
            UINTN com_size = 0;
            EFI_PHYSICAL_ADDRESS com_addr = 0;
            EFI_STATUS load_status;

            if (!char16_to_ascii_upper(info->FileName, com_name, sizeof(com_name))) {
                Print(L"Warning: skip COM with unsupported name\r\n");
                continue;
            }

            if (!build_ciukios_path(info->FileName, path, sizeof(path) / sizeof(path[0]))) {
                Print(L"Warning: COM path too long, skipping\r\n");
                continue;
            }

            load_status = read_file(image, path, &com_buffer, &com_size);
            if (EFI_ERROR(load_status) || com_size == 0) {
                Print(L"Warning: cannot load COM %a (%r)\r\n", com_name, load_status);
                continue;
            }

            load_status = alloc_pages_below_4g(bytes_to_pages(com_size), EfiLoaderCode, &com_addr);
            if (EFI_ERROR(load_status)) {
                Print(L"Warning: cannot allocate COM memory for %a (%r)\r\n", com_name, load_status);
                uefi_call_wrapper(BS->FreePool, 1, com_buffer);
                continue;
            }

            mem_copy((VOID *)(UINTN)com_addr, com_buffer, com_size);
            uefi_call_wrapper(BS->FreePool, 1, com_buffer);

            if (!handoff_add_com_entry(handoff, com_name, com_addr, com_size)) {
                Print(L"Warning: failed to register COM %a\r\n", com_name);
                continue;
            }

            Print(L"COM loaded: %a (%d bytes) @ 0x%lx\r\n", com_name, com_size, com_addr);
        }
    }

    uefi_call_wrapper(dir->Close, 1, dir);

    if (handoff->com_count == 0) {
        Print(L"No COM programs found in \\EFI\\CiukiOS\r\n");
    } else {
        Print(L"COM catalog ready: %d entries\r\n", (UINTN)handoff->com_count);
    }

    return EFI_SUCCESS;
}

typedef struct vmode_config {
    UINT32 mode_id;     /* from "mode=N", 0xFFFFFFFF if absent */
    UINT32 width;       /* from "width=N", 0 if absent */
    UINT32 height;      /* from "height=N", 0 if absent */
} vmode_config_t;

static UINT32 parse_decimal(const UINT8 *p, UINTN len) {
    UINT32 val = 0;
    for (UINTN i = 0; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') break;
        val = val * 10 + (p[i] - '0');
    }
    return val;
}

static VOID loader_memset(VOID *dst, UINT8 value, UINTN size) {
    UINT8 *p = (UINT8 *)dst;
    for (UINTN i = 0; i < size; i++) {
        p[i] = value;
    }
}

static VOID loader_memcpy(VOID *dst, const VOID *src, UINTN size) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    for (UINTN i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

static UINT8 cmos_read_byte(UINT8 idx) {
    UINT8 value;
    __asm__ volatile ("outb %0, %1" : : "a"((UINT8)(0x80U | (idx & 0x7FU))), "Nd"((UINT16)0x70));
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"((UINT16)0x71));
    return value;
}

static VOID cmos_write_byte(UINT8 idx, UINT8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"((UINT8)(0x80U | (idx & 0x7FU))), "Nd"((UINT16)0x70));
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"((UINT16)0x71));
}

static BOOLEAN bootcfg_load(bootcfg_data_t *cfg) {
    if (!cfg) {
        return FALSE;
    }

    for (UINTN i = 0; i < BOOTCFG_CMOS_SIZE; i++) {
        ((UINT8 *)cfg)[i] = cmos_read_byte((UINT8)(BOOTCFG_CMOS_BASE + i));
    }

    return bootcfg_valid(cfg) ? TRUE : FALSE;
}

static __attribute__((unused)) BOOLEAN bootcfg_store(const bootcfg_data_t *cfg) {
    bootcfg_data_t tmp;

    if (!cfg) {
        return FALSE;
    }

    loader_memcpy(&tmp, cfg, BOOTCFG_CMOS_SIZE);
    bootcfg_finalize(&tmp);
    for (UINTN i = 0; i < BOOTCFG_CMOS_SIZE; i++) {
        cmos_write_byte((UINT8)(BOOTCFG_CMOS_BASE + i), ((UINT8 *)&tmp)[i]);
    }

    return TRUE;
}

static __attribute__((unused)) VOID bootcfg_clear(VOID) {
    for (UINTN i = 0; i < BOOTCFG_CMOS_SIZE; i++) {
        cmos_write_byte((UINT8)(BOOTCFG_CMOS_BASE + i), 0U);
    }
}

static BOOLEAN vmode_cfg_valid(const vmode_config_t *cfg) {
    if (!cfg) {
        return FALSE;
    }
    if (cfg->mode_id != BOOTCFG_MODE_ID_NONE) {
        return TRUE;
    }
    return (cfg->width != 0U && cfg->height != 0U) ? TRUE : FALSE;
}

static void load_vmode_config(EFI_HANDLE image, vmode_config_t *cfg) {
    VOID *buf = NULL;
    UINTN size = 0;
    EFI_STATUS st;
    const UINT8 *data;
    UINTN i;

    cfg->mode_id = 0xFFFFFFFFU;
    cfg->width = 0;
    cfg->height = 0;

    st = read_file(image, L"\\EFI\\CiukiOS\\VMODE.CFG", &buf, &size);
    if (EFI_ERROR(st) || !buf || size == 0) {
        return; /* file absent or empty — use defaults */
    }

    data = (const UINT8 *)buf;
    i = 0;
    while (i < size) {
        /* skip whitespace/newlines */
        while (i < size && (data[i] == ' ' || data[i] == '\t' ||
               data[i] == '\r' || data[i] == '\n')) {
            i++;
        }
        if (i >= size) break;

        /* find end of line */
        UINTN line_start = i;
        while (i < size && data[i] != '\r' && data[i] != '\n') {
            i++;
        }
        UINTN line_len = i - line_start;
        const UINT8 *line = &data[line_start];

        /* parse "key=value" */
        if (line_len > 5 && line[0] == 'm' && line[1] == 'o' &&
            line[2] == 'd' && line[3] == 'e' && line[4] == '=') {
            cfg->mode_id = parse_decimal(line + 5, line_len - 5);
        } else if (line_len > 6 && line[0] == 'w' && line[1] == 'i' &&
                   line[2] == 'd' && line[3] == 't' && line[4] == 'h' &&
                   line[5] == '=') {
            cfg->width = parse_decimal(line + 6, line_len - 6);
        } else if (line_len > 7 && line[0] == 'h' && line[1] == 'e' &&
                   line[2] == 'i' && line[3] == 'g' && line[4] == 'h' &&
                   line[5] == 't' && line[6] == '=') {
            cfg->height = parse_decimal(line + 7, line_len - 7);
        }
    }

    uefi_call_wrapper(BS->FreePool, 1, buf);

    Print(L"VMODE.CFG: mode=%d width=%d height=%d\r\n",
          cfg->mode_id, cfg->width, cfg->height);
}

static EFI_STATUS load_disk_cache(EFI_HANDLE image, handoff_v0_t *handoff) {
    EFI_STATUS status;
    EFI_BLOCK_IO_PROTOCOL *block_io = NULL;
    EFI_BLOCK_IO_MEDIA *media;
    UINT64 total_lbas;
    UINT64 chosen_lbas;
    UINT64 cache_bytes_u64;
    UINTN cache_bytes;
    EFI_PHYSICAL_ADDRESS cache_addr;

    handoff->disk_cache_phys_base = 0;
    handoff->disk_cache_byte_size = 0;
    handoff->disk_cache_lba_start = 0;
    handoff->disk_cache_lba_count = 0;
    handoff->disk_cache_block_size = 0;
    handoff->disk_cache_flags = 0;

    status = open_boot_block_io(image, &block_io);
    if (EFI_ERROR(status) || !block_io || !block_io->Media) {
        Print(L"Warning: BlockIO not available (%r)\r\n", status);
        return EFI_SUCCESS;
    }

    media = block_io->Media;
    if (!media->MediaPresent || media->BlockSize == 0) {
        Print(L"Warning: boot media not present for disk cache\r\n");
        return EFI_SUCCESS;
    }

    total_lbas = media->LastBlock + 1ULL;
    chosen_lbas = total_lbas;

    if (chosen_lbas > (HANDOFF_DISK_CACHE_MAX_BYTES / media->BlockSize)) {
        chosen_lbas = (HANDOFF_DISK_CACHE_MAX_BYTES / media->BlockSize);
    }
    if (chosen_lbas == 0) {
        Print(L"Warning: disk cache computed as zero blocks\r\n");
        return EFI_SUCCESS;
    }

    cache_bytes_u64 = chosen_lbas * (UINT64)media->BlockSize;
    if (cache_bytes_u64 > 0xFFFFFFFFULL) {
        Print(L"Warning: disk cache too large for current build\r\n");
        return EFI_SUCCESS;
    }

    cache_bytes = (UINTN)cache_bytes_u64;
    status = alloc_pages_below_4g(bytes_to_pages(cache_bytes), EfiLoaderData, &cache_addr);
    if (EFI_ERROR(status)) {
        Print(L"Warning: cannot allocate disk cache (%r)\r\n", status);
        return EFI_SUCCESS;
    }

    mem_zero((VOID *)(UINTN)cache_addr, bytes_to_pages(cache_bytes) * PAGE_SIZE);

    status = uefi_call_wrapper(
        block_io->ReadBlocks,
        5,
        block_io,
        media->MediaId,
        0,
        cache_bytes,
        (VOID *)(UINTN)cache_addr
    );
    if (EFI_ERROR(status)) {
        Print(L"Warning: ReadBlocks failed for disk cache (%r)\r\n", status);
        return EFI_SUCCESS;
    }

    handoff->disk_cache_phys_base = (UINT64)cache_addr;
    handoff->disk_cache_byte_size = (UINT64)cache_bytes;
    handoff->disk_cache_lba_start = 0;
    handoff->disk_cache_lba_count = chosen_lbas;
    handoff->disk_cache_block_size = media->BlockSize;
    handoff->disk_cache_flags = 1;

    Print(L"Disk cache ready: lba_count=%ld block=%d bytes=0x%lx @ 0x%lx\r\n",
          chosen_lbas,
          media->BlockSize,
          cache_bytes,
          cache_addr);

    return EFI_SUCCESS;
}

static EFI_STATUS elf_compute_load_range(
    VOID *elf_file,
    UINTN elf_size,
    UINT64 *image_min_out,
    UINT64 *image_max_out
) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_file;
    Elf64_Phdr *phdrs;
    UINT64 image_min = ~0ULL;
    UINT64 image_max = 0ULL;

    if (!elf_file || !image_min_out || !image_max_out || elf_size < sizeof(Elf64_Ehdr)) {
        return EFI_LOAD_ERROR;
    }

    if (ehdr->e_ident[0] != 0x7F ||
        ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' ||
        ehdr->e_ident[3] != 'F') {
        return EFI_LOAD_ERROR;
    }

    if (ehdr->e_ident[4] != ELFCLASS64 ||
        ehdr->e_ident[5] != ELFDATA2LSB ||
        ehdr->e_type != ET_EXEC ||
        ehdr->e_machine != EM_X86_64) {
        return EFI_LOAD_ERROR;
    }

    if (ehdr->e_phoff < sizeof(Elf64_Ehdr) ||
        ehdr->e_phoff >= elf_size ||
        ehdr->e_phoff + (UINT64)ehdr->e_phnum * sizeof(Elf64_Phdr) > elf_size) {
        return EFI_LOAD_ERROR;
    }

    phdrs = (Elf64_Phdr *)((UINT8 *)elf_file + ehdr->e_phoff);
    for (UINT16 i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        UINT64 seg_base;
        UINT64 alloc_base;
        UINT64 alloc_end;

        if (ph->p_type != PT_LOAD) {
            continue;
        }

        seg_base = ph->p_paddr ? ph->p_paddr : ph->p_vaddr;
        alloc_base = align_down(seg_base, PAGE_SIZE);
        alloc_end = align_up(seg_base + ph->p_memsz, PAGE_SIZE);

        if (alloc_base < image_min) {
            image_min = alloc_base;
        }
        if (alloc_end > image_max) {
            image_max = alloc_end;
        }
    }

    if (image_min == ~0ULL || image_max == 0ULL || image_max <= image_min) {
        return EFI_LOAD_ERROR;
    }

    *image_min_out = image_min;
    *image_max_out = image_max;
    return EFI_SUCCESS;
}

static BOOLEAN buffer_overlaps_image_range(
    const VOID *buffer,
    UINTN size,
    UINT64 image_min,
    UINT64 image_max
) {
    UINT64 buf_start;
    UINT64 buf_end;

    if (!buffer || size == 0 || image_max <= image_min) {
        return FALSE;
    }

    buf_start = (UINT64)(UINTN)buffer;
    buf_end = buf_start + (UINT64)size;

    return (buf_start < image_max) && (buf_end > image_min);
}

static EFI_STATUS relocate_elf_buffer_if_overlapping(
    VOID **buffer,
    UINTN size,
    UINT64 image_min,
    UINT64 image_max
) {
    EFI_STATUS status;
    VOID *new_buffer;

    if (!buffer || !*buffer || size == 0) {
        return EFI_INVALID_PARAMETER;
    }

    if (!buffer_overlaps_image_range(*buffer, size, image_min, image_max)) {
        return EFI_SUCCESS;
    }

    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, size, &new_buffer);
    if (EFI_ERROR(status)) {
        return status;
    }

    mem_copy(new_buffer, *buffer, size);
    uefi_call_wrapper(BS->FreePool, 1, *buffer);
    *buffer = new_buffer;
    return EFI_SUCCESS;
}

static EFI_STATUS load_elf_image(
    VOID *elf_file,
    UINTN elf_size,
    stage_entry_t *entry_out,
    UINT64 *image_phys_base_out,
    UINT64 *image_phys_size_out
) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_file;
    Elf64_Phdr *phdrs;
    UINT16 i;
    UINT64 image_min = ~0ULL;
    UINT64 image_max = 0;

    if (elf_size < sizeof(Elf64_Ehdr)) {
        return EFI_LOAD_ERROR;
    }

    if (ehdr->e_ident[0] != 0x7F ||
        ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' ||
        ehdr->e_ident[3] != 'F') {
        return EFI_LOAD_ERROR;
        }

        if (ehdr->e_ident[4] != ELFCLASS64 ||
            ehdr->e_ident[5] != ELFDATA2LSB ||
            ehdr->e_type != ET_EXEC ||
            ehdr->e_machine != EM_X86_64) {
            return EFI_LOAD_ERROR;
            }

    // Validate that program headers are inside the file
    if (ehdr->e_phoff < sizeof(Elf64_Ehdr) ||
        ehdr->e_phoff >= elf_size ||
        ehdr->e_phoff + (UINT64)ehdr->e_phnum * sizeof(Elf64_Phdr) > elf_size) {
        Print(L"Error: program headers exceed file size\r\n");
        return EFI_LOAD_ERROR;
    }

    phdrs = (Elf64_Phdr *)((UINT8 *)elf_file + ehdr->e_phoff);

        for (i = 0; i < ehdr->e_phnum; i++) {
            Elf64_Phdr *ph = &phdrs[i];

            if (ph->p_type != PT_LOAD) {
                continue;
            }

            UINT64 seg_base = ph->p_paddr ? ph->p_paddr : ph->p_vaddr;

            // Validate that address is consistent with linker script (>= 2MB)
            if (seg_base < 0x200000 || seg_base > 0xFFFFFFFFFFFF0000ULL) {
                Print(L"Error: segment address outside expected range: 0x%lx\r\n", seg_base);
                return EFI_LOAD_ERROR;
            }

            UINT64 alloc_base = align_down(seg_base, PAGE_SIZE);
            UINT64 alloc_end  = align_up(seg_base + ph->p_memsz, PAGE_SIZE);
            UINTN pages = (UINTN)((alloc_end - alloc_base) / PAGE_SIZE);
            EFI_PHYSICAL_ADDRESS addr = alloc_base;
            EFI_MEMORY_TYPE mem_type = (ph->p_flags & PF_X) ? EfiLoaderCode : EfiLoaderData;

            EFI_STATUS status = uefi_call_wrapper(
                BS->AllocatePages,
                4,
                AllocateAddress,
                mem_type,
                pages,
                &addr
            );
            if (EFI_ERROR(status)) {
                Print(L"Error: PT_LOAD alloc failed idx=%d base=0x%lx pages=0x%lx status=%r\r\n",
                      i,
                      alloc_base,
                      pages,
                      status);
                return status;
            }

            mem_zero((VOID *)(UINTN)alloc_base, (UINTN)(alloc_end - alloc_base));

            // Validate that segment data is inside the file
            if (ph->p_offset + ph->p_filesz > elf_size) {
                Print(L"Error: segment data exceeds file size\r\n");
                return EFI_LOAD_ERROR;
            }

            mem_copy(
                (VOID *)(UINTN)seg_base,
                (VOID *)((UINT8 *)elf_file + ph->p_offset),
                (UINTN)ph->p_filesz
            );

            // Debug check via debugcon (port 0xE9): 'D' + 4 bytes from dst, 'S' + 4 bytes from src
            {
                UINT8 *dst = (UINT8 *)(UINTN)seg_base;
                UINT8 *src = (UINT8 *)elf_file + ph->p_offset;
                __asm__ volatile ("outb %0, %1" : : "a"((UINT8)'D'), "Nd"((UINT16)0xE9));
                for (int _di = 0; _di < 4; _di++)
                    __asm__ volatile ("outb %0, %1" : : "a"(dst[_di]), "Nd"((UINT16)0xE9));
                __asm__ volatile ("outb %0, %1" : : "a"((UINT8)'S'), "Nd"((UINT16)0xE9));
                for (int _di = 0; _di < 4; _di++)
                    __asm__ volatile ("outb %0, %1" : : "a"(src[_di]), "Nd"((UINT16)0xE9));
            }

            if (alloc_base < image_min) {
                image_min = alloc_base;
            }
            if (alloc_end > image_max) {
                image_max = alloc_end;
            }
        }

        // Ensure at least one PT_LOAD segment was found
        if (image_min == ~0ULL || image_max == 0) {
            Print(L"Error: no PT_LOAD segments found in ELF image\r\n");
            return EFI_LOAD_ERROR;
        }

        *entry_out = (stage_entry_t)(UINTN)ehdr->e_entry;
        *image_phys_base_out = image_min;
        *image_phys_size_out = (image_max > image_min) ? (image_max - image_min) : 0;

        return EFI_SUCCESS;
}

static EFI_STATUS acquire_memory_map(
    EFI_MEMORY_DESCRIPTOR **memory_map,
    UINTN *memory_map_size,
    UINTN *map_key,
    UINTN *desc_size,
    UINT32 *desc_version
) {
    EFI_STATUS status;
    EFI_PHYSICAL_ADDRESS map_addr;
    UINTN pages;

    *memory_map = NULL;
    *memory_map_size = 0;

    status = uefi_call_wrapper(
        BS->GetMemoryMap,
        5,
        memory_map_size,
        *memory_map,
        map_key,
        desc_size,
        desc_version
    );

    if (status != EFI_BUFFER_TOO_SMALL) {
        return status;
    }

    *memory_map_size += (*desc_size) * 8;
    pages = bytes_to_pages(*memory_map_size);

    status = alloc_pages_below_4g(pages, EfiLoaderData, &map_addr);
    if (EFI_ERROR(status)) {
        return status;
    }

    *memory_map = (EFI_MEMORY_DESCRIPTOR *)(UINTN)map_addr;
    mem_zero(*memory_map, pages * PAGE_SIZE);

    status = uefi_call_wrapper(
        BS->GetMemoryMap,
        5,
        memory_map_size,
        *memory_map,
        map_key,
        desc_size,
        desc_version
    );

    return status;
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table) {
    EFI_STATUS status;
    VOID *kernel_buffer = NULL;
    UINTN kernel_size = 0;
    VOID *stage2_buffer = NULL;
    UINTN stage2_size = 0;
    stage_entry_t entry_point = NULL;
    boot_info_t *boot_info = NULL;
    handoff_v0_t *handoff = NULL;
    EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
    UINTN memory_map_size = 0;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    UINT64 image_phys_base = 0;
    UINT64 image_phys_size = 0;
    UINT64 kernel_phys_base = 0;
    UINT64 kernel_phys_size = 0;
    UINT64 new_cr3 = 0;
    EFI_PHYSICAL_ADDRESS boot_info_addr = 0;
    EFI_PHYSICAL_ADDRESS handoff_addr = 0;
    EFI_PHYSICAL_ADDRESS stage2_file_addr = 0;
    EFI_PHYSICAL_ADDRESS kernel_file_addr = 0;
    BOOLEAN using_stage2 = FALSE;

    InitializeLib(image, system_table);

    Print(L"CiukiOS UEFI Loader started\r\n");

    status = alloc_pages_below_4g(1, EfiLoaderData, &boot_info_addr);
    if (EFI_ERROR(status)) {
        Print(L"Error allocating boot_info: %r\r\n", status);
        return status;
    }

    boot_info = (boot_info_t *)(UINTN)boot_info_addr;
    mem_zero(boot_info, PAGE_SIZE);

    status = alloc_pages_below_4g(1, EfiLoaderData, &handoff_addr);
    if (EFI_ERROR(status)) {
        Print(L"Error allocating handoff: %r\r\n", status);
        return status;
    }

    handoff = (handoff_v0_t *)(UINTN)handoff_addr;
    mem_zero(handoff, PAGE_SIZE);

    status = read_file(
        image,
        L"\\EFI\\CiukiOS\\stage2.elf",
        &stage2_buffer,
        &stage2_size
    );
    if (!EFI_ERROR(status)) {
        UINT64 stage2_image_min = 0;
        UINT64 stage2_image_max = 0;

        if (stage2_size == 0) {
            Print(L"Error: stage2.elf is empty\r\n");
            return EFI_LOAD_ERROR;
        }

        status = elf_compute_load_range(
            stage2_buffer,
            stage2_size,
            &stage2_image_min,
            &stage2_image_max
        );
        if (EFI_ERROR(status)) {
            Print(L"Error validating stage2.elf load range: %r\r\n", status);
            return status;
        }

        status = relocate_elf_buffer_if_overlapping(
            &stage2_buffer,
            stage2_size,
            stage2_image_min,
            stage2_image_max
        );
        if (EFI_ERROR(status)) {
            Print(L"Error relocating stage2.elf staging buffer: %r\r\n", status);
            return status;
        }

        status = alloc_staging_pages_preferred(stage2_size, EfiLoaderData, &stage2_file_addr);
        if (!EFI_ERROR(status)) {
            mem_copy((VOID *)(UINTN)stage2_file_addr, stage2_buffer, stage2_size);
            uefi_call_wrapper(BS->FreePool, 1, stage2_buffer);
            stage2_buffer = (VOID *)(UINTN)stage2_file_addr;
        }

        Print(L"stage2.elf loaded into memory\r\n");

        status = load_elf_image(
            stage2_buffer,
            stage2_size,
            &entry_point,
            &image_phys_base,
            &image_phys_size
        );
        if (EFI_ERROR(status)) {
            Print(L"Error loading stage2.elf: %r\r\n", status);
            return status;
        }

        using_stage2 = TRUE;
        handoff->magic = HANDOFF_V0_MAGIC;
        handoff->version = HANDOFF_V0_VERSION;
        handoff->stage2_load_addr = image_phys_base;
        handoff->stage2_size = image_phys_size;
        handoff->flags = 0;
        handoff->framebuffer_base = 0;
        handoff->framebuffer_width = 0;
        handoff->framebuffer_height = 0;
        handoff->framebuffer_pitch = 0;
        handoff->framebuffer_bpp = 0;

    } else if (status == EFI_NOT_FOUND) {
        Print(L"stage2.elf not found, falling back to kernel.elf\r\n");

        status = read_file(
            image,
            L"\\EFI\\CiukiOS\\kernel.elf",
            &kernel_buffer,
            &kernel_size
        );
        if (EFI_ERROR(status)) {
            Print(L"Error opening kernel.elf: %r\r\n", status);
            return status;
        }

        {
            UINT64 kernel_image_min = 0;
            UINT64 kernel_image_max = 0;

            if (kernel_size == 0) {
                Print(L"Error: kernel.elf is empty\r\n");
                return EFI_LOAD_ERROR;
            }

            status = elf_compute_load_range(
                kernel_buffer,
                kernel_size,
                &kernel_image_min,
                &kernel_image_max
            );
            if (EFI_ERROR(status)) {
                Print(L"Error validating kernel.elf load range: %r\r\n", status);
                return status;
            }

            status = relocate_elf_buffer_if_overlapping(
                &kernel_buffer,
                kernel_size,
                kernel_image_min,
                kernel_image_max
            );
            if (EFI_ERROR(status)) {
                Print(L"Error relocating kernel.elf staging buffer: %r\r\n", status);
                return status;
            }

            status = alloc_staging_pages_preferred(kernel_size, EfiLoaderData, &kernel_file_addr);
            if (!EFI_ERROR(status)) {
                mem_copy((VOID *)(UINTN)kernel_file_addr, kernel_buffer, kernel_size);
                uefi_call_wrapper(BS->FreePool, 1, kernel_buffer);
                kernel_buffer = (VOID *)(UINTN)kernel_file_addr;
            }
        }

        Print(L"kernel.elf loaded into memory\r\n");

        status = load_elf_image(
            kernel_buffer,
            kernel_size,
            &entry_point,
            &image_phys_base,
            &image_phys_size
        );
        if (EFI_ERROR(status)) {
            Print(L"Error loading kernel.elf: %r\r\n", status);
            return status;
        }
    } else {
        Print(L"Error opening stage2.elf: %r\r\n", status);
        return status;
    }

    kernel_phys_base = image_phys_base;
    kernel_phys_size = image_phys_size;

    if (using_stage2) {
        status = load_com_catalog(image, handoff);
        if (EFI_ERROR(status)) {
            Print(L"Warning: COM catalog load failed (%r)\r\n", status);
            handoff->com_phys_base = 0;
            handoff->com_phys_size = 0;
            handoff->com_count = 0;
            mem_zero(handoff->com_entries, sizeof(handoff->com_entries));
        }

        status = load_disk_cache(image, handoff);
        if (EFI_ERROR(status)) {
            Print(L"Warning: disk cache load failed (%r)\r\n", status);
            handoff->disk_cache_phys_base = 0;
            handoff->disk_cache_byte_size = 0;
            handoff->disk_cache_lba_start = 0;
            handoff->disk_cache_lba_count = 0;
            handoff->disk_cache_block_size = 0;
            handoff->disk_cache_flags = 0;
        }
    }

    status = build_bootstrap_paging(&new_cr3);
    if (EFI_ERROR(status)) {
        Print(L"Error building bootstrap paging: %r\r\n", status);
        return status;
    }

    status = acquire_memory_map(
        &memory_map,
        &memory_map_size,
        &map_key,
        &desc_size,
        &desc_version
    );
    if (EFI_ERROR(status)) {
        Print(L"Error in GetMemoryMap: %r\r\n", status);
        return status;
    }

    boot_info->magic = BOOTINFO_MAGIC;
    boot_info->memory_map_ptr = (UINT64)(UINTN)memory_map;
    boot_info->memory_map_size = (UINT64)memory_map_size;
    boot_info->memory_map_descriptor_size = (UINT64)desc_size;
    boot_info->memory_map_descriptor_version = desc_version;
    boot_info->kernel_phys_base = kernel_phys_base;
    boot_info->kernel_phys_size = kernel_phys_size;

    {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
        EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
        EFI_STATUS gop_status = uefi_call_wrapper(
            BS->LocateProtocol, 3, &gop_guid, NULL, (VOID **)&gop
        );
        if (!EFI_ERROR(gop_status) && gop && gop->Mode && gop->Mode->Info) {
            /* --- GOP mode catalog + policyv2 scoring engine --- */
            {
                /* Resolution class table for scoring (P1-V1) */
                static const struct { UINT32 w; UINT32 h; UINT32 rank; const CHAR16 *name; } res_class[] = {
                    {1024,  768, 5, L"baseline"},
                    {1280,  720, 4, L"HD720"},
                    {1280,  800, 4, L"HD800"},
                    {1600,  900, 3, L"HD+"},
                    {1920, 1080, 2, L"FHD"},
                    {2560, 1440, 1, L"QHD"},
                    {3840, 2160, 0, L"4K"},
                };
                static const UINT32 res_class_count = sizeof(res_class) / sizeof(res_class[0]);

                /* Preferred resolution order for fallback scoring (lower index = higher priority) */
                static const struct { UINT32 w; UINT32 h; } preferred[] = {
                    {1024, 768},
                    {800,  600},
                    {1280, 720},
                    {1280, 1024},
                    {1920, 1080},
                };
                UINT32 pref_count = sizeof(preferred) / sizeof(preferred[0]);
                UINT32 best_mode = gop->Mode->Mode;
                UINT32 best_pref = pref_count;
                INT32  best_score = -1;
                UINT32 mode_count = gop->Mode->MaxMode;
                UINT32 mi;
                UINT32 catalog_count = 0;
                vmode_config_t vmode_cfg;
                bootcfg_data_t cmos_cfg;
                BOOLEAN cfg_resolved = FALSE;
                BOOLEAN cmos_cfg_valid = FALSE;
                BOOLEAN has_1024_candidate = FALSE;
                const CHAR16 *cfg_source = L"POLICY";

                loader_memset(&cmos_cfg, 0U, sizeof(cmos_cfg));
                cmos_cfg_valid = bootcfg_load(&cmos_cfg);
                load_vmode_config(image, &vmode_cfg);

                if (using_stage2) {
                    handoff->gop_mode_count = 0;
                    handoff->gop_active_mode_id = gop->Mode->Mode;
                }

                for (mi = 0; mi < mode_count; mi++) {
                    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info = NULL;
                    UINTN mode_info_size = 0;
                    UINT32 bpp_val;
                    UINT32 bytes_pp;
                    UINT32 fits_backbuf;
                    EFI_STATUS qi = uefi_call_wrapper(
                        gop->QueryMode, 4, gop, mi, &mode_info_size, &mode_info
                    );
                    if (EFI_ERROR(qi) || !mode_info) continue;

                    bpp_val = gop_detect_bpp(mode_info);
                    bytes_pp = (bpp_val + 7U) / 8U;
                    fits_backbuf = (mode_info->HorizontalResolution <= VIDEO_DRIVER_MAX_W &&
                                   mode_info->VerticalResolution   <= VIDEO_DRIVER_MAX_H &&
                                   bytes_pp <= VIDEO_DRIVER_MAX_BPP) ? 1U : 0U;

                    if (bpp_val == 32 && fits_backbuf &&
                        mode_info->HorizontalResolution >= VIDEO_POLICY_BASELINE_W &&
                        mode_info->VerticalResolution >= VIDEO_POLICY_BASELINE_H) {
                        has_1024_candidate = TRUE;
                    }

                    /* Populate GOP catalog entry */
                    if (using_stage2 && catalog_count < VIDEO_GOP_CATALOG_MAX) {
                        handoff_gop_mode_entry_t *entry = &handoff->gop_modes[catalog_count];
                        entry->mode_id = mi;
                        entry->width = mode_info->HorizontalResolution;
                        entry->height = mode_info->VerticalResolution;
                        entry->bpp = bpp_val;
                        entry->pixels_per_scanline = mode_info->PixelsPerScanLine;
                        entry->flags = fits_backbuf;
                        catalog_count++;
                    }

                    /* --- CMOS priority check (any 32bpp mode) --- */
                    if (!cfg_resolved && bpp_val == 32) {
                        if (cmos_cfg_valid &&
                            cmos_cfg.mode_id != BOOTCFG_MODE_ID_NONE &&
                            mi == cmos_cfg.mode_id) {
                            best_mode = mi;
                            cfg_resolved = TRUE;
                            cfg_source = L"CMOS";
                        }
                        if (!cfg_resolved && cmos_cfg_valid &&
                            cmos_cfg.width != 0U && cmos_cfg.height != 0U &&
                            mode_info->HorizontalResolution == cmos_cfg.width &&
                            mode_info->VerticalResolution == cmos_cfg.height) {
                            best_mode = mi;
                            cfg_resolved = TRUE;
                            cfg_source = L"CMOS";
                        }
                    }

                    /* --- VMODE.CFG priority check (any 32bpp mode) --- */
                    if (!cfg_resolved && bpp_val == 32 && vmode_cfg_valid(&vmode_cfg)) {
                        if (vmode_cfg.mode_id != 0xFFFFFFFFU &&
                            mi == vmode_cfg.mode_id) {
                            best_mode = mi;
                            cfg_resolved = TRUE;
                            cfg_source = L"VMODE.CFG";
                        }
                        if (!cfg_resolved &&
                            vmode_cfg.width != 0 && vmode_cfg.height != 0 &&
                            mode_info->HorizontalResolution == vmode_cfg.width &&
                            mode_info->VerticalResolution   == vmode_cfg.height) {
                            best_mode = mi;
                            cfg_resolved = TRUE;
                            cfg_source = L"VMODE.CFG";
                        }
                    }

                    /* --- Policyv2 scoring engine (P1-V1) --- */
                    if (!cfg_resolved && bpp_val == 32) {
                        INT32 mode_score = 0;
                        UINT32 mw = mode_info->HorizontalResolution;
                        UINT32 mh = mode_info->VerticalResolution;
                        UINT64 frame_bytes = (UINT64)mh * (UINT64)mode_info->PixelsPerScanLine * bytes_pp;

                        /* Filter: sane dimensions (32..7680), pitch > 0 */
                        if (mw >= 32U && mw <= 7680U && mh >= 32U && mh <= 4320U &&
                            mode_info->PixelsPerScanLine >= mw) {

                            /* Score: resolution class match (0-60 points) */
                            for (UINT32 ci = 0; ci < res_class_count; ci++) {
                                if (mw == res_class[ci].w && mh == res_class[ci].h) {
                                    mode_score += (INT32)(60U - res_class[ci].rank * 8U);
                                    break;
                                }
                            }

                            /* Score: aspect ratio proximity to 16:10 or 16:9 (0-20 points) */
                            {
                                UINT32 ratio_x10 = (mw * 10U) / (mh > 0U ? mh : 1U);
                                /* 16:9 = 17, 16:10 = 16, 4:3 = 13 */
                                if (ratio_x10 >= 16U && ratio_x10 <= 18U) {
                                    mode_score += 20;
                                } else if (ratio_x10 >= 13U && ratio_x10 <= 15U) {
                                    mode_score += 10;
                                }
                            }

                            /* Score: fits double-buffer budget (0-15 points) */
                            if (frame_bytes <= VIDEO_BUDGET_SAFE_CEILING) {
                                mode_score += 15;
                            } else if (frame_bytes <= VIDEO_BUDGET_TIER_QHD_BYTES) {
                                mode_score += 5;
                            }

                            /* Score: baseline satisfaction bonus (0-5 points) */
                            if (mw >= VIDEO_POLICY_BASELINE_W && mh >= VIDEO_POLICY_BASELINE_H) {
                                mode_score += 5;
                            }

                            /* Tie-break: prefer lower mode index for determinism */
                            if (mode_score > best_score ||
                                (mode_score == best_score && mi < best_mode)) {
                                best_mode = mi;
                                best_score = mode_score;
                            }
                        }

                        /* Legacy fallback preference table (fits_backbuf gated) */
                        if (fits_backbuf) {
                            for (UINT32 pi = 0; pi < pref_count; pi++) {
                                if (mw == preferred[pi].w &&
                                    mh == preferred[pi].h &&
                                    pi < best_pref) {
                                    best_pref = pi;
                                    break;
                                }
                            }
                        }
                    }

                    uefi_call_wrapper(BS->FreePool, 1, mode_info);
                }

                if (using_stage2) {
                    handoff->gop_mode_count = catalog_count;
                }

                /* Policyv2 result marker (P1-V1) */
                {
                    const CHAR16 *result_str = L"PASS";
                    if (!cfg_resolved && best_score < 0) {
                        result_str = L"FALLBACK";
                    }
                    Print(L"GOP: policyv2 modes=%d selected=%dx%d score=%d result=%s\r\n",
                          mode_count, 0, 0, best_score, result_str);
                    /* Actual selected resolution printed after SetMode below */
                }

                if (best_mode != gop->Mode->Mode) {
                    EFI_STATUS sm = uefi_call_wrapper(gop->SetMode, 2, gop, best_mode);
                    if (!EFI_ERROR(sm)) {
                        Print(L"GOP: switched to mode %d (%dx%d)%s\r\n",
                              best_mode,
                              gop->Mode->Info->HorizontalResolution,
                              gop->Mode->Info->VerticalResolution,
                              cfg_resolved ? L" (from config)" : L"");
                    } else {
                        Print(L"GOP: SetMode %d failed (%r), using default\r\n", best_mode, sm);
                        cfg_source = L"POLICY";
                    }
                }

                Print(L"GOP: config source=%s mode=%d selected=%dx%d\r\n",
                      cfg_source,
                      gop->Mode->Mode,
                      gop->Mode->Info->HorizontalResolution,
                      gop->Mode->Info->VerticalResolution);

                /* Re-emit policyv2 marker with final resolved resolution */
                {
                    UINT32 sel_w = gop->Mode->Info->HorizontalResolution;
                    UINT32 sel_h = gop->Mode->Info->VerticalResolution;
                    const CHAR16 *result_str = L"PASS";
                    if (!cfg_resolved && best_score < 0) {
                        result_str = L"FALLBACK";
                    }
                    Print(L"GOP: policyv2 modes=%d selected=%dx%d score=%d result=%s\r\n",
                          mode_count, sel_w, sel_h, best_score, result_str);
                }

                /* Budgetv2 tier classification (P1-V2) */
                {
                    UINT32 sel_w = gop->Mode->Info->HorizontalResolution;
                    UINT32 sel_h = gop->Mode->Info->VerticalResolution;
                    UINT32 sel_bpp = gop_detect_bpp(gop->Mode->Info);
                    UINT32 sel_bpp_bytes = (sel_bpp + 7U) / 8U;
                    UINT64 frame_bytes = (UINT64)sel_h *
                                         (UINT64)gop->Mode->Info->PixelsPerScanLine *
                                         sel_bpp_bytes;
                    const CHAR16 *class_name = L"unknown";
                    UINT32 allow_db = 0;

                    if (frame_bytes <= VIDEO_BUDGET_TIER_BASELINE_BYTES) {
                        class_name = L"baseline"; allow_db = 1;
                    } else if (frame_bytes <= VIDEO_BUDGET_TIER_HD_BYTES) {
                        class_name = L"HD"; allow_db = 1;
                    } else if (frame_bytes <= VIDEO_BUDGET_TIER_HDP_BYTES) {
                        class_name = L"HD+"; allow_db = 1;
                    } else if (frame_bytes <= VIDEO_BUDGET_TIER_FHD_BYTES) {
                        class_name = L"FHD"; allow_db = 1;
                    } else if (frame_bytes <= VIDEO_BUDGET_TIER_QHD_BYTES) {
                        class_name = L"QHD"; allow_db = 0;
                    } else if (frame_bytes <= VIDEO_BUDGET_TIER_4K_BYTES) {
                        class_name = L"4K"; allow_db = 0;
                    } else {
                        class_name = L"oversize"; allow_db = 0;
                    }

                    Print(L"GOP: budgetv2 class=%s bytes=%lu allow_db=%d\r\n",
                          class_name, frame_bytes, allow_db);

                    /* Fallback degrade: if selected mode exceeds safe ceiling and no config override, degrade */
                    if (!cfg_resolved && frame_bytes > VIDEO_BUDGET_SAFE_CEILING) {
                        /* Try to find a lower resolution class that fits */
                        UINT32 fallback_mode = best_mode;
                        BOOLEAN found_fallback = FALSE;
                        UINT32 fi;

                        for (fi = 0; fi < catalog_count && using_stage2; fi++) {
                            handoff_gop_mode_entry_t *fe = &handoff->gop_modes[fi];
                            if (fe->bpp == 32 &&
                                fe->width >= VIDEO_POLICY_BASELINE_W &&
                                fe->height >= VIDEO_POLICY_BASELINE_H) {
                                UINT64 fb = (UINT64)fe->height * (UINT64)fe->pixels_per_scanline * 4ULL;
                                if (fb <= VIDEO_BUDGET_SAFE_CEILING) {
                                    fallback_mode = fe->mode_id;
                                    found_fallback = TRUE;
                                    Print(L"GOP: fallback reason=overbudget next=%dx%d\r\n",
                                          fe->width, fe->height);
                                    break;
                                }
                            }
                        }

                        if (found_fallback && fallback_mode != gop->Mode->Mode) {
                            EFI_STATUS fm = uefi_call_wrapper(gop->SetMode, 2, gop, fallback_mode);
                            if (!EFI_ERROR(fm)) {
                                sel_w = gop->Mode->Info->HorizontalResolution;
                                sel_h = gop->Mode->Info->VerticalResolution;
                                Print(L"GOP: degraded to mode %d (%dx%d)\r\n",
                                      fallback_mode, sel_w, sel_h);
                            }
                        }
                    }
                }

                {
                    UINT32 selected_w = gop->Mode->Info->HorizontalResolution;
                    UINT32 selected_h = gop->Mode->Info->VerticalResolution;
                    BOOLEAN policy_pass = TRUE;

                    if (has_1024_candidate &&
                        (selected_w < VIDEO_POLICY_BASELINE_W ||
                         selected_h < VIDEO_POLICY_BASELINE_H)) {
                        policy_pass = FALSE;
                    }

                    Print(L"GOP: policy1024 available=%d selected=%dx%d result=%s\r\n",
                          has_1024_candidate ? 1 : 0,
                          selected_w,
                          selected_h,
                          policy_pass ? L"PASS" : L"FAIL");

                    {
                        UINT32 sel_bpp = gop_detect_bpp(gop->Mode->Info);
                        UINT32 sel_bpp_bytes = (sel_bpp + 7U) / 8U;
                        UINT64 needed = (UINT64)selected_h *
                                        (UINT64)gop->Mode->Info->PixelsPerScanLine *
                                        sel_bpp_bytes;
                        UINT64 budget = (UINT64)VIDEO_DRIVER_MAX_W *
                                        VIDEO_DRIVER_MAX_H *
                                        VIDEO_DRIVER_MAX_BPP;
                        Print(L"GOP: backbuf budget=%lu needed=%lu fits=%s\r\n",
                              budget, needed,
                              (needed <= budget) ? L"YES" : L"NO");
                    }
                }

                if (using_stage2) {
                    handoff->gop_active_mode_id = gop->Mode->Mode;
                }
            }

            {
                UINT32 bpp = gop_detect_bpp(gop->Mode->Info);
                UINT32 bytes_per_pixel = (bpp + 7U) / 8U;

                if (bytes_per_pixel == 0U) {
                    bytes_per_pixel = 4U;
                    bpp = 32U;
                }

                boot_info->framebuffer_base   = (UINT64)gop->Mode->FrameBufferBase;
                boot_info->framebuffer_width  = gop->Mode->Info->HorizontalResolution;
                boot_info->framebuffer_height = gop->Mode->Info->VerticalResolution;
                boot_info->framebuffer_pitch  = gop->Mode->Info->PixelsPerScanLine * bytes_per_pixel;
                boot_info->framebuffer_bpp    = bpp;

                if (using_stage2) {
                    handoff->framebuffer_base = boot_info->framebuffer_base;
                    handoff->framebuffer_width = boot_info->framebuffer_width;
                    handoff->framebuffer_height = boot_info->framebuffer_height;
                    handoff->framebuffer_pitch = boot_info->framebuffer_pitch;
                    handoff->framebuffer_bpp = boot_info->framebuffer_bpp;
                }

                Print(L"Framebuffer: %dx%d pitch=%d bpp=%d @ 0x%lx\r\n",
                      boot_info->framebuffer_width,
                      boot_info->framebuffer_height,
                      boot_info->framebuffer_pitch,
                      boot_info->framebuffer_bpp,
                      boot_info->framebuffer_base);
            }
        } else {
            Print(L"Warning: GOP not found, no framebuffer\r\n");
        }
    }

    Print(L"entry_point = 0x%lx\r\n", (UINT64)(UINTN)entry_point);
    Print(L"image_phys_base = 0x%lx\r\n", image_phys_base);
    Print(L"image_phys_size = 0x%lx\r\n", image_phys_size);
    Print(L"new_cr3 = 0x%lx\r\n", new_cr3);
    Print(L"boot_info = 0x%lx\r\n", (UINT64)(UINTN)boot_info);
    if (using_stage2) {
        Print(L"handoff = 0x%lx\r\n", (UINT64)(UINTN)handoff);
    }
    Print(L"memory_map = 0x%lx\r\n", (UINT64)(UINTN)memory_map);
    if (using_stage2) {
        Print(L"Stage2 ELF loaded, leaving Boot Services...\r\n");
    } else {
        Print(L"Kernel ELF loaded, leaving Boot Services...\r\n");
    }
    dump_kernel_entry_bytes(entry_point);

    status = uefi_call_wrapper(BS->ExitBootServices, 2, image, map_key);

    if (status == EFI_INVALID_PARAMETER) {
        status = acquire_memory_map(
            &memory_map,
            &memory_map_size,
            &map_key,
            &desc_size,
            &desc_version
        );
        if (EFI_ERROR(status)) {
            return status;
        }

        boot_info->memory_map_ptr = (UINT64)(UINTN)memory_map;
        boot_info->memory_map_size = (UINT64)memory_map_size;
        boot_info->memory_map_descriptor_size = (UINT64)desc_size;
        boot_info->memory_map_descriptor_version = desc_version;

        status = uefi_call_wrapper(BS->ExitBootServices, 2, image, map_key);
        if (EFI_ERROR(status)) {
            return status;
        }
    } else if (EFI_ERROR(status)) {
        return status;
    }

    efi_handoff(new_cr3, entry_point, boot_info, using_stage2 ? handoff : NULL);

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
