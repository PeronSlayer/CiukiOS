/* FCB cursors advance in place; handle DTAs carry independent positions.
 * Their identical query must not merge those two lifetime contracts. */
static void merged_search_tests(void) {
    unsigned before=checks;
    for (unsigned fcb_first=0;fcb_first<2;fcb_first++) {
        setup(); close_file(create_file("C:/MIX0.DAT")); close_file(create_file("C:/MIX1.DAT"));
        h50_fcb_name(0x800,"MIX?    DAT",false);
        struct dos_file_frame f;
        if (fcb_first) {
            set_dta(0x1000); f=h50_fcb(0x11,0x800,0); CHECK(!(f.ax&255));
        }
        set_dta(0x900); f=path_call(0x4e00,"C:/MIX?.DAT"); CHECK(!(f.flags&1));
        uint8_t token[43]; memcpy(token,guest+0x50900,sizeof(token));
        if (!fcb_first) {
            set_dta(0x1000); f=h50_fcb(0x11,0x800,0); CHECK(!(f.ax&255));
        }
        CHECK(cursor_count()==2);
        /* Reusing the handle query cannot move the FCB's current position. */
        set_dta(0x900); f=path_call(0x4e00,"C:/MIX?.DAT"); CHECK(!(f.flags&1) && cursor_count()==2);
        set_dta(0x1000); f=h50_fcb(0x12,0x800,0);
        CHECK(!(f.ax&255) && !memcmp(guest+0x51001,"MIX1    DAT",11));
        f=h50_fcb(0x12,0x800,0); CHECK((f.ax&255)==255 && cursor_count()==1);
        /* FCB EOF must leave the earlier copied handle DTA authoritative. */
        set_dta(0x900); memcpy(guest+0x50900,token,sizeof(token)); f=call(frame(0x4f00));
        CHECK(!(f.flags&1) && !strcmp((char *)guest+0x5091e,"MIX1.DAT"));
        teardown();
    }
    printf("PASS merged FCB/handle search checks=%u both creation orders, independent positions, copied DTA after FCB EOF\n",checks-before);
}

static void merged_discard_identity_tests(void) {
    unsigned before=checks;
    format(); static struct dos_file_volume private;
    OK(cache_init(&cache,65536)); OK(dos_file_volume_open(&private,&cache,&device,true));
    int h=vfs_open(&private.table,"C:/PINNED.DAT",VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0); CHECK(h>=0);
    struct fat_entry entry=private.table.handles[h]->node->entry;
    struct vfs_file_identity *identity;
    OK(vfs_identity_get(&private.space,&private.fat,&entry,&identity));
    struct vfs_page_identity *page;
    OK(vfs_page_get(&private.space,identity,0,&page));
    uint8_t data[512]={0x84}; size_t done=99; fail_write=1;
    CHECK(vfs_write(&private.table,h,data,sizeof(data),&done)==-FS_EIO && !done && private.fat.readonly);
    CHECK(dos_file_volume_close(&private)==-FS_EBUSY && !private.table_ready && private.mounted);
    unsigned io=reads+writes+barriers;
    CHECK(dos_file_volume_discard(&private)==-FS_EBUSY && private.mounted);
    vfs_page_put(&private.space,page); /* identity reference alone blocks discard */
    CHECK(dos_file_volume_discard(&private)==-FS_EBUSY && private.mounted);
    OK(vfs_page_get(&private.space,identity,0,&page));
    vfs_identity_put(&private.space,identity); /* page reference alone blocks it */
    CHECK(dos_file_volume_discard(&private)==-FS_EBUSY && private.mounted);
    CHECK(reads+writes+barriers==io);
    vfs_page_put(&private.space,page);
    OK(dos_file_volume_discard(&private));
    CHECK(!private.mounted && !private.fat.mounted && !identity->valid &&
        !private.fat.identity_changed && !private.fat.identity_ctx && reads+writes+barriers==io);
    cache_destroy(&cache); CHECK(allocations_live==resident_allocations && !fs_host_contig_live);
    printf("PASS merged private discard/identity checks=%u retained identity and page references refuse, quiescent discard does zero I/O\n",checks-before);
}
