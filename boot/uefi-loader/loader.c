#include <efi.h>
#include <efilib.h>
#include "elf64.h"
#include "../proto/bootinfo.h"
#include "../proto/handoff.h"

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
            /* --- GOP mode selection --- */
            /* Try to pick a preferred resolution (32bpp only). Non-fatal. */
            {
                static const struct { UINT32 w; UINT32 h; } preferred[] = {
                    {800,  600},
                    {1024, 768},
                    {1280, 720},
                    {1280, 1024},
                    {1920, 1080},
                };
                UINT32 pref_count = sizeof(preferred) / sizeof(preferred[0]);
                UINT32 best_mode = gop->Mode->Mode; /* default: keep current */
                UINT32 best_pref = pref_count;       /* lower = better */
                UINT32 mode_count = gop->Mode->MaxMode;
                UINT32 mi;

                for (mi = 0; mi < mode_count; mi++) {
                    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info = NULL;
                    UINTN mode_info_size = 0;
                    EFI_STATUS qi = uefi_call_wrapper(
                        gop->QueryMode, 4, gop, mi, &mode_info_size, &mode_info
                    );
                    if (EFI_ERROR(qi) || !mode_info) continue;

                    /* Only consider 32bpp modes */
                    if (gop_detect_bpp(mode_info) != 32) continue;

                    for (UINT32 pi = 0; pi < pref_count; pi++) {
                        if (mode_info->HorizontalResolution == preferred[pi].w &&
                            mode_info->VerticalResolution   == preferred[pi].h &&
                            pi < best_pref) {
                            best_mode = mi;
                            best_pref = pi;
                            break;
                        }
                    }

                    uefi_call_wrapper(BS->FreePool, 1, mode_info);
                }

                if (best_mode != gop->Mode->Mode) {
                    EFI_STATUS sm = uefi_call_wrapper(gop->SetMode, 2, gop, best_mode);
                    if (!EFI_ERROR(sm)) {
                        Print(L"GOP: switched to mode %d (%dx%d)\r\n",
                              best_mode,
                              gop->Mode->Info->HorizontalResolution,
                              gop->Mode->Info->VerticalResolution);
                    } else {
                        Print(L"GOP: SetMode %d failed (%r), using default\r\n", best_mode, sm);
                    }
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
