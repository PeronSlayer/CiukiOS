/* Amendment 1: production FAT/VFS/requests; fault transports and guest/time
 * seams only. No second filesystem implementation. */
static unsigned cursor_count(void) {
    unsigned n=0; for (unsigned i=0;i<DOS_FILE_SEARCHES;i++) n+=bridge.cursor[i].used; return n;
}
static void set_dta(unsigned off) {
    struct dos_file_frame f=frame(0x1a00); f.dx=(uint16_t)off; f=call(f); CHECK(!(f.flags&1));
}
static void copied_search_tests(void) {
    setup();
    for (unsigned i=0;i<4;i++) { char name[32]; snprintf(name,sizeof(name),"C:/COPY%u.DAT",i); close_file(create_file(name)); }
    set_dta(0x800); struct dos_file_frame f=path_call(0x4e00,"C:/COPY?.DAT"); CHECK(!(f.flags&1));
    uint8_t first[43]; memcpy(first,guest+0x50800,43); memcpy(guest+0x50900,first,43);
    unsigned n=1; while (!((f=call(frame(0x4f00))).flags&1)) n++; CHECK(n==4 && f.ax==18);
    CHECK(cursor_count()==1); set_dta(0x900); f=call(frame(0x4f00)); CHECK(!(f.flags&1));
    CHECK(!strcmp((char *)guest+0x5091e,"COPY1.DAT"));
    for (unsigned i=0;i<100;i++) { f=path_call(0x4e00,"C:/COPY?.DAT"); CHECK(!(f.flags&1)); }
    CHECK(cursor_count()==1); /* identical queries share immutable authority */
    /* A child's lifecycle cannot revoke its parent's copies. */
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_BEGIN,0x3000,0x2000,expanded(0x3000)));
    OK(dos_file_lifecycle(&bridge,DOS_FILE_EXEC_COMMIT,0x3000,0,expanded(0x3000)));
    set_dta(0xa00); f=path_call(0x4e00,"C:/COPY?.DAT"); CHECK(!(f.flags&1) && cursor_count()==2);
    uint8_t child[43]; memcpy(child,guest+0x50a00,43);
    OK(dos_file_lifecycle(&bridge,DOS_FILE_PROCESS_EXIT,0x3000,0,expanded(0x3000)));
    CHECK(cursor_count()==1); set_dta(0x900); memcpy(guest+0x50900,first,43);
    f=call(frame(0x4f00)); CHECK(!(f.flags&1));
    memcpy(guest+0x50900,child,43); f=call(frame(0x4f00)); CHECK((f.flags&1) && f.ax==18);
    /* New mount lifetime retires authority and every old copied token. */
    space.generation[2]++; memcpy(guest+0x50900,first,43);
    f=call(frame(0x4f00)); CHECK((f.flags&1) && f.ax==18 && !cursor_count());
    bridge.view[2].generation=bridge.view[3].generation=space.generation[2];
    set_dta(0x800);
    for (unsigned i=0;i<DOS_FILE_SEARCHES;i++) {
        char pattern[FS_PATH_BYTES]; snprintf(pattern,sizeof(pattern),"C:/%sCOPY?.DAT",i ? "*" : "");
        /* Distinct equivalent wildcard spellings exercise real authority admission. */
        char *leaf=pattern+3; memmove(leaf+i,leaf,strlen(leaf)+1); memset(leaf,'*',i);
        f=path_call(0x4e00,pattern); CHECK(!(f.flags&1));
    }
    CHECK(cursor_count()==DOS_FILE_SEARCHES);
    f=path_call(0x4e00,"C:/*.*"); CHECK((f.flags&1) && f.ax==4);
    dos_file_cancel(&bridge); CHECK(!cursor_count()); teardown();
    puts("H49-05 continuation: PASS EOF copies, query reuse, child exit, mount generation, exhaustion and VM reclamation");
}

static void search_namespace_change_tests(void) {
    setup();
    for (unsigned i=0;i<3;i++) { char name[32]; snprintf(name,sizeof(name),"C:/SHIFT%u.DAT",i); close_file(create_file(name)); }
    set_dta(0x800); struct dos_file_frame f=path_call(0x4e00,"C:/SHIFT?.DAT"); CHECK(!(f.flags&1));
    f=path_call(0x4100,"C:/SHIFT0.DAT"); CHECK(!(f.flags&1));
    f=path_call(0x4e00,"C:/SHIFT?.DAT"); CHECK(!(f.flags&1) && !strcmp((char *)guest+0x5081e,"SHIFT1.DAT"));
    f=call(frame(0x4f00)); CHECK(!(f.flags&1) && !strcmp((char *)guest+0x5081e,"SHIFT2.DAT"));
    CHECK(cursor_count()==1); teardown();
    puts("H49-05 continuation: PASS query reuse after namespace mutation advances token authority");
}

