/* H51-06: production disk open/identity/namespace/lifetime with fake transport. */
static void test_kernel_choice(void)
{
    empty(); uint32_t saved_bytes=bundled_bytes;
    bundled_bytes=(DOSVM_BASE_MIN_SECTORS+8u)*512u;
    const char *base="C:\\GAMES\\CIUKIGAM.IMG";
    struct dosvm_config config={.base=base,.game=1};
    struct dosvm *v=0;
    CHECK(!dosvm_create(&config,&v));
    char first[128],hash[65]; strcpy(first,v->overlay_path); strcpy(hash,v->base_sha256);
    CHECK(strstr(first,"G1K0") && strstr(first,hash) && !v->disable_file_bridge);
    destroy(v,false); empty();
    config.game=2; CHECK(!dosvm_create(&config,&v));
    CHECK(strstr(v->overlay_path,"G2K0") && !strcmp(hash,v->base_sha256) && strcmp(first,v->overlay_path));
    destroy(v,false); empty();
    config.game=1; bundled_bytes+=512; CHECK(!dosvm_create(&config,&v));
    CHECK(strcmp(hash,v->base_sha256) && strcmp(first,v->overlay_path));
    strcpy(hash,v->base_sha256); destroy(v,false); empty();
    config.kernel=1;config.base="C:\\SYSTEM\\DOS\\FREEDOS.IMG";
    CHECK(dosvm_create(&config,&v)==-EINVAL && !v); empty();
    config.base="C:\\SYSTEM\\DOS\\DOSBOOT.IMG";
    CHECK(dosvm_create(&config,&v)==-EINVAL && !v); empty();
    /* New personality must not reuse reference sidecar or old COW identity,
     * even when their synthetic base contents happen to be equal. */
    config.kernel=2;config.base="C:\\SYSTEM\\DOS\\UNIFIED.IMG";unified_present=false;
    CHECK(dosvm_create(&config,&v)==-ENOENT && !v);empty();
    unified_present=true;unified_checksum_missing=true;
    CHECK(dosvm_create(&config,&v)==-ENOENT && !v);empty();
    unified_checksum_missing=false;memset(unified_identity,'0',64);unified_identity[64]='\n';
    CHECK(dosvm_create(&config,&v)==-FS_EIO && !v);empty();
    memcpy(unified_identity,hash,64);
    CHECK(!dosvm_create(&config,&v));
    CHECK(v->disable_file_bridge && strstr(v->overlay_path,"G1K2") && strstr(v->overlay_path,hash));
    CHECK(!strcmp(last_base,"C:\\SYSTEM\\DOS\\UNIFIED.IMG"));
    destroy(v,false);empty();unified_identity[0]=0;unified_present=true;
    reference_present=false;bundled_bytes=saved_bytes;
    puts("H51-06 kernel choice: PASS game/kernel/base-specific COW, missing/corrupt reference, close/relaunch ledgers");
}
/* Regression (f3-51 merge fe9a34ef): the desktop's DOOM launch sends CREATE
 * with launch.game=2, and the window stayed at "Preparing..." while the VMM
 * hashed the 27 MB games disk with one seek+512-byte read per sector (each
 * FAT read walks the cluster chain from the first cluster: O(n^2)). The whole
 * base must be read once, in order, in 64 KiB read-cache blocks, and the
 * reused block must not publish hashed bytes as cached guest sectors. */
