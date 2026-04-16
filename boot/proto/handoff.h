#ifndef HANDOFF_H
#define HANDOFF_H

#include <stdint.h>
#include "video_limits.h"

#define HANDOFF_V0_MAGIC 0x30464F484B554943ULL /* "CIUKHOF0" */
#define HANDOFF_V0_VERSION 1ULL

typedef struct handoff_gop_mode_entry {
    uint32_t mode_id;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pixels_per_scanline;
    uint32_t flags;  /* bit 0: compatible with driver backbuffer limits */
} handoff_gop_mode_entry_t;

#define HANDOFF_COM_MAX 16U
#define HANDOFF_COM_NAME_MAX 12U /* 8.3 name with dot, e.g. "INIT.COM" */
#define HANDOFF_DISK_CACHE_MAX_BYTES (8ULL * 1024ULL * 1024ULL)

typedef struct handoff_com_entry {
    char name[HANDOFF_COM_NAME_MAX + 1];
    uint64_t phys_base;
    uint64_t size;
} handoff_com_entry_t;

typedef struct handoff_v0 {
    uint64_t magic;
    uint64_t version;
    uint64_t stage2_load_addr;
    uint64_t stage2_size;
    uint64_t flags;
    uint64_t framebuffer_base; /* GOP framebuffer base at handoff time */
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_bpp;
    uint32_t framebuffer_reserved0;
    uint64_t com_phys_base;   /* physical address of loaded COM binary, 0 if none */
    uint64_t com_phys_size;   /* size in bytes of loaded COM binary, 0 if none */
    uint64_t com_count;       /* number of valid entries in com_entries */
    handoff_com_entry_t com_entries[HANDOFF_COM_MAX];
    uint64_t disk_cache_phys_base; /* physical base of cached disk blocks, 0 if unavailable */
    uint64_t disk_cache_byte_size; /* byte size of cache region */
    uint64_t disk_cache_lba_start; /* first cached LBA (normally 0) */
    uint64_t disk_cache_lba_count; /* number of cached LBAs */
    uint32_t disk_cache_block_size; /* bytes per LBA */
    uint32_t disk_cache_flags;
    uint32_t gop_mode_count;       /* number of valid entries in gop_modes */
    uint32_t gop_active_mode_id;   /* active GOP mode id at handoff time */
    handoff_gop_mode_entry_t gop_modes[VIDEO_GOP_CATALOG_MAX];
} handoff_v0_t;

#endif
