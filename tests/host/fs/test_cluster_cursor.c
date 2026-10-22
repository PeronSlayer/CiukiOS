/* f3-51c: per-open-file FAT cluster cursor and VFS write generations on a
 * production FAT32 volume (4 KiB clusters, as C:) whose 27,336,704-byte
 * games disk was written by mtools. FAT lookups are counted at the block
 * cache: every FAT entry decode reads its FAT sector through
 * cache_read_ahead (linked with --wrap). SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include "fake.h"
#include <ciuki/fat_native.h>
#include <ciuki/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#define GAME "C:/GAMES/CIUKIGAM.IMG"
#define GAME_BYTES 27336704u
#define BLOCK 65536u
static struct fake f;
static struct block_cache cache;
static struct fat_volume vol;
static struct vfs space;
static struct vfs_table t, w;
static uint64_t lookups, fat_lo, fat_hi;
int __real_cache_read_ahead(struct block_cache *, struct blkdev *, uint64_t, uint32_t, void *);
int __wrap_cache_read_ahead(struct block_cache *c, struct blkdev *d, uint64_t l, uint32_t n, void *b) {
    if (d==&f.dev && l>=fat_lo && l<fat_hi) lookups++;
    return __real_cache_read_ahead(c,d,l,n,b);
}
static uint64_t now_ns(void) { struct timespec s; clock_gettime(CLOCK_MONOTONIC,&s); return (uint64_t)s.tv_sec*1000000000u+(uint64_t)s.tv_nsec; }
/* Same formula as the generator in scripts/test/host_fs_tests.sh. */
static uint8_t game_byte(uint64_t i) { uint64_t s=i>>9; return (uint8_t)(s*131u+(s>>8)+(i&511u)*7u); }
static uint8_t frag_byte(uint32_t generation, uint64_t i) { return (uint8_t)(i*13u+(i>>12)*29u+generation*101u); }
static void bounds(void) { fat_lo=vol.start+vol.reserved; fat_hi=fat_lo+(uint64_t)vol.fats*vol.fat_sectors; }
static void mount(void) {
    CHECK(!fat_mount(&vol,&cache,&f.dev,0,f.dev.capacity,true) && !vol.readonly && vol.type==32 && vol.spc==8);
    CHECK(!vfs_attach(&space,2,&vol)); bounds();
}
static struct vfs_description *desc(struct vfs_table *table, int h) { return table->handles[h]; }
struct pass { uint64_t lookups, ns; char sha[65]; uint32_t reads; };
/* legacy: the pre-f3-51c demand+stream path, no cursor anywhere (the entry
 * was decoded by the open, so it carries no write cursor either). */