static void test_game_window_base_hash(void)
{
    empty();
    uint32_t saved_bytes=bundled_bytes; void (*saved_sector)(uint32_t,void *)=disk_base_sector;
    bundled_bytes=27336704u; disk_base_sector=0; /* C:\GAMES\CIUKIGAM.IMG, failing image */
    const uint32_t sectors=bundled_bytes/512u, block=DOSVM_READ_SECTORS*512u;
    struct sha256_ctx ctx; uint8_t sector[512], digest[32]; char expected[65];
    sha256_init(&ctx);
    for (uint32_t lba=0;lba<sectors;lba++) {
        memset(sector,lba%251,512);
        if (!lba) { sector[0]=0xEE; sector[510]=bad_signature ? 0 : 0x55; sector[511]=0xAA; }
        sha256_update(&ctx,sector,512);
    }
    sha256_final(&ctx,digest); sha256_hex(digest,expected);
    struct process *saved=fake_current; fake_current=&fake_proc[0];
    unsigned reads=disk_reads, seeks=base_seeks;
    struct ciuki_dosvm_command w=command_for(0,0);
    w.surface_fd=0; memcpy(w.name,"DOOM",4); w.launch.game=2; /* \DOOM\DOOM.EXE, CiukiDOS default */
    CHECK(!dosvm_call(CIUKI_DOSVM_CREATE,&w,sizeof(w)) && w.id && w.state==CIUKI_DOSVM_BOOTING);
    /* Hash: one seek and ceil(size/64 KiB) reads; boot sector: one more each. */
    CHECK(base_seeks-seeks==2 && disk_reads-reads==(bundled_bytes+block-1)/block+1);
    struct dosvm *v=dosvm_lookup((struct dosvm_key){w.id,w.generation});
    CHECK(v && v->game==2 && v->kernel==2 && v->disable_file_bridge && !strcmp(last_base,DOSVM_DEFAULT_BASE));
    CHECK(!strcmp(v->base_sha256,expected) && strstr(v->overlay_path,"G2K2") && strstr(v->overlay_path,expected));
    CHECK(strstr(game_launch_record,"probe=vm-window event=DATA group=game_launch game=2 kernel=unified base_sha256=") &&
          strstr(game_launch_record,expected) && strstr(game_launch_record,"bridge=0"));
    const uint32_t probes[]={1,127,128,sectors-17,sectors-1};
    for (unsigned i=0;i<sizeof(probes)/sizeof(probes[0]);i++) {
        uint8_t want[512]; memset(want,probes[i]%251,512);
        CHECK(!dosvm_disk_read(v,probes[i],sector) && !memcmp(sector,want,512));
    }
    struct ciuki_dosvm_command shut=command_for(w.id,w.generation);
    CHECK(!finish_test_vm(&shut,sizeof(shut))); empty();
    fake_current=saved; bundled_bytes=saved_bytes; disk_base_sector=saved_sector;
    puts("f3-51 game window base hash: PASS 27 MB games disk read once in 64 KiB blocks, digest, COW name, cache");
}
/* f3-51c: the launch digest is cached per base content version for the
 * session. A cached launch keeps item 8's identity (digest, COW name,
 * FREEDOS.SHA check, game_launch record) and reads no base block; every
 * version change re-hashes; the cache is a bounded LRU. */
