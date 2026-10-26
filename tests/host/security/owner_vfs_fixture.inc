/* Included by the production channel fixture. Only disk/account-evidence
 * boundaries are fake; all OWNER storage operations use real FAT/VFS. */
#include "../fs/fake.h"
#include "../../../src/kernel/fs/owner_vfs.h"
static void real_mkdir(struct vfs_table *t,const char *name)
{ int e=vfs_mkdir(t,name,0); CHECK(!e || e==-EEXIST); }
static void real_file(struct vfs_table *t,const char *name,const void *data,size_t bytes)
{
    int fd=vfs_open(t,name,VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_ALL,0); CHECK(fd>=0);
    size_t n=0; CHECK(!vfs_write(t,fd,data,bytes,&n) && n==bytes); CHECK(!vfs_commit(t,fd)); CHECK(!vfs_close(t,fd));
}
static void test_owner_real_vfs_case(const char *image,unsigned count,bool imported)
{
    struct fake *disk=calloc(1,sizeof(*disk)); struct block_cache *cache=calloc(1,sizeof(*cache));
    struct vfs *v=calloc(1,sizeof(*v)); struct vfs_table *table=calloc(1,sizeof(*table)); struct fat_volume *volume=calloc(1,sizeof(*volume));
    struct owner_store *s=calloc(1,sizeof(*s)); struct owner_vfs *backend=calloc(1,sizeof(*backend)); CHECK(disk && cache && v && table && volume && s && backend);
    CHECK(!fake_open(disk,image)); disk->undo_enabled=true; CHECK(!cache_init(cache,0)); vfs_init(v);
    CHECK(!fat_mount(volume,cache,&disk->dev,0,disk->dev.capacity,true)); CHECK(!vfs_attach(v,2,volume)); CHECK(!vfs_table_init(v,table,2));
    const char *dirs[]={"C:/System","C:/System/Security","C:/System/Security/Owner","C:/System/Security/Staging",
        "C:/System/Security/Staging/g0000000000000001","C:/Windows","C:/Windows/Profiles"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++) real_mkdir(table,dirs[i]);
    struct owner_fake evidence={0}; memset(evidence.account_digest,7,32); make_batch(&evidence,count,1,imported);
    for(unsigned i=0;i<count;i++) {
        char stage[128],file[160]; uint32_t uid=evidence.request.bindings[i].uid;
        ksnprintf(stage,sizeof(stage),"C:/System/Security/Staging/g0000000000000001/p%08x",uid); real_mkdir(table,stage);
        ksnprintf(file,sizeof(file),"%s/data.txt",stage); real_file(table,file,"owner fixture",13);
        if(imported && i>=2) {
            ksnprintf(file,sizeof(file),"C:%s",evidence.request.bindings[i].path); real_mkdir(table,file);
            ksnprintf(file,sizeof(file),"C:%s/data.txt",evidence.request.bindings[i].path); real_file(table,file,"owner fixture",13);
        }
        uint8_t digest[32]; CHECK(!owner_vfs_tree_digest(table,stage,digest)); memcpy(evidence.manifest.data+64+256*i+208,digest,32);
    }
    sha256(evidence.manifest.data,evidence.manifest.bytes,evidence.request.manifest_digest);
    real_file(table,"C:/System/Security/Staging/g0000000000000001/manifest",evidence.manifest.data,evidence.manifest.bytes);
    struct owner_account_evidence account={&evidence,account_intent,account_now,checkpoint_marker,import_map}; struct owner_backend io;
    CHECK(!owner_vfs_backend(backend,table,31,&account,&io)); CHECK(!owner_open(s,&io,owner_sid,31,true));
    CHECK(owner_root_occupied(s,31,evidence.request.bindings[2].path)==(imported?1:0));
    if(imported) { evidence.bad_map=1; uint8_t denied[OWNER_REPLY_MAX]; CHECK(call(s,&evidence.request,OWNER_RESERVE,denied)==-EACCES && !s->key_high); evidence.bad_map=0; }
    uint8_t reply[OWNER_REPLY_MAX]; CHECK(!call(s,&evidence.request,OWNER_RESERVE,reply)); CHECK(backend->sealed_g==1);
    CHECK(owner_vfs_reserved(backend,"/Windows/Profiles")); CHECK(owner_vfs_reserved(backend,"/System/Security/Staging/g0000000000000001/manifest"));
    CHECK(!owner_vfs_reserved(backend,"/System/Security/accounts.db.new")); CHECK(!owner_vfs_reserved(backend,"/Systematic"));
    CHECK(!call(s,&evidence.request,OWNER_PREPARE,reply));
    evidence.intent=1; CHECK(!call(s,&evidence.request,OWNER_COMMIT,reply)); CHECK(!backend->sealed_g && s->profile_count==count);
    for(unsigned i=0;i<count;i++) { char path[128]; ksnprintf(path,sizeof(path),"C:%s/data.txt",evidence.request.bindings[i].path); struct fat_entry entry; CHECK(!vfs_stat(table,path,&entry) && entry.size==13); }
    CHECK(!owner_security_bind(s)); struct owner_record record; CHECK(!owner_profile(s,1000,&record));
    struct ciuki_security_profile descriptor={.version=1,.size=112,.uid=1000,.volume_id_lo=31,
        .owner_key_lo=(uint32_t)record.binding.key,.owner_generation_lo=(uint32_t)record.binding.generation};
    memcpy(descriptor.path,record.binding.path,64); descriptor.path_bytes=(uint32_t)strlen(descriptor.path)+1;
    CHECK(!security_owner_verify(&descriptor)); descriptor.owner_key_lo++; CHECK(security_owner_verify(&descriptor)==-EIO);
    owner_close(s); CHECK(!owner_vfs_backend(backend,table,31,&account,&io)); CHECK(!owner_open(s,&io,owner_sid,31,false));
    CHECK(s->profile_count==count && s->generation==1); CHECK(!call(s,&evidence.request,OWNER_STATUS,reply) && fs_rd32(reply+8)==OWNER_COMMITTED);
    owner_close(s);
    /* A coherent store cannot be rebound to a replacement profile directory. */
    char original[128]; ksnprintf(original,sizeof(original),"C:%s",evidence.request.bindings[2].path);
    CHECK(!vfs_rename(table,original,"C:/Windows/Profiles/old-root")); real_mkdir(table,original);
    CHECK(owner_open(s,&io,owner_sid,31,false)==-EIO); owner_close(s);
    fake_destroy(&evidence); vfs_table_destroy(table); CHECK(!vfs_detach(v,2)); vfs_destroy(v); cache_destroy(cache); fake_reset(disk); fake_close(disk);
    free(s); free(backend); free(volume); free(table); free(v); free(cache); free(disk);
    printf("H02 real FAT OWNER: %u%s bindings, destination manifest/tree, snapshot/barriers, mount reload, PROFILE and replacement rejection PASS\n",count,imported?" (16 imports + builtins)":"");
}
static void test_owner_real_vfs(const char *image)
{ test_owner_real_vfs_case(image,3,false); test_owner_real_vfs_case(image,18,true); }