static struct pass sequential(uint32_t limit, bool legacy) {
    int h=vfs_open(&t,GAME,VFS_READ,VFS_DENY_WRITE,0); CHECK(h>=0);
    struct vfs_description *d=desc(&t,h);
    CHECK(d->node->entry.size==GAME_BYTES && !d->node->entry.write_cursor.cluster);
    static uint8_t buf[BLOCK];
    struct sha256_ctx ctx; sha256_init(&ctx); struct pass p={0};
    uint64_t begin=lookups, start=now_ns();
    for (uint32_t pos=0;pos<limit;) {
        size_t n=limit-pos<BLOCK ? limit-pos : BLOCK, done=0;
        if (legacy) CHECK(!fat_read_stream(&vol,&d->node->entry,pos,buf,n,&done,&d->stream,0));
        else CHECK(!vfs_read(&t,h,buf,n,&done));
        CHECK(done==n);
        for (size_t i=0;i<n;i+=509) CHECK(buf[i]==game_byte(pos+i));
        sha256_update(&ctx,buf,n); pos+=(uint32_t)n; p.reads++;
        while (cache_prefetch_tick(&cache)) { }
    }
    p.ns=now_ns()-start; p.lookups=lookups-begin;
    uint8_t digest[32]; sha256_final(&ctx,digest); sha256_hex(digest,p.sha);
    CHECK(!vfs_close(&t,h)); return p;
}
static void sequential_reads(const char *expected) {
    const uint32_t clusters=GAME_BYTES/4096u, half=GAME_BYTES/2u&~(BLOCK-1);
    struct pass before=sequential(GAME_BYTES,true), after=sequential(GAME_BYTES,false);
    struct pass before_half=sequential(half,true), after_half=sequential(half,false);
    CHECK(!strcmp(before.sha,expected) && !strcmp(after.sha,expected));
    /* Linear: about two lookups per cluster (the run's contiguity checks
     * plus the stream planner's window), versus a walk from the first
     * cluster for every 64 KiB read before. */
    CHECK(after.lookups<=3u*clusters && after.lookups>=clusters);
    CHECK(before.lookups>=50u*after.lookups);
    CHECK(after.lookups*100u<=after_half.lookups*215u && before.lookups*100u>=before_half.lookups*350u);
    /* Launch identity: a cached relaunch only reads the version. */
    int h=vfs_open(&t,GAME,VFS_READ,VFS_DENY_WRITE,0); CHECK(h>=0);
    struct vfs_file_version version; uint64_t start=now_ns();
    CHECK(!vfs_handle_version(&t,h,&version)); uint64_t cached_ns=now_ns()-start;
    CHECK(version.size==GAME_BYTES && !vfs_close(&t,h));
    printf("TRACE f3-51c sequential bytes=%u clusters=%u reads=%u sha256=%s\n",GAME_BYTES,clusters,after.reads,after.sha);
    printf("TRACE f3-51c fat_lookups before=%llu after=%llu half_before=%llu half_after=%llu per_cluster_after=%.3f\n",
        (unsigned long long)before.lookups,(unsigned long long)after.lookups,
        (unsigned long long)before_half.lookups,(unsigned long long)after_half.lookups,(double)after.lookups/clusters);
    printf("TRACE f3-51c launch_hash_ms before=%.1f after=%.1f cached_version_us=%.1f (host, ASan/UBSan -O1)\n",
        before.ns/1e6,after.ns/1e6,cached_ns/1e3);
    puts("PASS f3-51c 27 MB sequential FAT32 read: identical SHA-256, linear FAT lookups with the open-file cursor");
}
/* Two files appended alternately: FRAG's chain is fragmented (no two
 * neighbours adjacent), FILL takes every other cluster. */