static unsigned launch_seeks, launch_reads;
static struct dosvm *digest_launch(unsigned game, unsigned kernel, const char *base, int expect, int cached)
{
    struct dosvm_config config={.base=base,.game=(uint8_t)game,.kernel=(uint8_t)kernel};
    struct dosvm *v=0; unsigned seeks=base_seeks, reads=disk_reads;
    game_launch_record[0]=0;
    CHECK(dosvm_create(&config,&v)==expect);
    launch_seeks=base_seeks-seeks; launch_reads=disk_reads-reads;
    if (expect) { CHECK(!v); empty(); return 0; }
    CHECK(v && v->base_cached==(cached!=0) && strstr(v->overlay_path,v->base_sha256));
    CHECK(strstr(game_launch_record,cached ? " cached=1" : " cached=0") && strstr(game_launch_record,v->base_sha256));
    return v;
}
static unsigned bump_at;
static void bump_during_hash(void) { if (version_calls==bump_at) base_writes++; } /* the post-hash call */
static void test_game_digest_cache(void)
{
    empty();
    uint32_t saved_bytes=bundled_bytes; void (*saved_sector)(uint32_t,void *)=disk_base_sector;
    bundled_bytes=(DOSVM_BASE_MIN_SECTORS+24u)*512u; disk_base_sector=0;
    const char *base="C:\\GAMES\\CIUKIGAM.IMG";
    const unsigned blocks=(bundled_bytes+DOSVM_READ_SECTORS*512u-1)/(DOSVM_READ_SECTORS*512u);
    char digest[65];
    /* First launch after boot hashes once; the relaunch reuses the digest. */
    struct dosvm *v=digest_launch(2,0,base,0,0); strcpy(digest,v->base_sha256);
    unsigned hashed_seeks=launch_seeks, hashed_reads=launch_reads; destroy(v,false); empty();
    v=digest_launch(2,0,base,0,1);
    CHECK(!strcmp(v->base_sha256,digest) && strstr(v->overlay_path,"G2K0"));
    CHECK(hashed_seeks-launch_seeks==1 && hashed_reads-launch_reads==blocks); /* no base block read */
    destroy(v,false); empty();
    v=digest_launch(1,0,base,0,1); CHECK(!strcmp(v->base_sha256,digest) && strstr(v->overlay_path,"G1K0"));
    destroy(v,false); empty();
    /* Each version change re-hashes once, then hits: write generation,
     * modification time, location (rename over / recreate), remount. */
    uint64_t *u64[]={&base_writes,&base_mount}; uint32_t *u32[]={&base_mtime,&base_location};
    for (unsigned i=0;i<4;i++) {
        if (i<2) (*u64[i])++; else (*u32[i-2])++;
        v=digest_launch(2,0,base,0,0); CHECK(!strcmp(v->base_sha256,digest) && launch_reads==hashed_reads);
        destroy(v,false); empty();
        v=digest_launch(2,0,base,0,1); destroy(v,false); empty();
    }
    /* Truncate/extend: new size, new digest and COW name. */
    bundled_bytes+=512;
    v=digest_launch(2,0,base,0,0); CHECK(strcmp(v->base_sha256,digest)); destroy(v,false); empty();
    v=digest_launch(2,0,base,0,1); CHECK(strcmp(v->base_sha256,digest)); destroy(v,false); empty();
    bundled_bytes-=512; base_writes++; /* the write restoring the former bytes */
    /* No version (read-only provider): hash at every launch, never cached. */
    version_error=-FS_EOPNOTSUPP;
    for (unsigned i=0;i<2;i++) { v=digest_launch(2,0,base,0,0); CHECK(!strcmp(v->base_sha256,digest)); destroy(v,false); empty(); }
    version_error=0;
    /* A version that changes across the hash is never stored. */
    unsigned calls=version_calls; base_writes++;
    bump_at=version_calls+2; version_hook=bump_during_hash;
    v=digest_launch(2,0,base,0,0); destroy(v,false); empty(); version_hook=0;
    CHECK(version_calls-calls==2);
    v=digest_launch(2,0,base,0,0); destroy(v,false); empty(); /* bumped version: hashed */
    v=digest_launch(2,0,base,0,1); destroy(v,false); empty();
    /* Bounded LRU of 16 versions: the 17th evicts the least recently used. */
    uint64_t first=base_writes+1;
    for (unsigned i=0;i<=DOSVM_DIGESTS;i++) { base_writes=first+i; v=digest_launch(2,0,base,0,0); destroy(v,false); empty(); }
    base_writes=first+1; v=digest_launch(2,0,base,0,1); destroy(v,false); empty();
    base_writes=first; v=digest_launch(2,0,base,0,0); destroy(v,false); empty(); /* evicted */
    base_writes=first+DOSVM_DIGESTS; v=digest_launch(2,0,base,0,1); destroy(v,false); empty();
    /* Unified sidecar is checked even when a digest is cached. */
    memcpy(unified_identity,digest,64); unified_identity[64]='\n';
    const char *unified="C:\\SYSTEM\\DOS\\UNIFIED.IMG";
    v=digest_launch(1,2,unified,0,0); CHECK(v->disable_file_bridge && strstr(v->overlay_path,"G1K2")); destroy(v,false); empty();
    v=digest_launch(1,2,unified,0,1); CHECK(!strcmp(v->base_sha256,digest)); destroy(v,false); empty();
    unified_identity[0]^=1; digest_launch(1,2,unified,-FS_EIO,1); unified_identity[0]=0;
    bundled_bytes=saved_bytes; disk_base_sector=saved_sector;
    puts("f3-51c game digest cache: PASS first launch hashes, relaunch reads no base block, write/mtime/location/mount/size invalidate, no version or unstable version never cached, LRU 16, FREEDOS.SHA still checked");
}
