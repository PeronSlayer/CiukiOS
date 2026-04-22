#include "disk.h"

static u8 *g_cache_base;
static u64 g_cache_size;
static u64 g_lba_start;
static u64 g_lba_count;
static u32 g_block_size;
static int g_ready;

void stage2_disk_init(const handoff_v0_t *handoff) {
    g_cache_base = (u8 *)0;
    g_cache_size = 0;
    g_lba_start = 0;
    g_lba_count = 0;
    g_block_size = 0;
    g_ready = 0;

    if (!handoff) {
        return;
    }
    if (handoff->disk_cache_phys_base == 0 ||
        handoff->disk_cache_byte_size == 0 ||
        handoff->disk_cache_lba_count == 0 ||
        handoff->disk_cache_block_size == 0) {
        return;
    }

    g_cache_base = (u8 *)(u64)handoff->disk_cache_phys_base;
    g_cache_size = handoff->disk_cache_byte_size;
    g_lba_start = handoff->disk_cache_lba_start;
    g_lba_count = handoff->disk_cache_lba_count;
    g_block_size = handoff->disk_cache_block_size;
    g_ready = 1;
}

int stage2_disk_ready(void) {
    return g_ready;
}

u32 stage2_disk_block_size(void) {
    return g_block_size;
}

u64 stage2_disk_block_count(void) {
    return g_lba_count;
}

const u8 *stage2_disk_lba_ptr(u64 lba) {
    u8 *p = stage2_disk_lba_ptr_rw(lba);
    return (const u8 *)p;
}

u8 *stage2_disk_lba_ptr_rw(u64 lba) {
    u64 rel_lba;
    u64 byte_offset;

    if (!g_ready) {
        return (u8 *)0;
    }
    if (lba < g_lba_start) {
        return (u8 *)0;
    }

    rel_lba = lba - g_lba_start;
    if (rel_lba >= g_lba_count) {
        return (u8 *)0;
    }

    byte_offset = rel_lba * (u64)g_block_size;
    if (byte_offset >= g_cache_size) {
        return (u8 *)0;
    }

    return g_cache_base + byte_offset;
}

int stage2_disk_read_blocks(u64 lba, u32 count, void *out) {
    const u8 *src;
    u8 *dst = (u8 *)out;
    u64 total_bytes;

    if (!g_ready || !out || count == 0) {
        return 0;
    }

    src = stage2_disk_lba_ptr(lba);
    if (!src) {
        return 0;
    }

    total_bytes = (u64)count * (u64)g_block_size;
    if (total_bytes > g_cache_size) {
        return 0;
    }

    for (u64 i = 0; i < total_bytes; i++) {
        dst[i] = src[i];
    }

    return 1;
}

int stage2_disk_write_blocks(u64 lba, u32 count, const void *in) {
    u8 *dst;
    const u8 *src = (const u8 *)in;
    u64 total_bytes;

    if (!g_ready || !in || count == 0) {
        return 0;
    }

    dst = stage2_disk_lba_ptr_rw(lba);
    if (!dst) {
        return 0;
    }

    total_bytes = (u64)count * (u64)g_block_size;
    if (total_bytes > g_cache_size) {
        return 0;
    }

    for (u64 i = 0; i < total_bytes; i++) {
        dst[i] = src[i];
    }

    return 1;
}
