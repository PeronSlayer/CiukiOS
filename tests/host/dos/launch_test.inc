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
    CHECK(dosvm_create(&config,&v)==-ENOENT && !v); empty();
    reference_present=true;reference_checksum_missing=true;
    CHECK(dosvm_create(&config,&v)==-ENOENT && !v); empty();
    reference_checksum_missing=false;memset(reference_identity,'0',64);reference_identity[64]='\n';
    CHECK(dosvm_create(&config,&v)==-FS_EIO && !v); empty();
    memcpy(reference_identity,hash,64);
    CHECK(!dosvm_create(&config,&v));
    CHECK(v->disable_file_bridge && strstr(v->overlay_path,"G1K1") && strstr(v->overlay_path,hash));
    struct dosvm_key previous=dosvm_key(v);destroy(v,false);empty();
    CHECK(!dosvm_valid(previous));
    CHECK(!dosvm_create(&config,&v) && v->info.key.generation!=previous.generation);
    destroy(v,false);empty();
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
    CHECK(v && v->game==2 && !v->kernel && !v->disable_file_bridge && !strcmp(last_base,DOSVM_BUNDLED_BASE));
    CHECK(!strcmp(v->base_sha256,expected) && strstr(v->overlay_path,"G2K0") && strstr(v->overlay_path,expected));
    CHECK(strstr(game_launch_record,"probe=vm-window event=DATA group=game_launch game=2 kernel=ciukidos base_sha256=") &&
          strstr(game_launch_record,expected) && strstr(game_launch_record,"bridge=1"));
    const uint32_t probes[]={1,127,128,sectors-17,sectors-1};
    for (unsigned i=0;i<sizeof(probes)/sizeof(probes[0]);i++) {
        uint8_t want[512]; memset(want,probes[i]%251,512);
        CHECK(!dosvm_disk_read(v,probes[i],sector) && !memcmp(sector,want,512));
    }
    struct ciuki_dosvm_command shut=command_for(w.id,w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE,&shut,sizeof(shut))); empty();
    fake_current=saved; bundled_bytes=saved_bytes; disk_base_sector=saved_sector;
    puts("f3-51 game window base hash: PASS 27 MB games disk read once in 64 KiB blocks, digest, COW name, cache");
}