static fs_lock *release_lock;
static uint64_t release_at;
static unsigned independent_work;
static void wait_pause(void *arg) {
    const struct vfs_request *r=fs_request_current();
    uint64_t before=wall_time; pause_time(arg); independent_work++;
    if (r && before<=r->deadline && wall_time>r->deadline) wall_time=r->deadline;
    if (release_lock && wall_time>=release_at) { fs_lock_drop(release_lock); release_lock=0; }
}
static void renew_request(struct vfs_request *r,struct vfs_wait_info *info) {
    memset(info,0,sizeof(*info)); cancel_request=false;
    *r=(struct vfs_request){0,wall_time+DOS_FILE_BUDGET_US,wall,request_cancel,wait_pause,0x49,info,0};
    info->drives=1u<<2;
}
static void held_waits(void) {
    setup(); struct vfs_request r; struct vfs_wait_info info; struct fs_request_scope scope;
    renew_request(&r,&info); fs_request_enter(&scope,&r);
    fs_lock_take(&cache.lock); uint8_t b[512]; memset(b,0xa5,sizeof(b)); unsigned before=reads;
    CHECK(cache_read(&cache,&device,100,b)==-VFS_ETIMEOUT && wall_time==r.deadline);
    CHECK(info.reason==VFS_WAIT_CACHE && info.outcome==-VFS_ETIMEOUT && info.elapsed_us==DOS_FILE_BUDGET_US && reads==before && b[0]==0xa5);
    fs_lock_drop(&cache.lock); fs_request_leave(&scope);
    renew_request(&r,&info); fs_request_enter(&scope,&r);
    OK(vfs_admit(&space,1u<<2,&r)); struct vfs_io_state state=vfs_io_begin_locked(&space);
    fs_lock_take(&space.lock); release_lock=&space.lock; release_at=r.deadline;
    vfs_io_end_locked(&space,state);
    CHECK(wall_time==r.deadline && info.reason==VFS_WAIT_NAMESPACE && info.outcome==-VFS_ETIMEOUT);
    vfs_unlock(&space); fs_request_leave(&scope);
    CHECK(!space.active && !space.mutating && !space.waiters);
    OK(vfs_admit(&space,1u<<2,0)); vfs_unlock(&space);
    /* Cancel a joined fill without freeing the owner's stack reservation. */
    struct cache_fill fill={.dev=&device,.lba=100,.count=1,.buffer=b};
    cache.fills=&fill; renew_request(&r,&info); fs_request_enter(&scope,&r);
    CHECK(cache_read(&cache,&device,100,b)==-VFS_ETIMEOUT && !fill.waiters && cache.fills==&fill);
    CHECK(info.reason==VFS_WAIT_CACHE && wall_time==r.deadline);
    fs_request_leave(&scope); cache.fills=0;
    /* Pending f1-82 writeback owns the shared batch; cancellation cannot
     * clean somebody else's dirty blocks or make a caller error sticky. */
    OK(cache_write(&cache,&device,100,b)); uint32_t dirty=cache.dirty_bytes; CHECK(dirty==512);
    cache.batch_busy=true; renew_request(&r,&info); fs_request_enter(&scope,&r);
    CHECK(cache_barrier(&cache,&device)==-VFS_ETIMEOUT && wall_time==r.deadline);
    CHECK(info.reason==VFS_WAIT_CACHE && cache.batch_busy && cache.dirty_bytes==dirty && !cache_error(&cache,&device));
    fs_request_leave(&scope); cache.batch_busy=false; OK(cache_barrier(&cache,&device)); CHECK(!cache.dirty_bytes);
    CHECK(independent_work>=4000); teardown();
    puts("H49-08 continuation: PASS cache and namespace reacquisition deadlines, retirement and unrelated work");
}
static void controller_waits(void) {
    struct blk_queue q; blk_queue_init(&q,"dos-test",65536); device.queue=&q;
    struct vfs_request r; struct vfs_wait_info info; struct fs_request_scope scope; uint8_t b[512];
    renew_request(&r,&info); fs_request_enter(&scope,&r);
    /* An unrelated issued command cannot hold a queued cancelled waiter. */
    q.busy=true; unsigned before=reads;
    CHECK(blkdev_read(&device,100,1,b)==-VFS_ETIMEOUT && wall_time==r.deadline);
    CHECK(info.reason==VFS_WAIT_TRANSPORT && info.outcome==-VFS_ETIMEOUT && reads==before);
    CHECK(!q.head && !q.requests); q.busy=false; fs_request_leave(&scope);
    renew_request(&r,&info); fs_request_enter(&scope,&r); OK(blkdev_read(&device,100,1,b)); fs_request_leave(&scope);
    struct blk_queue_stats st; blk_queue_snapshot(&q,&st); CHECK(st.cancelled==1 && st.issued==1);
    renew_request(&r,&info); fs_request_enter(&scope,&r); transport_hold=true;
    CHECK(blkdev_read(&device,101,1,b)==-VFS_ETIMEOUT && wall_time==r.deadline);
    CHECK(info.reason==VFS_WAIT_TRANSPORT && info.outcome==-VFS_ETIMEOUT);
    transport_hold=false; fs_request_leave(&scope);
    CHECK(!q.head && !q.requests && !q.active); OK(blk_queue_destroy(&q)); device.queue=0;
    puts("H49-08 continuation: PASS f1-83 queued deadline, unlink during busy controller, successor and pool retirement");
}

