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
