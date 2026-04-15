#ifndef HANDOFF_H
#define HANDOFF_H

#include <stdint.h>

#define HANDOFF_V0_MAGIC 0x30464F484B554943ULL /* "CIUKHOF0" */
#define HANDOFF_V0_VERSION 0ULL

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
} handoff_v0_t;

#endif