/* Read-only remount after each failure: production parser enumerates owning
 * entries, then the test checks uniqueness/chain length against disk FAT16.
 * Lost clusters/dirty flags are permitted by the existing F1 failure policy. */
static void check_directory_ownership(struct fat_volume *scan,uint32_t dir,uint8_t *owners,unsigned depth) {
    CHECK(depth<8); uint32_t cursor=0; struct fat_entry ent; int e;
    while (!(e=fat_next(scan,dir,&cursor,&ent))) {
        if ((ent.attr&FAT_ATTR_VOLUME) || path_equal(ent.alias,".") || path_equal(ent.alias,"..")) continue;
        uint32_t c=ent.first, count=0;
        while (c>=2 && c<scan->clusters+2) {
            CHECK(!owners[c] && ++count<=scan->clusters); owners[c]=1;
            uint32_t next=fs_rd16(disk+512+c*2);
            if (next>=0xfff8) { c=0; break; }
            CHECK(next>=2 && next<scan->clusters+2); c=next;
        }
        CHECK(!c && (uint64_t)count*512>=ent.size);
        if (ent.attr&FAT_ATTR_DIR) check_directory_ownership(scan,ent.first,owners,depth+1);
    }
    CHECK(e==-FS_ENOENT);
}
static void check_disk_ownership(void) {
    static struct block_cache scan_cache; static struct fat_volume scan;
    bool logical=device.logical; device.logical=true;
    OK(cache_init(&scan_cache,65536)); OK(fat_mount(&scan,&scan_cache,&device,0,8192,false));
    uint8_t owners[8192]={0}; check_directory_ownership(&scan,0,owners,0);
    OK(fat_unmount(&scan)); cache_destroy(&scan_cache); device.logical=logical;
}
static unsigned failed_unmounts;
static void fault_teardown(void) {
    dos_file_cancel(&bridge); CHECK(!pins);
    vfs_table_destroy(&table); vfs_table_destroy(&native_table);
    int e=vfs_detach(&space,2);
    if (e) {
        CHECK(e==-FS_EIO || e==-FS_EROFS || e==-FS_EUCLEAN); failed_unmounts++;
        /* Fixture media removal after recording refused durable unmount. */
        space.volumes[2]=0; volume.mounted=false;
    }
    for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) CHECK(!space.descriptions[i].refs);
    for (unsigned i=0;i<VFS_NODES;i++) CHECK(!space.nodes[i].refs);
    vfs_destroy(&space); cache_destroy(&cache); CHECK(!fs_host_contig_live);
}
enum mutation { MUT_CREATE,MUT_WRITE,MUT_TRUNCATE,MUT_RENAME,MUT_DELETE,MUT_CLOSE,MUT_AUTOCOMMIT,MUT_RENAME_CROSS,MUT_EXTEND,MUTATIONS };
static unsigned mutation_prepare(enum mutation op) {
    setup(); bridge.ops.critical=0;
    if (op==MUT_CREATE) return 0;
    if (op==MUT_RENAME_CROSS) OK(vfs_mkdir(&native_table,"C:/DEST",0));
    unsigned h=create_file("C:/FAULT.DAT"); memset(guest+0x50400,0x69,8192);
    struct dos_file_frame f=transfer_file(0x4000,h,4096); CHECK(!(f.flags&1) && f.ax==4096);
    if (op==MUT_RENAME || op==MUT_RENAME_CROSS || op==MUT_DELETE || op==MUT_AUTOCOMMIT) {
        close_file(h);
        if (op==MUT_AUTOCOMMIT) {
            path_set("C:/FAULT.DAT"); f=frame(0x6c00); f.si=0x100; f.bx=0x6042; f.cx=0; f.dx=1; f=call(f); CHECK(!(f.flags&1)); h=f.ax;
        }
    }
    if (op==MUT_WRITE) { f=seek_file(h,0,0); CHECK(!(f.flags&1)); }
    if (op==MUT_EXTEND) { f=seek_file(h,0,12288); CHECK(!(f.flags&1)); }
    if (op==MUT_TRUNCATE) { f=seek_file(h,0,1024); CHECK(!(f.flags&1)); }
    memset(guest+0x50400,0x72,8192);
    if (op==MUT_RENAME || op==MUT_RENAME_CROSS) strcpy((char *)guest+0x50600,op==MUT_RENAME ? "C:/OTHER.DAT" : "C:/DEST/OTHER.DAT");
    return h;
}
static enum dos_file_result mutation_execute(enum mutation op,unsigned h,struct dos_file_frame *f) {
    *f=frame(op==MUT_CREATE ? 0x3c00 : (op==MUT_RENAME || op==MUT_RENAME_CROSS) ? 0x5600 : op==MUT_DELETE ? 0x4100 : op==MUT_CLOSE ? 0x3e00 : 0x4000);
    f->bx=(uint16_t)h; f->cx=op==MUT_WRITE || op==MUT_AUTOCOMMIT ? 8192 : 0; f->dx=0x400;
    if (op==MUT_CREATE || op==MUT_RENAME || op==MUT_RENAME_CROSS || op==MUT_DELETE) { path_set(op==MUT_CREATE ? "C:/NEW.DAT" : "C:/FAULT.DAT"); f->dx=0x100; }
    if (op==MUT_RENAME || op==MUT_RENAME_CROSS) f->di=0x600;
    return dos_file_dispatch(&bridge,f,DOS_FILE_V86,0x21);
}
static void mutation_fault_matrix(void) {
    static const char *names[]={"create","write","truncate","rename","delete","close","autocommit","rename_cross","extend"};
    unsigned total=0, stops=0, allocs=0, short_writes=0, error_returns=0;
    for (unsigned cached=0;cached<3;cached++) for (enum mutation op=0;op<MUTATIONS;op++) {
        device.logical=!cached; device.write_cache_state=cached==2 ? BLKDEV_CACHE_ENABLED : BLKDEV_CACHE_DISABLED; unsigned h=mutation_prepare(op); struct dos_file_frame f;
        io_calls=0; io_tracking=true; allocation_calls=0;
        CHECK(mutation_execute(op,h,&f)==DOS_FILE_DONE && !(f.flags&1));
        unsigned calls=io_calls, allocations=allocation_calls; io_tracking=false;
        teardown();
        for (unsigned nth=1;nth<=calls;nth++) {
            h=mutation_prepare(op); io_calls=0; io_fault=nth; io_tracking=true;
            enum dos_file_result result=mutation_execute(op,h,&f);
            io_tracking=false; io_fault=0; CHECK(result==DOS_FILE_DONE && !pins && !fs_request_current());
            CHECK(!space.active && !space.reserved && !space.mutating && !space.waiters);
            if (f.flags&1) {
                CHECK(f.ax==29 || f.ax==30 || f.ax==5 || f.ax==19 || f.ax==31); error_returns++;
                if (op==MUT_WRITE || op==MUT_TRUNCATE || op==MUT_EXTEND || op==MUT_AUTOCOMMIT)
                    CHECK(bridge.cleanup_status || bridge.process[0].error.code==f.ax);
            } else if (op==MUT_WRITE || op==MUT_AUTOCOMMIT) {
                CHECK(f.ax==4096 || f.ax==8192);
                unsigned index=bridge.process[0].handles[h];
                struct dos_file_description *s=&bridge.sft[index];
                CHECK(s->table->handles[s->handle]->position==f.ax);
                uint8_t contents[8192]; size_t read=0;
                uint64_t position=0; OK(vfs_seek(s->table,s->handle,0,VFS_SEEK_SET,&position));
                OK(vfs_read(s->table,s->handle,contents,f.ax,&read)); CHECK(read==f.ax);
                for (unsigned j=0;j<read;j++) CHECK(contents[j]==0x72);
                if (f.ax==4096) { CHECK(bridge.process[0].error.code==29 || bridge.process[0].error.code==30); short_writes++; }
            }
            /* No failed call publishes a duplicate VFS description. */
            unsigned descriptions=0; for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) descriptions+=space.descriptions[i].refs!=0;
            CHECK(descriptions<=1);
            check_disk_ownership(); fault_teardown(); total++;
        }
        for (unsigned nth=1;nth<=allocations;nth++) {
            h=mutation_prepare(op); allocation_calls=0; allocation_fail=nth;
            CHECK(mutation_execute(op,h,&f)==DOS_FILE_DONE); allocation_fail=0;
            CHECK(!pins && !space.active && !space.reserved && !space.mutating && !space.waiters);
            check_disk_ownership(); fault_teardown(); allocs++;
        }
        for (unsigned nth=1;nth<=calls;nth++) {
            h=mutation_prepare(op); io_calls=0; io_stop=nth; io_tracking=true;
            uint8_t buffer[8192]; memcpy(buffer,guest+0x50400,sizeof(buffer));
            enum dos_file_result result=mutation_execute(op,h,&f);
            io_tracking=false; io_stop=0;
            CHECK(result==DOS_FILE_PENDING && cancel_request && !pins);
            CHECK(!memcmp(buffer,guest+0x50400,sizeof(buffer)) && !fs_request_current());
            CHECK(!space.active && !space.reserved && !space.mutating && !space.waiters);
            cancel_request=false; check_disk_ownership(); fault_teardown(); stops++;
        }
        printf("H49-06 continuation mutation=%s cached=%u io_points=%u allocations=%u PASS\n",names[op],cached,calls,allocations);
    }
    device.logical=true; device.write_cache_state=BLKDEV_CACHE_DISABLED; CHECK(total && stops && allocs);
    CHECK(short_writes && error_returns);
    printf("H49-06 continuation: PASS io_faults=%u stop_points=%u allocation_points=%u partial_writes=%u errors=%u refused_unmounts=%u ownership=unique\n",total,stops,allocs,short_writes,error_returns,failed_unmounts);
}
static void private_failure_teardown(void) {
    format(); device.logical=true; static struct dos_file_volume private;
    OK(cache_init(&cache,65536)); OK(dos_file_volume_open(&private,&cache,&device,true));
    struct cache_stats before,after; cache_stats(&cache,&before);
    struct vfs_table other; OK(vfs_table_init(&private.space,&other,2));
    int h=vfs_open(&private.table,"C:/PRIVATE.DAT",VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0); CHECK(h>=0);
    int second=vfs_open(&other,"C:/PRIVATE.DAT",VFS_READ,VFS_DENY_NONE,0); CHECK(second>=0);
    CHECK(dos_file_volume_discard(&private)==-FS_EBUSY);
    struct vfs_request r; struct vfs_wait_info info; struct fs_request_scope scope; int cleanup=0;
    renew_request(&r,&info); r.cleanup_status=&cleanup; fs_request_enter(&scope,&r);
    uint8_t bytes[512]={0x42}; size_t done=99; fail_write=1;
    CHECK(vfs_write(&private.table,h,bytes,sizeof(bytes),&done)==-FS_EIO && !done);
    fs_request_leave(&scope); CHECK(cleanup==-FS_EIO && private.fat.readonly);
    CHECK(dos_file_volume_close(&private)==-FS_EBUSY && private.mounted && !private.table_ready);
    CHECK(dos_file_volume_discard(&private)==-FS_EBUSY); /* another native claim survives */
    OK(vfs_close(&other,second)); vfs_table_destroy(&other);
    CHECK(dos_file_volume_close(&private)==-FS_EROFS && cleanup==-FS_EIO);
    legacy_volume_claim=true; CHECK(dos_file_volume_discard(&private)==-FS_EBUSY); legacy_volume_claim=false;
    unsigned transfers=reads+writes+barriers;
    OK(dos_file_volume_discard(&private)); CHECK(!private.mounted && !private.fat.mounted && cleanup==-FS_EIO);
    CHECK(reads+writes+barriers==transfers); OK(dos_file_volume_close(&private));
    cache_stats(&cache,&after); CHECK(before.data_pages==after.data_pages && before.meta_pages==after.meta_pages && !after.dirty_bytes);
    cache_destroy(&cache); CHECK(!fs_host_contig_live && allocations_live==resident_allocations);
    puts("H49-08 private teardown: PASS failed mutation status, no claim bypass, zero-I/O discard, idempotence and ledger baseline");
}
static void continuation_tests(void) {
    copied_search_tests(); search_namespace_change_tests(); held_waits(); controller_waits(); mutation_fault_matrix(); private_failure_teardown();
    CHECK(allocations_live==resident_allocations && !fs_host_contig_live && !fs_request_current());
}