static void fragmented(uint32_t generation, int a) {
    int b=vfs_open(&w,"C:/FILL.BIN",VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0); CHECK(b>=0);
    uint64_t position; CHECK(!vfs_seek(&w,b,0,VFS_SEEK_END,&position));
    static uint8_t chunk[4096];
    for (unsigned i=0;i<64;i++) {
        size_t done;
        for (unsigned j=0;j<4096;j++) chunk[j]=frag_byte(generation,(uint64_t)i*4096u+j);
        CHECK(!vfs_write(&w,a,chunk,4096,&done) && done==4096);
        memset(chunk,0xF1,sizeof(chunk)); CHECK(!vfs_write(&w,b,chunk,4096,&done) && done==4096);
    }
    CHECK(!vfs_close(&w,b));
}
static void read_check(int h, uint64_t pos, size_t n, uint32_t generation) {
    static uint8_t buf[16384]; uint64_t at; size_t done;
    CHECK(n<=sizeof(buf) && !vfs_seek(&t,h,(int64_t)pos,VFS_SEEK_SET,&at) && !vfs_read(&t,h,buf,n,&done) && done==n);
    for (size_t i=0;i<n;i++) CHECK(buf[i]==frag_byte(generation,pos+i));
}
static uint32_t cluster_of(const struct fat_entry *e, uint32_t index) {
    uint32_t c=e->first;
    for (uint32_t i=0;i<index;i++) CHECK(!fat_get_cluster(&vol,c,&c));
    return c;
}
static void cursor_rules(void) {
    int a=vfs_open(&w,"C:/FRAG.BIN",VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0); CHECK(a>=0);
    fragmented(0,a);
    int r=vfs_open(&t,"C:/FRAG.BIN",VFS_READ,VFS_DENY_NONE,0); CHECK(r>=0);
    struct vfs_description *rd=desc(&t,r); struct vfs_node *node=rd->node;
    CHECK(node==desc(&w,a)->node && node->entry.size==64u*4096u);
    unsigned adjacent=0;
    for (uint32_t i=0,c=node->entry.first,n;i+1<64;i++,c=n) { CHECK(!fat_get_cluster(&vol,c,&n)); adjacent+=n==c+1; }
    CHECK(!adjacent);
    /* Unaligned sequential reads over a fragmented chain stay linear. */
    uint64_t begin=lookups; uint64_t at; CHECK(!vfs_seek(&t,r,0,VFS_SEEK_SET,&at));
    static uint8_t buf[3000]; uint64_t pos=0;
    while (pos<node->entry.size) {
        size_t done; CHECK(!vfs_read(&t,r,buf,sizeof(buf),&done) && done);
        for (size_t i=0;i<done;i++) CHECK(buf[i]==frag_byte(0,pos+i));
        pos+=done; while (cache_prefetch_tick(&cache)) { }
    }
    uint64_t sequential=lookups-begin; CHECK(sequential<=4u*64u);
    CHECK(rd->cursor.index==63 && rd->cursor.cluster==cluster_of(&node->entry,63));
    /* Backward: from the first cluster; forward: from the cursor. */
    read_check(r,40u*4096u+7u,100,0); begin=lookups;
    read_check(r,10u*4096u+5u,100,0); uint64_t backward=lookups-begin;
    CHECK(rd->cursor.index==10 && backward>=10);
    begin=lookups; read_check(r,11u*4096u+1u,100,0); uint64_t forward=lookups-begin;
    CHECK(forward<=2 && rd->cursor.index==11);
    /* In-place overwrite: no FAT mutation, the cursor stays valid. */
    uint64_t generation=node->entry.chain_generation; CHECK(rd->cursor.generation==generation);
    uint8_t same[10]; for (unsigned i=0;i<10;i++) same[i]=frag_byte(0,20u*4096u+i);
    size_t done; CHECK(!vfs_seek(&w,a,20*4096,VFS_SEEK_SET,&at) && !vfs_write(&w,a,same,10,&done) && done==10);
    CHECK(node->entry.chain_generation==generation);
    begin=lookups; read_check(r,12u*4096u,100,0); CHECK(lookups-begin<=2);
    /* Append: the FAT_P_LINK group mutates the chain; the cursor retires. */
    uint8_t tail[4096]; for (unsigned i=0;i<4096;i++) tail[i]=frag_byte(0,64u*4096u+i);
    CHECK(!vfs_seek(&w,a,0,VFS_SEEK_END,&at) && !vfs_write(&w,a,tail,4096,&done) && done==4096);
    CHECK(node->entry.chain_generation!=generation && rd->cursor.generation==generation);
    begin=lookups; read_check(r,50u*4096u,100,0); CHECK(lookups-begin>=50);
    /* Truncate and reallocation reusing the same first cluster with a
     * shifted chain: a stale cursor would read another cluster's bytes. */
    read_check(r,30u*4096u,4096,0);
    struct fat_cursor stale=rd->cursor; uint32_t first=node->entry.first, old31=cluster_of(&node->entry,31);
    CHECK(stale.index==30);
    CHECK(!vfs_truncate(&w,a,0) && node->entry.chain_generation!=stale.generation && !node->entry.first);
    uint8_t chunk[4096];
    for (unsigned j=0;j<4096;j++) chunk[j]=frag_byte(1,j);
    CHECK(!vfs_seek(&w,a,0,VFS_SEEK_SET,&at) && !vfs_write(&w,a,chunk,4096,&done) && done==4096);
    int gap=vfs_open(&w,"C:/GAP.BIN",VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0); CHECK(gap>=0);
    memset(chunk,0x6A,sizeof(chunk));
    for (unsigned i=0;i<3;i++) CHECK(!vfs_write(&w,gap,chunk,4096,&done) && done==4096);
    CHECK(!vfs_close(&w,gap));
    for (unsigned i=1;i<64;i++) {
        for (unsigned j=0;j<4096;j++) chunk[j]=frag_byte(1,(uint64_t)i*4096u+j);
        CHECK(!vfs_write(&w,a,chunk,4096,&done) && done==4096);
    }
    CHECK(node->entry.first==first && cluster_of(&node->entry,31)!=old31); /* adversarial reuse */
    CHECK(rd->cursor.first==first && rd->cursor.generation!=node->entry.chain_generation);
    read_check(r,31u*4096u,4096,1); read_check(r,32u*4096u,4096,1);
    /* A forged cursor with a foreign generation or an invalid cluster is
     * never followed; a fresh copy decoded from disk has its own stamp. */
    struct fat_entry fresh; CHECK(!fat_lookup(&vol,vol.root,"FRAG.BIN",&fresh));
    CHECK(fresh.first==first && fresh.chain_generation!=node->entry.chain_generation);
    struct fat_cursor forged=rd->cursor; static uint8_t big[4096];
    begin=lookups; CHECK(!fat_read_cursor(&vol,&fresh,&forged,40u*4096u,big,4096,&done) && done==4096);
    for (unsigned i=0;i<4096;i++) CHECK(big[i]==frag_byte(1,40u*4096u+i));
    CHECK(lookups-begin>=40 && forged.generation==fresh.chain_generation && forged.index==40);
    forged.cluster=vol.clusters+2; begin=lookups;
    CHECK(!fat_read_cursor(&vol,&fresh,&forged,41u*4096u,big,4096,&done) && lookups-begin>=41);
    forged=(struct fat_cursor){.generation=fresh.chain_generation+1,.first=first,.index=42,.cluster=first};
    CHECK(!fat_read_cursor(&vol,&fresh,&forged,43u*4096u,big,4096,&done));
    for (unsigned i=0;i<4096;i++) CHECK(big[i]==frag_byte(1,43u*4096u+i));
    /* The entry's write cursor is also a resolution hint for the writer. */
    CHECK(node->entry.write_cursor.index==63 && node->entry.write_cursor.generation==node->entry.chain_generation);
    CHECK(!vfs_close(&t,r) && !vfs_close(&w,a));
    /* Reserved log tail cut (publication), removal and rename replacement
     * restamp the chain. */
    int log=vfs_open(&w,"C:/CURSOR.LOG",VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0); CHECK(log>=0);
    CHECK(!vfs_log_reserve(&w,log,16u*4096u));
    memset(chunk,'L',sizeof(chunk)); CHECK(!vfs_log_write(&w,log,chunk,4096,4096,&done) && done==4096);
    generation=desc(&w,log)->node->entry.chain_generation;
    CHECK(!vfs_log_publish(&w,log,true) && desc(&w,log)->node->entry.chain_generation!=generation);
    CHECK(!vfs_close(&w,log));
    struct fat_entry victim; CHECK(!fat_lookup(&vol,vol.root,"GAP.BIN",&victim));
    generation=victim.chain_generation; CHECK(!fat_remove(&vol,&victim,false) && victim.chain_generation!=generation);
    struct fat_entry from, over;
    CHECK(!fat_lookup(&vol,vol.root,"CURSOR.LOG",&from) && !fat_lookup(&vol,vol.root,"FILL.BIN",&over));
    generation=over.chain_generation;
    CHECK(!fat_rename_replace(&vol,&from,&over,vol.root,"FILL.BIN",false) && !over.first && over.chain_generation!=generation);
    puts("PASS f3-51c cursor: fragmented linear reads, backward fallback, in-place keep, append/truncate/relocation/log cut/remove/replace restamp, stale and forged cursors ignored");
}
static struct vfs_file_version version_of(const char *path) {
    int h=vfs_open(&t,path,VFS_READ,VFS_DENY_WRITE,0); CHECK(h>=0);
    struct vfs_file_version v; CHECK(!vfs_handle_version(&t,h,&v)); CHECK(!vfs_close(&t,h)); return v;
}
static void write_file(const char *path, uint32_t bytes, uint8_t seed, bool keep_times, const struct vfs_file_version *times) {
    int h=vfs_open(&w,path,VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0); CHECK(h>=0);
    static uint8_t data[65536]; for (uint32_t i=0;i<bytes;i++) data[i]=(uint8_t)(i*3u+seed);
    size_t done; CHECK(!vfs_write(&w,h,data,bytes,&done) && done==bytes);
    if (keep_times) CHECK(!vfs_handle_times(&w,h,times->write_date,times->write_time));
    CHECK(!vfs_close(&w,h));
}
static void write_generations(void) {
    const char *small="C:/GAMES/SMALL.IMG";
    write_file(small,65536,7,false,0);
    struct vfs_file_version v0=version_of(small), v=version_of(small);
    CHECK(vfs_file_version_equal(&v0,&v) && v0.size==65536 && v0.drive==2);
    /* An unrelated file's writes (the VM's COW overlay) keep the version. */
    write_file("C:/OVERLAY.COW",4096,1,false,0); v=version_of(small); CHECK(vfs_file_version_equal(&v0,&v));
    /* Rewriting the same bytes and restoring the time: only the write
     * generation tells the versions apart. */
    write_file(small,65536,7,true,&v0); v=version_of(small);
    CHECK(v.size==v0.size && v.first==v0.first && v.index==v0.index && v.write_date==v0.write_date &&
          v.write_time==v0.write_time && v.write_generation!=v0.write_generation);
    /* Delete and recreate identical bytes and time: same slot, same first
     * cluster, same size and time; the write generation still differs. */
    struct vfs_file_version v1=v;
    CHECK(!vfs_delete(&t,small)); write_file(small,65536,7,true,&v1); v=version_of(small);
    CHECK(v.index==v1.index && v.first==v1.first && v.size==v1.size && v.write_date==v1.write_date &&
          v.write_time==v1.write_time && v.mount==v1.mount && !vfs_file_version_equal(&v,&v1));
    /* Time-only change, then truncate. */
    v1=v;
    int h=vfs_open(&w,small,VFS_READ|VFS_WRITE,VFS_DENY_NONE,0); CHECK(h>=0);
    CHECK(!vfs_handle_times(&w,h,v1.write_date,(uint16_t)(v1.write_time^1))); CHECK(!vfs_close(&w,h));
    v=version_of(small); CHECK(!vfs_file_version_equal(&v,&v1) && v.write_generation==v1.write_generation);
    h=vfs_open(&w,small,VFS_READ|VFS_WRITE,VFS_DENY_NONE,0); CHECK(h>=0 && !vfs_truncate(&w,h,65024) && !vfs_close(&w,h));
    v=version_of(small); CHECK(v.size==65024 && v.write_generation!=v1.write_generation);
    /* Rename over the base. */
    struct vfs_file_version v2=v;
    write_file("C:/GAMES/NEW.IMG",65024,7,true,&v2);
    struct vfs_file_version v3=version_of(small); CHECK(vfs_file_version_equal(&v3,&v2));
    struct fat_entry dir, from, over;
    CHECK(!fat_lookup(&vol,vol.root,"GAMES",&dir) && !fat_lookup(&vol,dir.first,"NEW.IMG",&from) &&
          !fat_lookup(&vol,dir.first,"SMALL.IMG",&over));
    CHECK(!fat_rename_replace(&vol,&from,&over,dir.first,"SMALL.IMG",false));
    v=version_of(small); CHECK(!vfs_file_version_equal(&v,&v3));
    /* Raw/external change of the volume (identity expiry). */
    v3=v; vfs_raw_changed(&space,&vol); v=version_of(small);
    CHECK(!vfs_file_version_equal(&v,&v3) && v.write_generation>v3.write_generation);
    /* Remount: a new attachment. */
    v3=v; CHECK(!vfs_detach(&space,2)); mount(); v=version_of(small);
    CHECK(v.mount!=v3.mount && v.first==v3.first && v.size==v3.size && v.index==v3.index);
    /* Bounded ledger: the base stays listed through 63 newer changes; the
     * 64th evicts it and the floor becomes its own sequence (same version);
     * the next eviction raises the floor: a conservative version change. */
    write_file(small,512,9,false,0); struct vfs_file_version v4=version_of(small);
    char path[32];
    for (unsigned i=0;i<=VFS_CHANGES;i++) {
        snprintf(path,sizeof(path),"C:/L%02u.BIN",i); write_file(path,512,(uint8_t)i,false,0);
        v=version_of(small);
        CHECK(vfs_file_version_equal(&v,&v4)==(i<VFS_CHANGES));
    }
    CHECK(v.write_generation>v4.write_generation);
    CHECK(vfs_handle_version(&t,77,&v)==-FS_EBADF);
    puts("PASS f3-51c write generations: same-bytes rewrite, delete+recreate in the same slot/cluster/time, time, truncate, rename over, raw change and remount change the version; unrelated writes do not; ledger eviction is conservative");
}
/* A digest cache pins only a counter, so unrelated ledger eviction must not
 * trigger another complete image hash or deny a later legitimate writer. */
