/* H08 (sec-user-02): the protected installer store and its desktop
 * presentation anchor through the production FAT/VFS/native resolver and the
 * default system-volume policy (user-security-contract.md 6.1 and 8.3, A1-5;
 * user-security-kernel.md rule table). Same fixtures and harness as H02.
 * SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "../proc/files_test.c"
#include "../../../src/kernel/fs/owner.h"
#include "../../../src/kernel/fs/owner_vfs.h"
static struct security_subject store_alice={.credential={.version=1,.size=64,.uid=1000,.group_count=1,.groups={545},.flags=1},.live=true};
static struct security_subject store_bob={.credential={.version=1,.size=64,.uid=1001,.group_count=1,.groups={545},.flags=1},.live=true};
static struct security_subject store_guest={.credential={.version=1,.size=64,.uid=501,.group_count=1,.groups={546},.flags=1},.live=true};
static struct security_subject store_admin={.credential={.version=1,.size=64,.uid=1002,.group_count=2,.groups={544,545},.flags=1},.live=true};
static struct security_subject store_system={.credential={.version=1,.size=64,.uid=0,.group_count=1,.groups={0},.flags=1},.live=true};
#define STORE "/Programs/Installers/Quake III Arena Demo"
#define SHOW "/Documents/Desktop/Quake III Arena Demo"
static int allowed(const char *path,struct px_node *cwd,const struct security_subject *subject,unsigned access,struct px_node **out)
{
    struct px_path result; vfs_lock(&vfs,0);
    int e=px_resolve_subject_locked(space,cwd,path,false,subject,&result);
    if(!e) e=px_policy_check_locked(result.node,subject,access);
    if(!e && out) *out=result.node;
    vfs_unlock(&vfs); return e;
}
static void make_file(struct vfs_table *t,const char *path)
{
    int fd=vfs_open(t,path,VFS_CREATE|VFS_WRITE|VFS_EXCLUSIVE,VFS_DENY_NONE,0); CHECK(fd>=0);
    size_t n; CHECK(!vfs_write(t,fd,"MZ",2,&n) && n==2); CHECK(!vfs_close(t,fd));
}
static void check_store(struct vfs_table *table,const char *when)
{
    const struct security_subject *who[]={&store_system,&store_admin,&store_alice,&store_bob,&store_guest};
    const char *ancestors[]={"/Programs","/Programs/Installers",STORE,"/Documents","/Documents/Desktop",SHOW};
    const char *files[]={STORE "/Q3ADemo.exe",STORE "/Q3A_EULA.txt",SHOW "/Install Quake III Arena Demo.lnk",SHOW "/Read Me.lnk"};
    struct fat_entry entry; CHECK(!vfs_stat(table,"C:" STORE,&entry));
    char alias[256]; ksnprintf(alias,sizeof(alias),"/Programs/Installers/%s/Q3ADemo.exe",entry.alias);
    struct px_node *store_cwd=0; CHECK(!allowed(STORE,space->root,&store_alice,POLICY_READ|POLICY_TRAVERSE,&store_cwd));
    for(unsigned i=0;i<5;i++) {
        int privileged=i<2;
        /* Everyone reads and launches the exact payload and its documents. */
        for(unsigned f=0;f<4;f++) CHECK(!allowed(files[f],space->root,who[i],POLICY_READ|POLICY_EXEC,0));
        CHECK(!allowed(alias,space->root,who[i],POLICY_READ|POLICY_EXEC,0));
        /* Replacing, truncating, retargeting or re-attributing them: administrators/SYSTEM only. */
        for(unsigned f=0;f<4;f++)
            CHECK(allowed(files[f],space->root,who[i],POLICY_WRITE|POLICY_REMOVE|POLICY_RENAME|POLICY_META,0)==(privileged?0:-EACCES));
        CHECK(allowed(alias,space->root,who[i],POLICY_WRITE,0)==(privileged?0:-EACCES));
        /* Adjacent DLL/INI/script planting in the store or the presentation folder. */
        CHECK(allowed(STORE,space->root,who[i],POLICY_ADD,0)==(privileged?0:-EACCES));
        CHECK(allowed(SHOW,space->root,who[i],POLICY_ADD,0)==(privileged?0:-EACCES));
        CHECK(allowed(".",store_cwd,who[i],POLICY_ADD,0)==(privileged?0:-EACCES));  /* current-directory planting */
        /* No ancestor can be renamed or removed to swap in a lookalike. */
        for(unsigned a=0;a<6;a++) if(!privileged) CHECK(allowed(ancestors[a],space->root,who[i],POLICY_RENAME|POLICY_REMOVE,0)==-EACCES);
    }
    printf("H08 installer store %s: read/execute for all, replacement/retarget/planting/ancestor rename denied to Standard and Guest PASS\n",when);
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
    const char *dirs[]={"/Programs","/Programs/Installers",STORE,"/Documents","/Documents/Desktop",SHOW};
    for(unsigned i=0;i<6;i++) { int e=make_dir(dirs[i]); CHECK(!e || e==-EEXIST); }
    make_file(table,"C:" STORE "/Q3ADemo.exe"); make_file(table,"C:" STORE "/Q3A_EULA.txt");
    make_file(table,"C:" SHOW "/Install Quake III Arena Demo.lnk"); make_file(table,"C:" SHOW "/Read Me.lnk");
    struct owner_store *owners=calloc(1,sizeof(*owners)); CHECK(owners); owners->ready=true; owners->volume=31; owners->sequence=1;
    struct policy_volume *policy=calloc(1,sizeof(*policy)); CHECK(policy);
    CHECK(!policy_init(policy,31,1,true,false,false,owners));
    vfs_lock(&vfs,0); CHECK(!policy_bind_locked(&vfs,2,policy)); vfs_unlock(&vfs);
    check_store(table,"first boot");
    /* Reboot: a fresh policy instance over the same mounted objects. */
    vfs_lock(&vfs,0); policy_unbind_locked(&vfs,2); vfs_unlock(&vfs);
    CHECK(!policy_init(policy,31,2,true,false,false,owners));
    vfs_lock(&vfs,0); CHECK(!policy_bind_locked(&vfs,2,policy)); vfs_unlock(&vfs);
    check_store(table,"after reboot");
    vfs_lock(&vfs,0); policy_unbind_locked(&vfs,2); vfs_unlock(&vfs);
    vfs_table_destroy(table); free(table); all_closed(); stop_collect(client);
    for(unsigned i=0;i<2;i++) CHECK(!vfs_detach(&vfs,volume_drive(i)));
    vfs_destroy(&vfs); cache_destroy(&cache); for(unsigned i=0;i<2;i++) { fake_reset(&disk[i]); fake_close(&disk[i]); }
    free(owners); free(policy); kfree(supervisor_task); supervisor_task=0; CHECK(!pages_used && !heap_blocks && !fs_host_contig_live); free(ram); ram=0;
    puts("PASS H08 installer store: protected payload, presentation anchor and ancestors on the production resolver; zero pages/heap/fds");
    return 0;
}
