/* F1-84: production identities and cross-personality lock matrix. No mmap.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include "fake.h"
#include "scan.h"
#include <stdio.h>
#include <assert.h>
static struct block_cache cache;
static struct fat_volume vol;
static struct vfs space;
static struct vfs_table a,b,child;
static unsigned cases;
static const unsigned deny[]={0,VFS_DENY_ALL,VFS_DENY_WRITE,VFS_DENY_READ,VFS_DENY_NONE};
static int open_mode(struct vfs_table *t,unsigned access,unsigned mode) {
    return vfs_open_claim(t,"C:/Lock identity long.bin",access,(enum vfs_share)deny[mode],0,mode,0);
}
static void explicit_shares(void) {
    for (unsigned x=0;x<VFS_OWNER_TYPES;x++) for (unsigned y=0;y<VFS_OWNER_TYPES;y++) {
        assert(!vfs_table_set_owner(&a,x,42) && !vfs_table_set_owner(&b,y,42));
        if (x!=VFS_OWNER_DOS) {
            char cwd[FS_PATH_BYTES];
            assert(!vfs_chdir(&a,"C:/Fixture dir") && !vfs_getcwd(&a,2,cwd));
            assert(!strcmp(cwd,"/Fixture dir") && !vfs_chdir(&a,"C:/"));
        }
        for (unsigned m=1;m<5;m++) for (unsigned n=1;n<5;n++)
            for (unsigned r=1;r<=3;r++) for (unsigned s=1;s<=3;s++) {
                int h=open_mode(&a,r,m); assert(h>=0);
                int k=open_mode(&b,s,n);
                bool conflict=(deny[m]&s) || (deny[n]&r);
                assert(conflict ? k==-VFS_ESHARE : k>=0);
                if (k>=0) assert(!vfs_close(&b,k));
                assert(!vfs_close(&a,h)); cases++;
            }
        int h=open_mode(&a,3,0); assert(h>=0);
        int k=open_mode(&b,3,0);
        assert(x==y ? k>=0 : k==-VFS_ECRITICAL_SHARE);
        if (k>=0) assert(!vfs_close(&b,k));
        assert(!vfs_close(&a,h)); cases++;
        for (unsigned n=1;n<5;n++) {
            h=open_mode(&a,3,0); assert(h>=0);
            assert(open_mode(&b,1,n)==-VFS_ESHARE); assert(!vfs_close(&a,h)); cases++;
            h=open_mode(&a,1,n); assert(h>=0);
            assert(open_mode(&b,3,0)==-VFS_ECRITICAL_SHARE); assert(!vfs_close(&a,h)); cases++;
        }
    }
    printf("PASS F1-84 share matrix owner_pairs=9 cases=%u bidirectional explicit/compatibility/equal-numeric-owner\n",cases);
}
static unsigned locks(void) { unsigned n=0; for (unsigned i=0;i<VFS_RANGE_LOCKS;i++) n+=space.range_locks[i].description!=0; return n; }
static void range_locks(void) {
    for (unsigned x=0;x<VFS_OWNER_TYPES;x++) for (unsigned y=0;y<VFS_OWNER_TYPES;y++) {
        assert(!vfs_table_set_owner(&a,x,42) && !vfs_table_set_owner(&b,y,42));
        assert(!vfs_table_set_owner(&child,y,43));
        int h=open_mode(&a,3,4),k=open_mode(&b,3,4); assert(h>=0 && k>=0);
        assert(!vfs_lock_range(&a,h,10,20,false));
        assert(vfs_lock_range(&b,k,15,1,false)==-VFS_ELOCK);
        assert(vfs_lock_range(&b,k,10,20,true)==-VFS_ELOCK);
        assert(!vfs_lock_range(&b,k,30,5,false)); /* adjacent */
        uint64_t pos; size_t done; char bytes[5]={0};
        assert(!vfs_seek(&b,k,10,VFS_SEEK_SET,&pos));
        assert(vfs_read(&b,k,bytes,1,&done)==-VFS_ELOCK && !done);
        assert(vfs_write(&b,k,bytes,1,&done)==-VFS_ELOCK && !done);
        assert(vfs_truncate(&b,k,0)==-VFS_ELOCK);
        assert(vfs_lock_range(&b,k,UINT32_MAX,1,false)==-FS_EINVAL);
        int inherited=vfs_dup(&a,h,&child,-1); assert(inherited>=0);
        assert(vfs_lock_range(&child,inherited,10,20,true)==-VFS_ELOCK);
        assert(!vfs_lock_range(&child,inherited,50,5,false));
        assert(locks()==3);
        vfs_release_lock_owner(&space,(struct vfs_lock_owner){x,42});
        assert(locks()==(x==y ? 1u : 2u)); /* type isolates equal numeric ids */
        assert(!vfs_lock_range(&a,h,10,20,false));
        assert(!vfs_close(&child,inherited)); assert(locks()==(x==y ? 1u : 2u));
        assert(!vfs_close(&a,h)); assert(locks()==(x==y ? 0u : 1u));
        assert(!vfs_close(&b,k)); assert(!locks());
    }
    puts("PASS F1-84 range matrix owner_pairs=9 overlap/adjacent/I-O/truncate/unlock/inheritance/close/death isolation");
    /* Atomic dup replacement has exactly the same lock release as close. */
    assert(!vfs_table_set_owner(&a,VFS_OWNER_NATIVE,7));
    int h=open_mode(&a,3,4),k=open_mode(&a,3,4); assert(h>=0 && k>=0);
    assert(!vfs_lock_range(&a,k,5,1,false) && locks()==1);
    assert(vfs_dup(&a,k,&a,k)==k && locks()==1); /* same descriptor is not a close */
    assert(vfs_dup(&a,h,&a,k)==k && !locks());
    assert(!vfs_close(&a,k) && !vfs_close(&a,h));
}
static void identities(void) {
    int h=open_mode(&a,3,4); assert(h>=0);
    struct fat_entry entry; assert(!vfs_handle_stat(&a,h,&entry,0));
    char path[80]; snprintf(path,sizeof(path),"C:/%s",entry.alias);
    int k=vfs_open(&b,path,3,0,0); assert(k>=0);
    assert(a.handles[h]->node==b.handles[k]->node);
    struct vfs_file_identity *id=a.handles[h]->node->identity;
    assert(id && id==b.handles[k]->node->identity);
    struct vfs_page_identity *p,*q,*new_page;
    assert(!vfs_page_get(&space,id,0,&p) && !vfs_page_get(&space,id,0,&q) && p==q);
    assert(p->key.file==id->serial && p->key.generation==id->generation && !p->key.index);
    char data[3]={'f','8','4'}; size_t n;
    assert(!vfs_write(&a,h,data,3,&n) && n==3 && !p->valid);
    assert(!vfs_page_get(&space,id,0,&new_page) && new_page!=p && new_page->valid);
    assert(!vfs_truncate(&a,h,2) && !new_page->valid);
    struct cache_completion record; assert(!vfs_completion(&a,h,&record));
    assert(record.accepted==record.durable && record.durable_completions && !record.incomplete);
    assert(!vfs_close(&b,k) && !vfs_close(&a,h));
    assert(!id->refs && id->pages==2 && vfs_identity_busy(&space,&vol));
    struct vfs_page_identity *later;
    assert(!vfs_page_get(&space,id,1,&later) && later->key.file==id->serial);
    vfs_page_put(&space,later); /* a surviving mapping can acquire another page after close */
    assert(vfs_detach(&space,2)==-FS_EBUSY); /* page lifetime outlives close */
    struct fat_entry file; assert(!fat_lookup(&vol,vol.root,"Lock identity long.bin",&file));
    struct vfs_file_identity *same; assert(!vfs_identity_get(&space,&vol,&file,&same) && same==id);
    uint64_t serial=id->serial;
    assert(!fat_rename(&vol,&file,vol.root,"Renamed identity.bin") && id->serial==serial && id->linked);
    struct fat_entry alias; assert(!fat_lookup(&vol,vol.root,file.alias,&alias));
    struct vfs_file_identity *renamed; assert(!vfs_identity_get(&space,&vol,&alias,&renamed) && renamed==id);
    assert(!fat_remove(&vol,&file,false) && !id->linked);
    assert(!fat_create(&vol,vol.root,"Replacement.bin",0,&file));
    struct vfs_file_identity *replacement; assert(!vfs_identity_get(&space,&vol,&file,&replacement));
    assert(replacement->serial!=serial); /* recycled directory slot cannot alias */
    vfs_identity_put(&space,renamed); vfs_identity_put(&space,same);
    vfs_page_put(&space,p); vfs_page_put(&space,q); vfs_page_put(&space,new_page);
    assert(!id->pages);
    assert(!vfs_page_get(&space,replacement,UINT32_MAX,&p));
    h=vfs_open(&a,"C:/Replacement.bin",3,0,0); assert(h>=0);
    vfs_raw_changed(&space,&vol); assert(!p->valid && !replacement->valid);
    uint64_t position;
    assert(vfs_seek(&a,h,0,VFS_SEEK_SET,&position)==-FS_EIO);
    assert(vfs_handle_times(&a,h,0,0)==-FS_EIO && vfs_commit(&a,h)==-FS_EIO);
    assert(vfs_read(&a,h,data,1,&n)==-FS_EIO && !n);
    assert(vfs_write(&a,h,data,1,&n)==-FS_EIO && !n);
    assert(!vfs_close(&a,h));
    assert(vfs_page_get(&space,replacement,0,&q)==-FS_EIO);
    vfs_page_put(&space,p); vfs_identity_put(&space,replacement);
    puts("PASS F1-84 page identity LFN/SFN aliases/write/truncate/close/rename/unlink/slot-reuse/raw invalidation");
}
int main(int argc,char **argv) {
    assert(argc==2); struct fake f; assert(!fake_open(&f,argv[1])); f.undo_enabled=true;
    assert(!cache_init(&cache,2*1024*1024) && !fat_mount(&vol,&cache,&f.dev,0,f.dev.capacity,true) && !vol.readonly);
    vfs_init(&space); assert(!vfs_attach(&space,2,&vol));
    assert(!vfs_table_init(&space,&a,2) && !vfs_table_init(&space,&b,2) && !vfs_table_init(&space,&child,2));
    assert(a.owner!=b.owner && a.owner!=child.owner && b.owner!=child.owner);
    int h=vfs_open(&a,"C:/Lock identity long.bin",3|VFS_CREATE,0,0); assert(h>=0);
    uint8_t data[100]={0}; size_t n; assert(!vfs_write(&a,h,data,sizeof(data),&n) && n==sizeof(data));
    assert(!vfs_lock_range(&a,h,2,3,false));
    int inherited=vfs_dup(&a,h,&b,-1); assert(inherited>=0);
    assert(!vfs_close(&b,inherited) && locks()==1); /* distinct default native contexts */
    assert(!vfs_close(&a,h) && !locks()); explicit_shares(); range_locks(); identities();
    vfs_table_destroy(&a); vfs_table_destroy(&b); vfs_table_destroy(&child);
    assert(!vfs_detach(&space,2)); vfs_destroy(&space); cache_destroy(&cache);
    struct scan_result scan=independent_scan(f.fd,0); assert(!scan.lost && !scan.divergent && !scan.corrupt && !scan.crosslinks);
    fake_reset(&f); fake_close(&f);
    printf("RESULT F1-84 identity/lock checks FAT%u failures=0\n",vol.type);
}
