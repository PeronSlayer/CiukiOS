/* Same production FAT/VFS/native fixtures as RK; independent H02 expectations.
 * SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "../proc/files_test.c"
#include "../../../src/kernel/fs/owner.h"
#include "../../../src/kernel/fs/owner_vfs.h"
static struct security_subject name_alice={.credential={.version=1,.size=64,.uid=1007,.group_count=1,.groups={545},.flags=1},.live=true};
static struct security_subject name_bob={.credential={.version=1,.size=64,.uid=1001,.group_count=1,.groups={545},.flags=1},.live=true};
static struct security_subject name_guest={.credential={.version=1,.size=64,.uid=501,.group_count=1,.groups={546},.flags=1},.live=true};
static struct security_subject name_admin={.credential={.version=1,.size=64,.uid=500,.group_count=2,.groups={544,545},.flags=1},.live=true};
static struct security_subject name_system={.credential={.version=1,.size=64,.uid=0,.group_count=1,.groups={0},.flags=1},.live=true};
static int authorized(const char *path,struct px_node *cwd,const struct security_subject *subject,unsigned access,struct px_node **out)
{
    struct px_path result; vfs_lock(&vfs,0);
    int e=px_resolve_subject_locked(space,cwd,path,false,subject,&result);
    if(!e) e=px_policy_check_locked(result.node,subject,access);
    if(!e && out) *out=result.node;
    vfs_unlock(&vfs); return e;
}
static void file_create(struct vfs_table *t,const char *path)
{
    int fd=vfs_open(t,path,VFS_CREATE|VFS_WRITE|VFS_EXCLUSIVE,VFS_DENY_NONE,0); CHECK(fd>=0);
    size_t n; CHECK(!vfs_write(t,fd,"data",4,&n) && n==4); CHECK(!vfs_close(t,fd));
}
static void profile_record(struct owner_store *owners,unsigned index,uint32_t uid,const char *path)
{
    struct px_path result; CHECK(!resolve_path(path,&result));
    struct owner_record *r=&owners->profiles[index]; memset(r,0,sizeof(*r));
    r->binding=(struct owner_binding){.uid=uid,.action=1,.volume=31,.key=index+1,.locator=index+1,.generation=1};
    memcpy(r->binding.path,path,strlen(path)+1); CHECK(!px_object_proof_locked(result.node,r->proof));
}
static void test_hive_names(struct vfs_table *table)
{
    const struct security_subject *subjects[]={&name_system,&name_admin,&name_alice,&name_bob,&name_guest};
    const char *parents[]={"/Windows","/Windows/Profiles/u00000003","/Windows/Profiles/u00000003/Settings/Registry"};
    const char *names[]={"SYSTEM.CRG","SYSTEM.CRG.0","SYSTEM.CRG.1","USER.CRG","USER.CRG.0","USER.CRG.1"};
    for(unsigned scope=0;scope<3;scope++) for(unsigned file=0;file<6;file++) {
        char path[256],alias[256],input[256]; ksnprintf(path,sizeof(path),"C:%s/%s",parents[scope],names[file]);
        file_create(table,path); struct fat_entry entry; CHECK(!vfs_stat(table,path,&entry));
        ksnprintf(alias,sizeof(alias),"%s/%s",parents[scope],entry.alias);
        for(unsigned who=0;who<5;who++) for(unsigned bit=1;bit<=POLICY_META;bit<<=1) {
            CHECK(authorized(path+2,space->root,subjects[who],bit,0)==(who?-EACCES:0));
            CHECK(authorized(alias,space->root,subjects[who],bit,0)==(who?-EACCES:0));
        }
        ksnprintf(input,sizeof(input),"C:%s/%s. ",parents[scope],entry.alias);
        struct px_path resolved; vfs_lock(&vfs,0);
        CHECK(vfs_resolve_subject_locked(table,&name_admin,input,true,false,&resolved)==-EACCES);
        CHECK(!vfs_resolve_subject_locked(table,&name_system,input,true,false,&resolved));
        CHECK(!px_policy_check_locked(resolved.node,&name_system,POLICY_ALL)); vfs_unlock(&vfs);
    }
    file_create(table,"C:/Windows/USER.CRG.01");
    CHECK(!authorized("/Windows/USER.CRG.01",space->root,&name_admin,POLICY_WRITE,0));
    CHECK(!authorized("/Windows/USER.CRG.01",space->root,&name_bob,POLICY_READ,0));
    puts("H02 review 1 hives: six real/legacy names, global/profile/Registry scopes, five subjects, aliases and Win32 trimming PASS");
}
static void test_volume_paths(struct vfs_table *table,struct px_node *private_cwd)
{
    /* Real C: directories named mnt/dev are data in a drive namespace. */
    const char *dirs[]={"C:/mnt","C:/mnt/d","C:/dev","D:/dir"};
    for(unsigned i=0;i<4;i++) { int e=vfs_mkdir(table,dirs[i],0); CHECK(!e || e==-EEXIST); }
    file_create(table,"C:/c-file"); file_create(table,"D:/d-file");
    file_create(table,"C:/mnt/d/local-file"); file_create(table,"C:/dev/local-file");
    CHECK(!vfs_chdir(table,"C:/Windows/Profiles/u00000003"));
    /* The legacy VFS frontend does not mount duplicate drive aliases. Supply
     * its per-drive cwd boundary for the new resolved-volume adapter. */
    memcpy(table->cwd[4],table->cwd[2],sizeof(table->cwd[4])); table->cwd_cluster[4]=table->cwd_cluster[2];
    struct px_path r; vfs_lock(&vfs,0);
    CHECK(!vfs_resolve_subject_locked(table,&name_alice,"E:/Windows/Profiles/u00000003/private.txt",false,false,&r));
    CHECK(r.node->volume==&volumes[0]);
    CHECK(vfs_resolve_subject_locked(table,&name_bob,"E:/Windows/Profiles/u00000003/private.txt",false,false,&r)==-EACCES);
    CHECK(!vfs_resolve_subject_locked(table,&name_alice,"E:private.txt",false,false,&r));
    CHECK(vfs_resolve_subject_locked(table,&name_bob,"E:private.txt",false,false,&r)==-EACCES);
    for(unsigned win32=0;win32<2;win32++) {
        CHECK(!vfs_resolve_subject_locked(table,&name_bob,"C:/c-file",win32,false,&r)); CHECK(r.node->volume==&volumes[0]);
        CHECK(!vfs_resolve_subject_locked(table,&name_bob,"\\c-file",win32,false,&r));
        CHECK(vfs_resolve_subject_locked(table,&name_bob,"C:private.txt",win32,false,&r)==-EACCES);
        CHECK(!vfs_resolve_subject_locked(table,&name_alice,"C:private.txt",win32,false,&r));
        CHECK(!px_resolve_volume_subject_locked(space,3,space->root,"D:\\..\\..\\d-file",win32,false,&name_bob,&r));
        CHECK(r.node->volume==&volumes[1]);
        CHECK(px_resolve_volume_subject_locked(space,3,space->root,"D:\\..\\..\\c-file",win32,false,&name_bob,&r)==-ENOENT);
        CHECK(!vfs_resolve_subject_locked(table,&name_bob,"C:\\mnt\\d\\local-file",win32,false,&r)); CHECK(r.node->volume==&volumes[0]);
        CHECK(vfs_resolve_subject_locked(table,&name_bob,"C:\\mnt\\d\\d-file",win32,false,&r)==-ENOENT);
        CHECK(!vfs_resolve_subject_locked(table,&name_bob,"C:\\dev\\local-file",win32,false,&r)); CHECK(r.node->volume==&volumes[0]);
        CHECK(vfs_resolve_subject_locked(table,&name_bob,"C:\\dev\\null",win32,false,&r)==-ENOENT);
        CHECK(vfs_resolve_subject_locked(table,&name_bob,"D:\\d-file\\",win32,false,&r)==-ENOTDIR);
        CHECK(vfs_resolve_subject_locked(table,&name_bob,"D:/d-file///",win32,false,&r)==-ENOTDIR);
        CHECK(!vfs_resolve_subject_locked(table,&name_bob,"D:\\dir\\",win32,false,&r)); CHECK(r.trailing);
        CHECK(!vfs_resolve_subject_locked(table,&name_bob,"D:\\..\\..\\",win32,false,&r)); CHECK(r.node->mountpoint && r.node->drive==3 && r.trailing);
        CHECK(vfs_resolve_subject_locked(table,&name_alice,"C:/Windows/Profiles/u000003ef/../u00000003/private.txt",win32,false,&r)==-EACCES);
    }
    CHECK(!px_resolve_subject_locked(space,space->root,"/mnt/d/d-file",false,&name_bob,&r)); CHECK(r.node->volume==&volumes[1]);
    struct px_node *data_cwd; CHECK(!px_resolve_subject_locked(space,space->root,"/mnt/d/dir",false,&name_bob,&r)); data_cwd=r.node;
    CHECK(!px_resolve_volume_subject_locked(space,3,data_cwd,"..\\..\\d-file",false,false,&name_bob,&r)); CHECK(r.node->volume==&volumes[1]);
    CHECK(px_resolve_volume_subject_locked(space,3,data_cwd,"..\\..\\c-file",false,false,&name_bob,&r)==-ENOENT);
    CHECK(px_resolve_volume_subject_locked(space,2,private_cwd,"../u000003ef/../u00000003/private.txt",false,false,&name_alice,&r)==-EACCES);
    strcpy(table->cwd[2],"/no-such-cwd");
    CHECK(!vfs_resolve_subject_locked(table,&name_bob,"C:\\c-file",false,false,&r));
    CHECK(vfs_resolve_subject_locked(table,&name_bob,"C:c-file",false,false,&r)==-ENOENT);
    vfs_unlock(&vfs); CHECK(!vfs_chdir(table,"C:/"));
    puts("H02 review 1 paths: selected-volume confinement, physical mnt/dev, cwd-independent absolute input, checked relative traversal and terminal separators PASS");
}
static void test_shared_desktop(struct vfs_table *table)
{
    const char *dirs[]={"/Documents","/Documents/Desktop","/Documents/Desktop/Quake III Arena Demo","/Documents/Desktop/My notes"};
    for(unsigned i=0;i<4;i++) { int e=make_dir(dirs[i]); CHECK(!e || e==-EEXIST); }
    file_create(table,"C:/Documents/Desktop/Quake III Arena Demo/Install Quake III Arena Demo.lnk");
    file_create(table,"C:/Documents/Desktop/My notes/edit.txt");
    const struct security_subject *subjects[]={&name_system,&name_admin,&name_alice,&name_bob,&name_guest};
    for(unsigned who=0;who<5;who++) {
        CHECK(!authorized("/Documents/Desktop/My notes/edit.txt",space->root,subjects[who],POLICY_READ,0));
        CHECK(authorized("/Documents/Desktop/My notes/edit.txt",space->root,subjects[who],POLICY_WRITE|POLICY_REMOVE|POLICY_RENAME,0)==(who==4?-EACCES:0));
        CHECK(authorized("/Documents/Desktop/My notes",space->root,subjects[who],POLICY_ADD,0)==(who==4?-EACCES:0));
        CHECK(!authorized("/Documents/Desktop/Quake III Arena Demo/Install Quake III Arena Demo.lnk",space->root,subjects[who],POLICY_READ|POLICY_EXEC,0));
        CHECK(authorized("/Documents/Desktop/Quake III Arena Demo/Install Quake III Arena Demo.lnk",space->root,subjects[who],POLICY_WRITE|POLICY_RENAME|POLICY_REMOVE,0)==(who<2?0:-EACCES));
        CHECK(authorized(dirs[2],space->root,subjects[who],POLICY_ADD,0)==(who<2?0:-EACCES));
        for(unsigned dir=0;dir<3;dir++) CHECK(authorized(dirs[dir],space->root,subjects[who],POLICY_RENAME|POLICY_REMOVE,0)==(who? -EACCES:0));
    }
    struct fat_entry entry; CHECK(!vfs_stat(table,"C:/Documents/Desktop/Quake III Arena Demo",&entry));
    char alias[256]; ksnprintf(alias,sizeof(alias),"/Documents/Desktop/%s/Install Quake III Arena Demo.lnk",entry.alias);
    CHECK(authorized(alias,space->root,&name_alice,POLICY_WRITE,0)==-EACCES);
    CHECK(!authorized(alias,space->root,&name_alice,POLICY_READ,0));
    puts("H02 review 1 Desktop: shared files/siblings writable, designated launch anchor/aliases/ancestors protected PASS");
}
static void test_mutable_labels(struct vfs_table *table,struct owner_store *owners)
{
    file_create(table,"C:/owned-root.txt"); struct px_path r; CHECK(!resolve_path("/owned-root.txt",&r));
    struct px_node *file=r.node; struct owner_record *root=&owners->roots[0];
    root->binding=(struct owner_binding){.uid=1007,.action=1,.volume=31,.key=3,.locator=3,.generation=1};
    memcpy(root->binding.path,"/owned-root.txt",16); CHECK(!px_object_proof_locked(file,root->proof)); owners->root_count=1; owners->sequence++;
    CHECK(!authorized("/owned-root.txt",space->root,&name_alice,POLICY_WRITE,0));
    uint32_t old_first=file->entry.first; CHECK(old_first);
    int fd=file_open(client,"/owned-root.txt",O_RDWR,0); CHECK(fd>=0);
    struct file_description *description=file_fd(client,fd); CHECK(!locked_truncate(description,0));
    CHECK(!file->entry.first && file->entry.first!=old_first);
    CHECK(!file_close(client,fd));
    CHECK(!info_set("/owned-root.txt",CIUKI_FILE_INFO_CREATED|CIUKI_FILE_INFO_ATTRIBUTES,
        (struct ciuki_file_info){.created=946684800,.created_ms=420,.attributes=CIUKI_ATTR_HIDDEN}));
    CHECK(!info_set("/Windows/Profiles/u00000003",CIUKI_FILE_INFO_CREATED,
        (struct ciuki_file_info){.created=946684800,.created_ms=420}));
    owners->sequence++; /* Force both file and profile proofs to be checked again. */
    CHECK(!authorized("/owned-root.txt",space->root,&name_alice,POLICY_WRITE,0));
    CHECK(authorized("/owned-root.txt",space->root,&name_bob,POLICY_WRITE,0)==-EACCES);
    CHECK(!authorized("/Windows/Profiles/u00000003/private.txt",space->root,&name_alice,POLICY_WRITE,0));
    CHECK(authorized("/Windows/Profiles/u00000003/private.txt",space->root,&name_bob,POLICY_READ,0)==-EACCES);
    struct policy_reference ref; name_alice.refs=1; vfs_lock(&vfs,0);
    CHECK(!policy_reference_get_locked(file,&name_alice,POLICY_READ,&ref)); vfs_unlock(&vfs);
    CHECK(!remove_path_native("/owned-root.txt",false)); file_create(table,"C:/owned-root.txt");
    vfs_lock(&vfs,0); CHECK(policy_reference_check_locked(&ref,POLICY_READ)==-EIO); policy_reference_put_locked(&ref); vfs_unlock(&vfs);
    CHECK(name_alice.refs==1);
    root->retired=true; owners->sequence++;
    CHECK(authorized("/owned-root.txt",space->root,&name_alice,POLICY_WRITE,0)==-EACCES);
    puts("H02 review 1 labels: truncate and mutable creation time preserve owner/private policy; replaced retained references deny PASS");
}
int main(int argc,char **argv)
{
    CHECK(argc==3); setvbuf(stdout,0,_IONBF,0);
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram); controller.state=T_RUNNING; g_current=&controller; proc_init();
    CHECK(!fake_open(&disk[0],argv[1]) && !fake_open(&disk[1],argv[2])); disk[0].undo_enabled=disk[1].undo_enabled=true;
    CHECK(!cache_init(&cache,0)); vfs_init(&vfs);
    for(unsigned i=0;i<2;i++) { CHECK(!fat_mount(&volumes[i],&cache,&disk[i].dev,0,disk[i].dev.capacity,true)); CHECK(!vfs_attach(&vfs,volume_drive(i),&volumes[i])); }
    CHECK(!files_attach(&vfs,&space)); client_thread=make_process(proc_supervisor(),&client); g_current=client_thread->task; g_current->state=T_RUNNING;
    client->cwd=space->root; px_retain(client->cwd); client->cwd_retain=px_retain; client->cwd_release=px_release;
    struct vfs_table *table=calloc(1,sizeof(*table)); CHECK(table); CHECK(!vfs_table_init(&vfs,table,2));
    const char *dirs[]={"/Windows","/Windows/Profiles","/Windows/Profiles/u00000003","/Windows/Profiles/u000003ef",
        "/Windows/Profiles/u00000003/Settings","/Windows/Profiles/u00000003/Settings/Registry","/Systematic","/Program Files","/École"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++) { int e=make_dir(dirs[i]); CHECK(!e || e==-EEXIST); }
    file_create(table,"C:/Windows/Profiles/u00000003/private.txt"); file_create(table,"C:/Windows/Profiles/u000003ef/private.txt");
    file_create(table,"C:/Systematic/public.txt"); file_create(table,"C:/Program Files/tool.exe"); file_create(table,"C:/École/élève.txt");
    struct owner_store *owners=calloc(1,sizeof(*owners)); CHECK(owners); owners->ready=true; owners->volume=31; owners->sequence=1;
    owners->profile_count=2; profile_record(owners,0,1007,dirs[2]); profile_record(owners,1,1001,dirs[3]);
    struct policy_volume policy,data; CHECK(!policy_init(&policy,31,1,true,false,false,owners)); CHECK(policy.count<=32);
    struct owner_store *data_owners=calloc(1,sizeof(*data_owners)); CHECK(data_owners); data_owners->ready=true; data_owners->volume=32;
    CHECK(!policy_init(&data,32,2,false,false,false,data_owners));
    vfs_lock(&vfs,0); CHECK(!policy_bind_locked(&vfs,2,&policy)); CHECK(!policy_bind_locked(&vfs,3,&data)); vfs_unlock(&vfs);
    struct fat_volume duplicate=volumes[0];
    vfs_lock(&vfs,0); vfs.volumes[4]=&duplicate; vfs.generation[4]=vfs.generation[2];
    CHECK(policy_bind_locked(&vfs,4,&policy)==-EACCES); vfs.volumes[4]=&volumes[0];
    CHECK(policy_bind_locked(&vfs,4,&data)==-EACCES); CHECK(!policy_bind_locked(&vfs,4,&policy)); vfs_unlock(&vfs);
    CHECK(authorized("/mnt/e/Windows/Profiles/u00000003/private.txt",space->root,&name_bob,POLICY_READ,0)==-EACCES);
    CHECK(!authorized("/mnt/e/Windows/Profiles/u00000003/private.txt",space->root,&name_alice,POLICY_WRITE,0));
    CHECK(!authorized("/Windows/Profiles/u00000003/private.txt",space->root,&name_alice,POLICY_WRITE,0));
    CHECK(authorized("/Windows/Profiles/u00000003/private.txt",space->root,&name_bob,POLICY_READ,0)==-EACCES);
    CHECK(authorized("/Windows/Profiles/u00000003/private.txt",space->root,&name_guest,POLICY_READ,0)==-EACCES);
    struct fat_entry private_root; CHECK(!vfs_stat(table,"C:/Windows/Profiles/u00000003",&private_root));
    char private_alias[128]; ksnprintf(private_alias,sizeof(private_alias),"/Windows/Profiles/%s/private.txt",private_root.alias);
    CHECK(authorized(private_alias,space->root,&name_bob,POLICY_READ,0)==-EACCES);
    CHECK(!authorized(private_alias,space->root,&name_alice,POLICY_WRITE,0));
    CHECK(authorized("/Windows/Profiles/u000003ef/../u00000003/private.txt",space->root,&name_alice,POLICY_READ,0)==-EACCES);
    CHECK(!authorized("/Windows/Profiles/u00000003/private.txt",space->root,&name_admin,POLICY_WRITE,0));
    CHECK(authorized("/Windows/Profiles/u00000003/Settings/Registry",space->root,&name_alice,POLICY_READ,0)==-EACCES);
    struct px_node *cwd; CHECK(!authorized("/Windows/Profiles/u00000003",space->root,&name_alice,POLICY_READ,&cwd));
    test_hive_names(table); test_volume_paths(table,cwd); test_shared_desktop(table); test_mutable_labels(table,owners);
    CHECK(!authorized("./private.txt",cwd,&name_alice,POLICY_WRITE,0));
    CHECK(authorized("private.txt",cwd,0,POLICY_READ,0)==-EACCES);
    CHECK(!authorized("/Systematic/public.txt",space->root,&name_alice,POLICY_READ,0));
    CHECK(authorized("/Systematic/public.txt",space->root,&name_alice,POLICY_WRITE,0)==-EACCES);
    CHECK(!authorized("/program files/tool.exe",space->root,&name_alice,POLICY_READ,0));
    CHECK(authorized("/Program Files/tool.exe",space->root,&name_alice,POLICY_WRITE,0)==-EACCES);
    struct fat_entry programs; CHECK(!vfs_stat(table,"C:/Program Files",&programs));
    char alias[128]; ksnprintf(alias,sizeof(alias),"/%s/tool.exe",programs.alias);
    CHECK(!authorized(alias,space->root,&name_alice,POLICY_READ,0));
    CHECK(authorized(alias,space->root,&name_alice,POLICY_WRITE,0)==-EACCES);
    CHECK(!authorized("/éCOLE/ÉLÈVE.TXT",space->root,&name_bob,POLICY_READ,0));
    struct px_path resolved; vfs_lock(&vfs,0);
    CHECK(!px_resolve_volume_subject_locked(space,2,space->root,"C:\\Program Files. \\tool.exe. ",true,false,&name_bob,&resolved));
    CHECK(px_policy_check_locked(resolved.node,&name_bob,POLICY_WRITE)==-EACCES);
    CHECK(px_resolve_subject_locked(space,cwd,"private.txt.",false,&name_alice,&resolved)==-EINVAL);
    CHECK(px_resolve_subject_locked(space,cwd,"\xc0\xaf",false,&name_alice,&resolved)==-EILSEQ);
    CHECK(!px_resolve_subject_locked(space,space->root,"/mnt/d",false,&name_bob,&resolved));
    struct policy_label label; CHECK(!px_policy_label_locked(resolved.node,&label)); CHECK(label.mount==data.mount);
    struct px_node *public_node; CHECK(!px_resolve_subject_locked(space,space->root,"/Program Files/tool.exe",false,&name_bob,&resolved)); public_node=resolved.node;
    CHECK(!px_policy_label_locked(public_node,&label)); unsigned reads=disk[0].read_calls;
    struct policy_reference reference; name_bob.refs=1;
    CHECK(!policy_reference_get_locked(public_node,&name_bob,POLICY_READ,&reference));
    CHECK(policy_reference_check_locked(&reference,POLICY_WRITE)==-EACCES);
    CHECK(!policy_reference_check_locked(&reference,POLICY_READ)); policy_reference_put_locked(&reference); CHECK(name_bob.refs==1);
    CHECK(!vfs_resolve_subject_locked(table,&name_bob,"C:/Program Files/tool.exe",false,false,&resolved));
    for(unsigned i=0;i<100000;i++) CHECK(!px_policy_check_locked(public_node,&name_bob,POLICY_READ));
    CHECK(reads==disk[0].read_calls); owners->sequence++;
    CHECK(policy_check(&policy,&label,&name_bob,POLICY_READ)==-EACCES);
    CHECK(!px_policy_check_locked(public_node,&name_bob,POLICY_READ));
    owners->ready=false; CHECK(px_policy_check_locked(cwd,&name_alice,POLICY_READ)==-EIO); owners->ready=true;
    vfs_unlock(&vfs);
    /* Real recursive stream digest (root + sorted descendants), stable twice. */
    uint8_t one[32],two[32]; CHECK(!owner_vfs_tree_digest(table,"C:/Windows/Profiles/u00000003",one));
    CHECK(!owner_vfs_tree_digest(table,"C:/Windows/Profiles/u00000003",two)); CHECK(!memcmp(one,two,32));
    /* Corrupt an actual LFN checksum in the production cache. Both spellings
     * must fail, rather than treating the short alias as a different label. */
    CHECK(programs.lfn_count); uint32_t cluster=programs.parent, index=programs.lfn_index;
    for(unsigned skip=index/(volumes[0].spc*16u);skip;skip--) CHECK(!fat_get_cluster(&volumes[0],cluster,&cluster));
    uint64_t sector=volumes[0].start+volumes[0].data_sector+(uint64_t)(cluster-2)*volumes[0].spc+(index/16)%volumes[0].spc;
    uint8_t saved[512],broken[512]; CHECK(!cache_read(&cache,&disk[0].dev,sector,saved)); memcpy(broken,saved,512); broken[(index%16)*32+13]^=1;
    CHECK(!cache_write(&cache,&disk[0].dev,sector,broken));
    CHECK(authorized(alias,space->root,&name_bob,POLICY_READ,0)==-EILSEQ);
    CHECK(authorized("/Program Files/tool.exe",space->root,&name_bob,POLICY_READ,0)==-EILSEQ);
    CHECK(!cache_write(&cache,&disk[0].dev,sector,saved));
    CHECK(!authorized(alias,space->root,&name_bob,POLICY_READ,0));
    /* Deliberately bypass policy through the historical internal test helper
     * to emulate a stale/replaced slot. The new resolver must not rebind it. */
    CHECK(!rename_path("/Windows/Profiles/u00000003","/Windows/Profiles/old-root"));
    CHECK(!make_dir("/Windows/Profiles/u00000003")); file_create(table,"C:/Windows/Profiles/u00000003/private.txt");
    CHECK(authorized("/Windows/Profiles/u00000003/private.txt",space->root,&name_alice,POLICY_READ,0)==-EIO);
    printf("H02 names: aliases, Unicode case, cwd, dotdot, prefix siblings, Win32 trimming, missing metadata; 100000 warm checks add zero reads; rules=%u PASS\n",policy.count);
    vfs_table_destroy(table); free(table); all_closed(); stop_collect(client);
    vfs_lock(&vfs,0); vfs.volumes[4]=0; vfs.policy[4]=0; px_volume_detached(&vfs,4); vfs_unlock(&vfs);
    for(unsigned i=0;i<2;i++) CHECK(!vfs_detach(&vfs,volume_drive(i)));
    vfs_destroy(&vfs); cache_destroy(&cache); for(unsigned i=0;i<2;i++) { fake_reset(&disk[i]); fake_close(&disk[i]); }
    free(owners); free(data_owners); kfree(supervisor_task); supervisor_task=0; CHECK(!pages_used && !heap_blocks && !fs_host_contig_live); free(ram); ram=0;
    puts("H02 resolved production objects: PASS; zero pages/heap/fds"); return 0;
}
