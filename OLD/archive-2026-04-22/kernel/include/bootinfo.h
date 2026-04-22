#ifndef BOOTINFO_H
#define BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAGIC 0x4349554B494F5301ULL

typedef struct boot_info {
    uint64_t magic;

    uint64_t framebuffer_base;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_bpp;
    uint32_t reserved0;

    uint64_t memory_map_ptr;
    uint64_t memory_map_size;
    uint64_t memory_map_descriptor_size;
    uint32_t memory_map_descriptor_version;
    uint32_t reserved1;

    uint64_t rsdp_ptr;

    uint64_t kernel_phys_base;
    uint64_t kernel_phys_size;
} boot_info_t;

#endif
