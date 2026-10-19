/* Production RO metadata/alias loops, conservative modeled T23 CPU cost.
 * Cache-only page traversal: 1 us/page, UTF-8 comparison: 64 us (max name),
 * two node copies: 2 us. No storage transport or hardware timing claim. */
#include <ciuki/fs_ro.h>
#include <stdio.h>
#include <assert.h>
static uint64_t total,span,longest;
static bool points;
static unsigned enumerated;
static void model_charge(uint64_t ns) { total+=ns; span+=ns; }
static void boundary(void) { if (span>longest) longest=span; span=0; }
static void model_service(void) { if (points) { model_charge(200); boundary(); } }
static bool model_equal(const char *a,const char *b) { model_charge(64000); return path_equal(a,b); }
#define fs_service model_service
#include "bounded_names.inc"
static uint32_t generation(struct fs_ro_volume *v) { (void)v; return 1; }
static int readdir_names(struct fs_ro_volume *v,uint64_t dir,struct fs_ro_cursor *c,struct fs_ro_node *n) {
    (void)v; assert(dir==1); enumerated++;
    unsigned i=(unsigned)c->opaque[0]; if (i==FS_RO_ENTRIES) return -FS_ENOENT; c->opaque[0]++;
    memset(n,0,sizeof(*n)); n->id=FS_RO_ENTRIES-i+2;
    snprintf(n->name,sizeof(n->name),"Collision metadata name %03u.txt",FS_RO_ENTRIES-i);
    return 0;
}
int main(void) {
    struct blkdev device={0};
    static const struct fs_ro_ops ops={.generation=generation,.readdir=readdir_names};
    uint64_t before=0,work=0;
    for (unsigned mode=0;mode<2;mode++) {
        struct fs_ro_volume ro={.ops=&ops,.dev=&device,.mounted=true,.mount_generation=1};
        points=mode; total=span=longest=0; enumerated=0;
        struct fs_ro_view *names; assert(!view(&ro,1,&names) && names->count==FS_RO_ENTRIES);
        boundary(); assert(enumerated==FS_RO_ENTRIES+1);
        if (!mode) { before=longest; work=total; }
        else {
            assert(longest<=2000000 && before>100000000 && total<=work*110/100);
            printf("H49 metadata/aliases: entries=%u negative_span_ns=%llu span_ns=%llu work_ns=%llu->%llu PASS\n",
                FS_RO_ENTRIES,(unsigned long long)before,(unsigned long long)longest,
                (unsigned long long)work,(unsigned long long)total);
        }
        /* Cached replay does not enumerate or allocate a second map. */
        struct fs_ro_view *again; assert(!view(&ro,1,&again) && again==names && enumerated==FS_RO_ENTRIES+1);
        while (ro.views) { struct fs_ro_view *old=ro.views; ro.views=old->next; view_free(old); }
    }
    return 0;
}
