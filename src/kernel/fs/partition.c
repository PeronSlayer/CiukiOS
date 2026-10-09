/* EBR data offsets are relative to that EBR; links to the container base.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "partition.h"
static bool ext(uint8_t t) { return t==5 || t==15 || t==0x85; }
static bool overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a<b+m && b<a+n; }
static int decode(const uint8_t *p, uint64_t base, uint64_t cap, struct partition *v) {
    memset(v,0,sizeof(*v)); v->type=p[4];
    uint32_t off=fs_rd32(p+8), n=fs_rd32(p+12);
    if (!v->type) return off || n || p[0] ? -FS_EINVAL : 0;
    if (v->type==0xee) return -FS_EOPNOTSUPP;
    if (p[0]!=0 && p[0]!=0x80) return -FS_EINVAL;
    v->start=base+off; v->count=n; v->bootable=p[0]==0x80;
    if (!n || !v->start || v->start>=cap || n>cap-v->start || v->start+n>(1u<<28)) return -FS_EINVAL;
    return 0;
}
static int sector(struct blkdev *d, uint64_t lba, uint8_t *b) {
    int e=blkdev_range(d,lba,1); if (e) return e;
    if (!d->read) return -FS_EINVAL;
    if ((e=d->read(d,lba,1,b))) return e;
    return fs_rd16(b+510)==0xaa55 ? 0 : -FS_EINVAL;
}
static int add(struct partition_table *t, struct partition *v) {
    if (t->count==PARTITION_MAX) return -FS_ELOOP;
    for (unsigned i=0;i<t->count;i++) if (overlap(v->start,v->count,t->entries[i].start,t->entries[i].count)) return -FS_EINVAL;
    t->entries[t->count++]=*v; return 0;
}
int partition_scan(struct blkdev *d, struct partition_table *t) {
    uint8_t b[512]; struct partition primary[4]; uint64_t seen[PARTITION_MAX];
    unsigned nseen=0; int e; memset(t,0,sizeof(*t));
    if ((e=sector(d,0,b))) return e;
    for (unsigned i=0;i<4;i++) {
        if ((e=decode(b+446+i*16,0,d->capacity,&primary[i]))) goto fail;
        if (!primary[i].type) continue;
        for (unsigned j=0;j<i;j++) if (primary[j].type && overlap(primary[i].start,primary[i].count,primary[j].start,primary[j].count)) { e=-FS_EINVAL; goto fail; }
        if (!ext(primary[i].type) && (e=add(t,&primary[i]))) goto fail;
    }
    for (unsigned p=0;p<4;p++) {
        if (!ext(primary[p].type)) continue;
        uint64_t base=primary[p].start, end=base+primary[p].count, cur=base;
        for (;;) {
            if (nseen==PARTITION_MAX) { e=-FS_ELOOP; goto fail; }
            for (unsigned i=0;i<nseen;i++) if (seen[i]==cur) { e=-FS_ELOOP; goto fail; }
            for (unsigned i=0;i<t->count;i++) if (overlap(cur,1,t->entries[i].start,t->entries[i].count)) { e=-FS_EINVAL; goto fail; }
            seen[nseen++]=cur; t->ebr_reads++;
            if ((e=sector(d,cur,b))) goto fail;
            uint64_t next=0;
            for (unsigned i=0;i<4;i++) {
                struct partition v;
                if ((e=decode(b+446+i*16,ext(b[450+i*16]) ? base : cur,d->capacity,&v))) goto fail;
                if (!v.type) continue;
                if (v.start<base || v.start>=end || v.count>end-v.start) { e=-FS_EINVAL; goto fail; }
                if (ext(v.type)) { if (next) { e=-FS_EINVAL; goto fail; } next=v.start; }
                else {
                    for (unsigned j=0;j<nseen;j++) if (overlap(v.start,v.count,seen[j],1)) { e=-FS_EINVAL; goto fail; }
                    v.logical=true; if ((e=add(t,&v))) goto fail;
                }
            }
            if (!next) break;
            cur=next;
        }
    }
    return 0;
fail: t->count=0; return e;
}
