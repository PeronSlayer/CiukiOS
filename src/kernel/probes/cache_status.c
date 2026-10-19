/* H-page cache counters. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/storage.h>
#include <ciuki/hardware_check.h>
/* C4 H-page snapshot: no I/O, shared identities add no duplicate frames. */
void hardware_check_cache(struct storage *s, struct hw_check_result *out) {
    if (!s || !s->ready) { out->status=HW_SKIP; ksnprintf(out->detail,sizeof(out->detail),"storage unavailable"); return; }
    struct cache_stats stats; cache_stats(&s->cache,&stats);
    uint64_t accesses=stats.hits+stats.misses;
    unsigned rate=accesses ? (unsigned)(stats.hits*100/accesses) : 0;
    out->status=stats.speculative_bytes<=stats.pages*4096u/8 ? HW_PASS : HW_FAIL;
    ksnprintf(out->detail,sizeof(out->detail),"KiB=%u hit=%u%% grow=%u reclaim=%u useful/waste=%llu/%llu KiB",
        stats.pages*4,rate,stats.growths,stats.reclaims,stats.prefetch_useful/1024,stats.prefetch_wasted/1024);
    klog("[cache] pages=%u data=%u meta=%u work=%u floor=%u ceiling=%u speculative=%u peak=%u pinned=%u dirty=%u error=%u blocked=%u/%u slice_us=%u",
        stats.pages,stats.data_pages,stats.meta_pages,stats.workspace_pages+stats.writeback_pages,
        stats.floor_pages,stats.ceiling_pages,stats.speculative_bytes,stats.speculative_peak,
        stats.pinned_data,stats.dirty_data,stats.error_data,stats.unreclaimable_data,stats.data_pages,stats.max_slice_us);
}