static void digest_version_watches(void) {
    const char *base="C:/GAMES/SMALL.IMG";
    struct vfs_file_version before=version_of(base), after;
    struct vfs_version_pin pin={0}, second={0};
    int h=vfs_open(&t,base,VFS_READ,VFS_DENY_WRITE,0); CHECK(h>=0);
    CHECK(!vfs_version_pin(&t,h,&before,&pin) && pin.serial);
    CHECK(!vfs_version_pin(&t,h,&before,&second) && second.serial==pin.serial);
    CHECK(!vfs_close(&t,h));
    char path[32];
    for (unsigned i=0;i<2*VFS_CHANGES;i++) {
        snprintf(path,sizeof(path),"C:/W%03u.BIN",i); write_file(path,512,(uint8_t)i,false,0);
        after=version_of(base); CHECK(vfs_file_version_equal(&before,&after));
    }
    write_file(base,512,9,true,&before); after=version_of(base);
    CHECK(!vfs_file_version_equal(&before,&after) && after.write_generation>before.write_generation);
    h=vfs_open(&t,base,VFS_READ,VFS_DENY_WRITE,0); CHECK(h>=0);
    struct vfs_version_pin stale={0}; CHECK(vfs_version_pin(&t,h,&before,&stale)==-FS_EAGAIN && !stale.serial);
    CHECK(!vfs_close(&t,h));
    vfs_version_unpin(&pin); CHECK(!pin.vfs && !pin.serial);
    vfs_version_unpin(&pin); /* idempotent */
    before=version_of(base);
    vfs_raw_changed(&space,&vol); after=version_of(base);
    CHECK(!vfs_file_version_equal(&before,&after));
    vfs_version_unpin(&second);
    struct vfs_version_pin full[VFS_VERSION_PINS]={0};
    for (unsigned i=0;i<=VFS_VERSION_PINS;i++) {
        snprintf(path,sizeof(path),"C:/W%03u.BIN",i);
        h=vfs_open(&t,path,VFS_READ,VFS_DENY_WRITE,0); CHECK(h>=0);
        before=version_of(path);
        struct vfs_version_pin extra={0};
        int e=vfs_version_pin(&t,h,&before,i<VFS_VERSION_PINS ? &full[i] : &extra);
        CHECK(e==(i<VFS_VERSION_PINS ? 0 : -FS_ENOSPC)); CHECK(!vfs_close(&t,h));
    }
    for (unsigned i=0;i<VFS_VERSION_PINS;i++) vfs_version_unpin(&full[i]);
    for (unsigned i=0;i<VFS_VERSION_PINS;i++) CHECK(!space.version_watches[i].refs);
    puts("PASS f3-63 digest watches: 128 unrelated writes keep the version, same-bytes write and raw change invalidate, bounded 16/refcounts/stale pin/release without open handles");
}
/* Last: a device error empties the open file's cursor (no later writes). */
static void error_path(void) {
    int r=vfs_open(&t,"C:/FRAG.BIN",VFS_READ,VFS_DENY_NONE,0); CHECK(r>=0);
    read_check(r,4096u,100,1); struct vfs_description *d=desc(&t,r); CHECK(d->cursor.cluster);
    uint32_t c=cluster_of(&d->node->entry,2);
    cache_invalidate(&cache,&f.dev,false);
    f.fail_read=vol.start+vol.data_sector+(uint64_t)(c-2)*vol.spc;
    uint64_t at; size_t done; static uint8_t buf[4096];
    CHECK(!vfs_seek(&t,r,2*4096,VFS_SEEK_SET,&at) && vfs_read(&t,r,buf,4096,&done)<0);
    CHECK(!d->cursor.cluster && !d->cursor.generation && !d->cursor.first);
    f.fail_read=UINT64_MAX; CHECK(!vfs_close(&t,r));
    puts("PASS f3-51c cursor emptied on a failed read");
}
int main(int argc, char **argv) {
    CHECK(argc==3 && strlen(argv[2])==64);
    CHECK(!fake_open(&f,argv[1]));
    CHECK(!cache_init(&cache,4u*1024u*1024u) && !cache_adaptive_enable(&cache,28672));
    vfs_init(&space); mount();
    CHECK(!vfs_table_init(&space,&t,2) && !vfs_table_init(&space,&w,2));
    sequential_reads(argv[2]);
    cursor_rules();
    write_generations();
    digest_version_watches();
    error_path();
    vfs_table_destroy(&t); vfs_table_destroy(&w);
    CHECK(!vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache); fake_close(&f);
    puts("RESULT f3-51c cluster cursor and write generations FAT32 failures=0");
    return 0;
}
