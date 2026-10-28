/* H04: production native syscalls, fd table, POSIX namespace, legacy VFS
 * tables, Win32 drive service, executable snapshots and the managed-volume
 * policy/OWNER code on the real FAT/VFS with a fake block device. The only
 * fakes are the disk, the OWNER storage blobs and the Win32 handle table.
 * Expected results are literal (independent of the implementation's labels).
 * SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "../proc/files_test.c"
#include <ciuki/w32_core.h>
#include <ciuki/w32_objects.h>
#include "../../../src/kernel/fs/owner_internal.h"
#include "../../../src/kernel/fs/owner_vfs.h"

/* ---- Win32 object table stub (same shape as kernel32_files.c) ---------- */
void w32_refusal_queue(struct process *p,uint32_t why,const char *name,uint32_t error){(void)p;(void)why;(void)name;(void)error;}
static struct w32_object *handles[64];
static unsigned core_allocations;
void *w32_alloc(uint32_t n) { void *p = calloc(1, n); if (p) core_allocations++; return p; }
void w32_free(void *p, uint32_t n) { (void)n; if (p) { core_allocations--; free(p); } }
struct w32_object *w32_object_new(uint32_t type)
{ struct w32_object *o = w32_alloc(sizeof(*o)); if (o) { o->type = type; o->refs = 1; } return o; }
void w32_retain(struct w32_object *o) { if (o) o->refs++; }
extern void w32_find_destroy(struct w32_object *);
void w32_release(struct w32_object *o)
{
    if (o && !--o->refs) {
        if (o->type == W32_OBJ_FILE && o->u.file.description) o->u.file.description->object.release(&o->u.file.description->object);
        if (o->type == W32_OBJ_FIND) w32_find_destroy(o);
        w32_free(o, sizeof(*o));
    }
}
struct w32_object *w32_lookup(struct proc_thread *t, uint32_t h, uint32_t type)
{ (void)t; struct w32_object *o = h && !(h & 3) && h / 4 < 64 ? handles[h / 4] : 0; return o && o->type == type ? o : 0; }
uint32_t w32_handle_install(struct process *p, struct w32_object *o, uint32_t access, bool inherit, uint32_t *h)
{
    (void)p; (void)access; (void)inherit;
    for (unsigned i = 1; i < 64; i++) if (!handles[i]) { handles[i] = o; w32_retain(o); *h = i * 4; return 0; }
    return 4;
}
static void w32_close(uint32_t h) { CHECK(h / 4 < 64 && handles[h / 4]); struct w32_object *o = handles[h / 4]; handles[h / 4] = 0; w32_release(o); }
#include "../../../src/kernel/proc/win32/files.c"

/* ---- OWNER storage: in-memory blobs, the only OWNER boundary fake ------ */
struct blob { uint8_t *data; size_t bytes; };
static struct blob owner_files[5];
static void blob_put(struct blob *b,const void *v,size_t n)
{ free(b->data); b->data=malloc(n?n:1); CHECK(b->data); memcpy(b->data,v,n); b->bytes=n; }
static int o_read(void *c,unsigned i,void *p,size_t cap,size_t *n)
{ (void)c; if(!owner_files[i].data) return -ENOENT; if(cap<owner_files[i].bytes) return -ENOSPC; *n=owner_files[i].bytes; memcpy(p,owner_files[i].data,*n); return 0; }
static int o_write(void *c,unsigned i,const void *p,size_t n) { (void)c; blob_put(&owner_files[i],p,n); return 0; }
static int o_flush(void *c,unsigned i) { (void)c; (void)i; return 0; }
static int o_barrier(void *c) { (void)c; return 0; }
static int o_manifest(void *c,uint64_t g,bool cp,void *p,size_t cap,size_t *n) { (void)c;(void)g;(void)cp;(void)p;(void)cap;(void)n; return -ENOENT; }
static int o_tree(void *c,uint64_t g,uint32_t uid,uint8_t d[32]) { (void)c;(void)g;(void)uid; memset(d,1,32); return 0; }
static int o_reserve(void *c,const struct owner_request *q,struct owner_record *r,bool re) { (void)c;(void)q;(void)r;(void)re; return -EIO; }
static int o_publish(void *c,const struct owner_request *q,struct owner_record *r) { (void)c;(void)q;(void)r; return -EIO; }
static int o_release(void *c,const struct owner_request *q,bool done) { (void)c;(void)q;(void)done; return 0; }
static int o_intent(void *c,const struct owner_request *q) { (void)c;(void)q; return 0; }
static int o_current(void *c,uint64_t *g,uint8_t d[32]) { (void)c; *g=0; memset(d,0,32); return 0; }
static int o_marker(void *c,const struct owner_request *q,uint64_t h,uint32_t u) { (void)c;(void)q;(void)h;(void)u; return -ENOENT; }
static int o_import(void *c,uint64_t g,uint32_t o,uint32_t u,const char *p,const char *d) { (void)c;(void)g;(void)o;(void)u;(void)p;(void)d; return -EACCES; }
static int o_occupied(void *c,const char *root) { (void)c;(void)root; return 0; }
static int o_verify(void *c,const struct owner_record *r) { (void)c; return ozero(r->proof,32)?-EIO:0; }
static const uint32_t sid[3]={17,31,99};

/* ---- identities --------------------------------------------------------- */
struct actor { struct process *p; struct proc_thread *t; const char *name; };
static struct actor alice, bob, admin, guest, systemp;
static void identity(struct actor *a,const char *name,uint32_t uid,unsigned count,uint32_t g0,uint32_t g1)
{
    a->t=make_process(proc_supervisor(),&a->p); a->name=name;
    CHECK(a->p->cwd==space->root); /* inherited from PID 1 (files_bootstrap), retained by proc_prepare */
    struct ciuki_credential *c=&a->p->subject->credential;
    if(uid) { c->uid=uid; c->primary_gid=g0==544?545:g0; c->group_count=count; c->groups[0]=g0; c->groups[1]=count>1?g1:0; c->flags=1; }
}
static void as(struct actor *a) { g_current=a->t->task; g_current->state=T_RUNNING; }
static void aput(struct actor *a,uint32_t va,const char *s) { CHECK(!ua_write(a->p->memory,va,s,(uint32_t)strlen(s)+1)); }
#define PATH2 (USER + 512)
#define DATA (USER + 1536)
static int32_t sys1(struct actor *a,uint32_t nr,const char *path,uint32_t c,uint32_t d)
{ as(a); aput(a,USER,path); return call(nr,USER,c,d,0,0); }
static int32_t sys_open(struct actor *a,const char *path,uint32_t flags) { return sys1(a,CIUKI_SYS_OPEN,path,flags,0666); }
static int32_t sys_rename(struct actor *a,const char *from,const char *to)
{ as(a); aput(a,USER,from); aput(a,PATH2,to); return call(CIUKI_SYS_RENAME,USER,PATH2,0,0,0); }
static int32_t sys_rw(struct actor *a,int32_t fd,bool write,const char *data,uint32_t n)
{ as(a); if(write) CHECK(!ua_write(a->p->memory,DATA,data,n)); return call(write?CIUKI_SYS_WRITE:CIUKI_SYS_READ,(uint32_t)fd,DATA,n,0,0); }
static int32_t sys_close(struct actor *a,int32_t fd) { as(a); return call(CIUKI_SYS_CLOSE,(uint32_t)fd,0,0,0,0); }
static int32_t sys_info_set(struct actor *a,const char *path,uint32_t mask,uint32_t attributes)
{
    as(a); aput(a,USER,path);
    struct ciuki_file_info record={.version=CIUKI_FILE_INFO_VERSION,.size=sizeof(record),.operation=CIUKI_FILE_INFO_SET,
        .mask=mask,.attributes=attributes,.created=946684800};
    CHECK(!ua_write(a->p->memory,DATA,&record,sizeof(record)));
    return call(CIUKI_SYS_FILE_INFO,USER,DATA,sizeof(record),0,0);
}
static int32_t sys_getdents(struct actor *a,int32_t fd) { as(a); return call(CIUKI_SYS_GETDENTS,(uint32_t)fd,DATA,sizeof(struct ciuki_dirent),0,0); }
static uint32_t w32(struct actor *a,unsigned action,const char *path,uint32_t access,unsigned disposition,uint32_t *result)
{
    as(a); aput(a,USER,path);
    struct ciuki_w32_file_args args={.action=action,.path=USER,.access=access,.share=7,.disposition=disposition,
        .buffer=DATA,.bytes=action==W32_F_FIRST || action==W32_F_STAT ? sizeof(struct ciuki_w32_file_info) : 260};
    CHECK(!w32_file_call(a->t,&args));
    if(result) *result=args.result;
    return args.error;
}
static uint32_t w32_move(struct actor *a,const char *from,const char *to)
{
    as(a); aput(a,USER,from); aput(a,PATH2,to);
    struct ciuki_w32_file_args args={.action=W32_F_MOVE,.path=USER,.path2=PATH2};
    CHECK(!w32_file_call(a->t,&args)); return args.error;
}
static void kernel_file(const char *path,const char *data)
{
    struct vfs_table t; CHECK(!vfs_table_init(&vfs,&t,2));
    int fd=vfs_open(&t,path,VFS_CREATE|VFS_WRITE|VFS_TRUNCATE,VFS_DENY_NONE,0); CHECK(fd>=0);
    size_t n; CHECK(!vfs_write(&t,fd,data,strlen(data),&n) && n==strlen(data)); CHECK(!vfs_close(&t,fd));
    vfs_table_destroy(&t);
}
static uint32_t kernel_size(const char *path)
{
    struct vfs_table t; CHECK(!vfs_table_init(&vfs,&t,2)); struct fat_entry e;
    int r=vfs_stat(&t,path,&e); vfs_table_destroy(&t); return r?UINT32_MAX:e.size;
}
static bool kernel_exists(const char *path) { return kernel_size(path)!=UINT32_MAX; }

/* ---- spy: device I/O issued while denied operations run ------------------ */
static int (*real_read)(struct blkdev *,uint64_t,uint32_t,void *);
static int (*real_write)(struct blkdev *,uint64_t,uint32_t,const void *);
static bool spying; static uint64_t private_lo[4], private_hi[4]; static unsigned private_count, private_reads, spy_writes;
static int spy_read(struct blkdev *d,uint64_t lba,uint32_t n,void *buf)
{
    if(spying) for(unsigned i=0;i<private_count;i++) if(lba<private_hi[i] && lba+n>private_lo[i]) private_reads++;
    return real_read(d,lba,n,buf);
}
static int spy_write(struct blkdev *d,uint64_t lba,uint32_t n,const void *buf)
{ if(spying) spy_writes++; return real_write(d,lba,n,buf); }
static void private_cluster(uint32_t cluster)
{
    struct fat_volume *v=&volumes[0]; CHECK(private_count<4 && cluster>=2);
    private_lo[private_count]=v->start+v->data_sector+(uint64_t)(cluster-2)*v->spc;
    private_hi[private_count]=private_lo[private_count]+v->spc; private_count++;
}
static void spy_begin(void)
{
    vfs_lock(&vfs,0); vfs_reserve_locked(&vfs,vfs_drive_mask(2)); vfs_unlock(&vfs);
    struct vfs_table t; CHECK(!vfs_table_init(&vfs,&t,2)); CHECK(!vfs_flush_drive(&t,2)); vfs_table_destroy(&t);
    cache_invalidate(&cache,&disk[0].dev,false); private_reads=spy_writes=0; spying=true;
}
static void spy_end(const char *what)
{
    spying=false;
    if(private_reads || spy_writes) printf("spy %s: private reads=%u writes=%u\n",what,private_reads,spy_writes);
    CHECK(!private_reads && !spy_writes);
}

static struct owner_store *owners;
static struct policy_volume policy;
#define ALICE_HOME "/Windows/Profiles/u00000003"
#define BOB_HOME "/Windows/Profiles/u000003e9"
static void profile_record(unsigned index,uint32_t uid,const char *path)
{
    struct px_path r; vfs_lock(&vfs,0); CHECK(!px_resolve_locked(space,space->root,path,false,&r));
    struct owner_record *o=&owners->profiles[index]; memset(o,0,sizeof(*o));
    o->binding=(struct owner_binding){.uid=uid,.action=OWNER_CREATE,.volume=31,.key=100+index,.locator=100+index,.generation=1};
    memcpy(o->binding.path,path,strlen(path)+1); CHECK(!px_object_proof_locked(r.node,o->proof)); vfs_unlock(&vfs);
}
static void layout(void)
{
    const char *dirs[]={"/Windows","/Windows/Profiles",ALICE_HOME,BOB_HOME,"/Program Files","/Documents","/System",
        "/System/Win32","/System/Security","/OLDGAME"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++) { int e=make_dir(dirs[i]); CHECK(!e || e==-EEXIST); }
    kernel_file("C:" ALICE_HOME "/private.txt","alice private data");
    kernel_file("C:" ALICE_HOME "/a.txt","alice movable");
    kernel_file("C:" ALICE_HOME "/app.elf","\177ELF not really an image");
    kernel_file("C:" BOB_HOME "/bob.txt","bob private data");
    kernel_file("C:/Program Files/tool.exe","program bytes");
    kernel_file("C:/Documents/shared.txt","shared data");
    kernel_file("C:/System/Win32/kernel32.dll","builtin");
    kernel_file("C:/System/other.dat","machine data");
    kernel_file("C:/System/Security/secret.dat","secret");
    kernel_file("C:/OLDGAME/data.dat","unowned public root data");
    kernel_file("D:/disk.txt","fixed unmanaged volume");
}
static void bind_policy(void)
{
    owners=calloc(1,sizeof(*owners)); CHECK(owners);
    struct owner_backend io={0,o_read,o_write,o_flush,o_barrier,o_manifest,o_tree,o_reserve,o_publish,o_release,
        o_intent,o_current,o_marker,o_import,o_occupied,o_verify};
    CHECK(!owner_open(owners,&io,sid,31,true));
    owners->profile_count=2; profile_record(0,1007,ALICE_HOME); profile_record(1,1001,BOB_HOME); owners->sequence++;
    CHECK(!policy_init(&policy,31,1,true,false,false,owners));
    vfs_lock(&vfs,0); CHECK(!policy_bind_locked(&vfs,2,&policy)); vfs_unlock(&vfs);
}

/* K-Q4 before binding: the unmanaged system volume keeps the Win32 ban. */
static void test_unbound_win32_ban(void)
{
    uint32_t h;
    CHECK(w32(&admin,W32_F_OPEN,"C:\\System\\other.dat",0xc0000000u,3,&h)==5);
    CHECK(!w32(&admin,W32_F_OPEN,"C:\\System\\other.dat",0x80000000u,3,&h)); w32_close(h);
    CHECK(sys_open(&bob,"/Documents/shared.txt",O_WRONLY)==-EACCES); /* unmanaged fixed: Standard read-only */
    int32_t fd=sys_open(&admin,"/Documents/shared.txt",O_WRONLY); CHECK(fd>=0); CHECK(!sys_close(&admin,fd));
    puts("H04 unmanaged system volume: Win32 all-System write ban retained, Standard read-only, administrator writes PASS");
}
static void test_profiles_native(void)
{
    struct actor *readers[]={&alice,&admin,&systemp}, *denied[]={&bob,&guest};
    for(unsigned i=0;i<3;i++) { int32_t fd=sys_open(readers[i],ALICE_HOME "/private.txt",O_RDONLY); CHECK(fd>=0);
        CHECK(sys_rw(readers[i],fd,false,0,18)==18); CHECK(!sys_close(readers[i],fd)); }
    struct fat_entry home; struct vfs_table kt; CHECK(!vfs_table_init(&vfs,&kt,2)); CHECK(!vfs_stat(&kt,"C:" ALICE_HOME,&home));
    struct fat_entry file; CHECK(!vfs_stat(&kt,"C:" ALICE_HOME "/private.txt",&file)); vfs_table_destroy(&kt);
    private_cluster(home.first); private_cluster(file.first);
    char alias[128]; ksnprintf(alias,sizeof(alias),"/Windows/Profiles/%s/private.txt",home.alias);
    char dotdot[128]; ksnprintf(dotdot,sizeof(dotdot),BOB_HOME "/../u00000003/private.txt");
    spy_begin();
    for(unsigned i=0;i<2;i++) {
        struct actor *a=denied[i];
        CHECK(sys_open(a,ALICE_HOME "/private.txt",O_RDONLY)==-EACCES);
        CHECK(sys_open(a,alias,O_RDONLY)==-EACCES);
        CHECK(sys_open(a,ALICE_HOME "/new.txt",O_CREAT|O_RDWR)==-EACCES);
        CHECK(sys_open(a,ALICE_HOME "/private.txt",O_WRONLY|O_TRUNC)==-EACCES);
        as(a); aput(a,USER,ALICE_HOME "/private.txt"); CHECK(call(CIUKI_SYS_STAT,USER,DATA,0,0,0)==-EACCES);
        CHECK(sys1(a,CIUKI_SYS_UNLINK,ALICE_HOME "/private.txt",0,0)==-EACCES);
        CHECK(sys1(a,CIUKI_SYS_MKDIR,ALICE_HOME "/sub",0777,0)==-EACCES);
        CHECK(sys1(a,CIUKI_SYS_CHDIR,ALICE_HOME,0,0)==-EACCES);
        CHECK(sys_rename(a,ALICE_HOME "/private.txt","/Documents/stolen.txt")==-EACCES);
        CHECK(sys_info_set(a,ALICE_HOME "/private.txt",CIUKI_FILE_INFO_ATTRIBUTES,0)==-EACCES);
        CHECK(proc_get_file_ops()->open(a->p->cwd,ALICE_HOME "/app.elf",&(struct ciuki_file){0})==-EACCES);
    }
    CHECK(sys_open(&bob,dotdot,O_RDONLY)==-EACCES); /* not collapsed before traversal */
    spy_end("profile denial");
    CHECK(!kernel_exists("C:/Documents/stolen.txt") && kernel_size("C:" ALICE_HOME "/private.txt")==18);
    struct ciuki_file image={0}; as(&alice);
    CHECK(!proc_get_file_ops()->open(alice.p->cwd,ALICE_HOME "/app.elf",&image)); image.close(image.cookie);
    struct process *child; CHECK(!proc_prepare(bob.p,&child)); CHECK(child->cwd==space->root);
    CHECK(security_exec_open(child,ALICE_HOME "/app.elf",&image)==-EACCES);
    CHECK(!security_exec_open(child,"/Program Files/tool.exe",&image)); image.close(image.cookie);
    proc_discard(child);
    puts("H04 profiles: Bob/Guest open/alias/dotdot/create/O_TRUNC/stat/unlink/mkdir/chdir/rename/attributes/exec denied; spy: no private read, no write PASS");
}
static void test_descriptions(void)
{
    int32_t fd=sys_open(&admin,"/Program Files/tool.exe",O_RDONLY); CHECK(fd>=0);
    CHECK(sys_rw(&admin,fd,true,"x",1)==-EBADF);
    as(&admin); CHECK(call(CIUKI_SYS_FTRUNCATE,(uint32_t)fd,0,0,0,0)==-EBADF);
    CHECK(!sys_close(&admin,fd));
    spy_begin();
    CHECK(sys_open(&bob,"/Program Files/tool.exe",O_WRONLY|O_TRUNC)==-EACCES);
    CHECK(sys_open(&bob,"/Program Files/new.exe",O_CREAT|O_WRONLY)==-EACCES);
    CHECK(sys_info_set(&bob,"/Program Files/tool.exe",CIUKI_FILE_INFO_ATTRIBUTES,0)==-EACCES);
    spy_end("program denial");
    CHECK(kernel_size("C:/Program Files/tool.exe")==13);
    /* A retained description of a stopping/closed subject gets no fresh I/O. */
    fd=sys_open(&alice,ALICE_HOME "/private.txt",O_RDWR); CHECK(fd>=0);
    alice.p->subject->live=false; CHECK(sys_rw(&alice,fd,false,0,4)==-EACCES); CHECK(sys_rw(&alice,fd,true,"x",1)==-EACCES);
    alice.p->subject->live=true; CHECK(sys_rw(&alice,fd,false,0,4)==4);
    /* Rights only shrink: a description used by another credential checks
     * that caller's own current permission. */
    struct file_description *d=file_fd(alice.p,fd); CHECK(d && d->subject==alice.p->subject);
    CHECK(d->granted&POLICY_WRITE); as(&bob); vfs_lock(&vfs,0);
    CHECK(file_description_authorize_locked(d,POLICY_READ)==-EACCES); vfs_unlock(&vfs);
    as(&admin); vfs_lock(&vfs,0); CHECK(!file_description_authorize_locked(d,POLICY_READ)); vfs_unlock(&vfs);
    /* Warm description checks are generation-checked labels: no storage I/O. */
    as(&alice); unsigned warm_reads=disk[0].read_calls; vfs_lock(&vfs,0);
    for(unsigned i=0;i<100000;i++) CHECK(!file_description_authorize_locked(d,POLICY_READ|POLICY_WRITE));
    vfs_unlock(&vfs); CHECK(disk[0].read_calls==warm_reads);
    /* Mapped-section paging/writeback: the original admission only. */
    size_t done=0; char page[8]; vfs_lock(&vfs,0);
    CHECK(!file_section_io_locked(d,page,4,false,true,0,&done) && done==4); vfs_unlock(&vfs);
    CHECK(!sys_close(&alice,fd));
    fd=sys_open(&alice,ALICE_HOME "/private.txt",O_RDONLY); CHECK(fd>=0); d=file_fd(alice.p,fd);
    CHECK(!(d->granted&POLICY_WRITE)); vfs_lock(&vfs,0);
    CHECK(file_section_io_locked(d,page,1,true,true,0,&done)==-EBADF); d->flags=O_RDWR;
    CHECK(file_section_io_locked(d,page,1,true,true,0,&done)==-EACCES); d->flags=O_RDONLY; vfs_unlock(&vfs);
    CHECK(!sys_close(&alice,fd));
    fd=sys_open(&alice,ALICE_HOME,O_RDONLY|O_DIRECTORY); CHECK(fd>=0);
    int32_t listed=sys_getdents(&alice,fd); if(listed<=0) printf("getdents=%d\n",listed); CHECK(listed>0); CHECK(!sys_close(&alice,fd));
    puts("H04 descriptions: read-only fd never writable, O_TRUNC/create/attributes denied with no write, dead subject no I/O, caller rights per call, section admission bounded PASS");
}
static void test_creator_roots(void)
{
    CHECK(!sys1(&alice,CIUKI_SYS_MKDIR,"/GAME",0777,0));
    CHECK(owners->root_count==1 && owners->roots[0].binding.uid==1007 && !owners->roots[0].pending && !owners->root_key);
    int32_t fd=sys_open(&alice,"/GAME/save.dat",O_CREAT|O_RDWR); CHECK(fd>=0);
    CHECK(sys_rw(&alice,fd,true,"save",4)==4); CHECK(!sys_close(&alice,fd));
    fd=sys_open(&bob,"/GAME/save.dat",O_RDONLY); CHECK(fd>=0); CHECK(!sys_close(&bob,fd));
    fd=sys_open(&guest,"/GAME/save.dat",O_RDONLY); CHECK(fd>=0); CHECK(!sys_close(&guest,fd));
    CHECK(sys_open(&bob,"/GAME/save.dat",O_WRONLY)==-EACCES);
    CHECK(sys1(&bob,CIUKI_SYS_UNLINK,"/GAME/save.dat",0,0)==-EACCES);
    CHECK(sys1(&bob,CIUKI_SYS_MKDIR,"/GAME/mods",0777,0)==-EACCES);
    CHECK(sys1(&guest,CIUKI_SYS_MKDIR,"/GUESTROOT",0777,0)==-EACCES);
    fd=sys_open(&admin,"/GAME/save.dat",O_WRONLY); CHECK(fd>=0); CHECK(!sys_close(&admin,fd));
    CHECK(sys_rename(&bob,"/GAME","/GAME2")==-EACCES);
    CHECK(!sys_rename(&alice,"/GAME","/GAME2"));
    CHECK(!strcmp(owners->roots[0].binding.path,"/GAME2") && !owners->roots[0].pending && !owners->root_key);
    fd=sys_open(&alice,"/GAME2/save.dat",O_RDWR); CHECK(fd>=0); CHECK(!sys_close(&alice,fd));
    CHECK(!sys1(&bob,CIUKI_SYS_MKDIR,"/GAME",0777,0)); /* freed name: Bob's own creator root */
    CHECK(owners->root_count==2 && owners->roots[1].binding.uid==1001);
    CHECK(sys_open(&alice,"/GAME/x",O_CREAT|O_RDWR)==-EACCES);
    /* Root files: creator record, open entry cannot be deleted, deletion
     * retires the record and the name stays a tombstone. */
    fd=sys_open(&alice,"/notes.txt",O_CREAT|O_RDWR); CHECK(fd>=0);
    CHECK(owners->root_count==3 && owners->roots[2].binding.uid==1007);
    CHECK(sys1(&alice,CIUKI_SYS_UNLINK,"/notes.txt",0,0)==-EBUSY);
    CHECK(!sys_close(&alice,fd));
    CHECK(sys1(&bob,CIUKI_SYS_UNLINK,"/notes.txt",0,0)==-EACCES);
    CHECK(!sys1(&alice,CIUKI_SYS_UNLINK,"/notes.txt",0,0)); CHECK(owners->roots[2].retired && !kernel_exists("C:/notes.txt"));
    CHECK(sys_open(&alice,"/notes.txt",O_CREAT|O_RDWR)==-EEXIST);
    CHECK(sys_open(&admin,"/notes.txt",O_CREAT|O_RDWR)==-EEXIST);
    /* FAT slot reuse after deletion: a new name in the same slot is a fresh
     * unowned entry, never the retired creator's object. */
    fd=sys_open(&admin,"/slot-reuse.txt",O_CREAT|O_RDWR); CHECK(fd>=0); CHECK(!sys_close(&admin,fd));
    CHECK(owners->root_count==3);
    CHECK(sys_open(&alice,"/slot-reuse.txt",O_WRONLY)==-EACCES);
    fd=sys_open(&alice,"/slot-reuse.txt",O_RDONLY); CHECK(fd>=0); CHECK(!sys_close(&alice,fd));
    CHECK(sys1(&alice,CIUKI_SYS_RMDIR,"/GAME2",0,0)==-ENOTEMPTY);
    CHECK(!sys1(&alice,CIUKI_SYS_UNLINK,"/GAME2/save.dat",0,0));
    CHECK(!sys1(&alice,CIUKI_SYS_RMDIR,"/GAME2",0,0)); CHECK(owners->roots[0].retired);
    /* Installed/reserved names are not creator roots; ownerless public root
     * data stays administrator-writable. */
    CHECK(sys1(&alice,CIUKI_SYS_MKDIR,"/Programs",0777,0)==-EACCES);
    CHECK(sys_open(&alice,"/OLDGAME/data.dat",O_WRONLY)==-EACCES);
    fd=sys_open(&admin,"/OLDGAME/data.dat",O_WRONLY); CHECK(fd>=0); CHECK(!sys_close(&admin,fd));
    CHECK(!sys1(&admin,CIUKI_SYS_MKDIR,"/ADMINROOT",0777,0) && owners->root_count==3);
    /* Interrupted creator intents (crash between reserve/change and finish)
     * are reconciled from namespace evidence by the next root mutation. */
    struct owner_record crashed; uint8_t proof[32]; memset(proof,0x5a,32);
    CHECK(!owner_root_reserve(owners,alice.p->subject,"/CRASHED",proof,&crashed) && owners->root_key);
    CHECK(!sys1(&bob,CIUKI_SYS_MKDIR,"/BOBROOT",0777,0));
    CHECK(!owners->root_key && owners->roots[3].retired && !strcmp(owners->roots[4].binding.path,"/BOBROOT"));
    CHECK(sys1(&alice,CIUKI_SYS_MKDIR,"/CRASHED",0777,0)==-EEXIST);
    CHECK(!owner_root_change(owners,bob.p->subject,owners->roots[1].binding.key,"/GAMEX",owners->roots[1].proof));
    CHECK(!rename_path("/GAME","/GAMEX")); /* the namespace step completed, its finish was lost */
    CHECK(!sys1(&alice,CIUKI_SYS_MKDIR,"/ALICE2",0777,0));
    CHECK(!owners->root_key && !strcmp(owners->roots[1].binding.path,"/GAMEX") && !owners->roots[1].pending);
    fd=sys_open(&bob,"/GAMEX/after-crash.txt",O_CREAT|O_RDWR); CHECK(fd>=0); CHECK(!sys_close(&bob,fd));
    CHECK(sys_open(&alice,"/GAMEX/after-crash.txt",O_WRONLY)==-EACCES);
    puts("H04 creator roots: Standard mkdir/create commit OWNER records, other Users read, Guest read-only, owner/admin rename transaction, open-entry EBUSY, retire, tombstone, slot reuse unowned, interrupted create/rename reconciled PASS");
}
static void test_moves(void)
{
    CHECK(sys_rename(&alice,ALICE_HOME "/a.txt","/Documents/a.txt")==-EACCES);
    CHECK(sys_rename(&bob,"/Documents/shared.txt","/Program Files/shared.txt")==-EACCES);
    CHECK(sys_rename(&bob,"/Program Files/tool.exe","/Documents/tool.exe")==-EACCES);
    CHECK(!sys_rename(&alice,"/Documents/shared.txt","/Documents/shared2.txt"));
    CHECK(sys_rename(&alice,"/Documents/shared2.txt",ALICE_HOME "/shared2.txt")==-EACCES);
    int32_t fd=sys_open(&alice,ALICE_HOME "/a.txt",O_RDWR); CHECK(fd>=0);
    CHECK(sys_rename(&admin,ALICE_HOME "/a.txt","/Documents/a.txt")==-EBUSY);
    CHECK(kernel_exists("C:" ALICE_HOME "/a.txt") && !kernel_exists("C:/Documents/a.txt"));
    CHECK(sys_rw(&alice,fd,false,0,5)==5); CHECK(!sys_close(&alice,fd));
    CHECK(!sys_rename(&admin,ALICE_HOME "/a.txt","/Documents/a.txt"));
    int32_t other=sys_open(&bob,"/Documents/a.txt",O_RDWR); CHECK(other>=0); CHECK(!sys_close(&bob,other));
    CHECK(sys_rename(&admin,"/Documents/a.txt","/System/Security/a.txt")==-EACCES);
    CHECK(sys_rename(&admin,"/Documents/a.txt","/Windows/USER.CRG")==-EACCES);
    CHECK(!sys1(&admin,CIUKI_SYS_MKDIR,BOB_HOME "/moved",0777,0));
    CHECK(sys1(&admin,CIUKI_SYS_CHDIR,BOB_HOME "/moved",0,0)==0);
    CHECK(sys_rename(&admin,BOB_HOME "/moved","/Documents/moved")==-EBUSY); /* admin cwd inside */
    CHECK(!sys1(&admin,CIUKI_SYS_CHDIR,"/",0,0));
    puts("H04 rename endpoints: source/destination/name rules, cross-root move Standard EACCES, administrator EBUSY unchanged while open or cwd PASS");
}
static void test_unmanaged_and_guest(void)
{
    int32_t fd=sys_open(&bob,"/mnt/d/disk.txt",O_RDONLY); CHECK(fd>=0); CHECK(!sys_close(&bob,fd));
    CHECK(sys_open(&bob,"/mnt/d/bob.txt",O_CREAT|O_RDWR)==-EACCES);
    fd=sys_open(&admin,"/mnt/d/admin.txt",O_CREAT|O_RDWR); CHECK(fd>=0); CHECK(!sys_close(&admin,fd));
    vfs.drive_flags[3]|=VFS_DRIVE_REMOVABLE;
    fd=sys_open(&bob,"/mnt/d/bob.txt",O_CREAT|O_RDWR); CHECK(fd>=0); CHECK(!sys_close(&bob,fd));
    CHECK(sys_open(&guest,"/mnt/d/guest.txt",O_CREAT|O_RDWR)==-EACCES);
    fd=sys_open(&guest,"/mnt/d/disk.txt",O_RDONLY); CHECK(fd>=0); CHECK(!sys_close(&guest,fd));
    vfs.drive_flags[3]&=~VFS_DRIVE_REMOVABLE;
    fd=sys_open(&guest,"/Documents/shared2.txt",O_RDONLY); CHECK(fd>=0); CHECK(!sys_close(&guest,fd));
    CHECK(sys_open(&guest,"/Documents/shared2.txt",O_WRONLY)==-EACCES);
    fd=sys_open(&bob,"/Documents/shared2.txt",O_WRONLY); CHECK(fd>=0); CHECK(!sys_close(&bob,fd));
    CHECK(sys_open(&admin,"/System/Security/secret.dat",O_RDONLY)==-EACCES);
    fd=sys_open(&systemp,"/System/Security/secret.dat",O_RDONLY); CHECK(fd>=0); CHECK(!sys_close(&systemp,fd));
    fd=sys_open(&bob,"/dev/null",O_RDWR); CHECK(fd>=0); CHECK(!sys_close(&bob,fd));
    puts("H04 unmanaged/removable/Guest/SECRET/devices: fixed unmanaged admin-writable, removable user-writable, Guest read-only, SYSTEM-only secrets PASS");
}
static void test_win32(void)
{
    uint32_t h;
    spy_begin();
    CHECK(w32(&bob,W32_F_OPEN,"C:\\Windows\\Profiles\\u00000003\\private.txt",0x80000000u,3,&h)==5);
    CHECK(w32(&bob,W32_F_FIRST,"C:\\Windows\\Profiles\\u00000003\\*.*",0,0,&h)==5);
    CHECK(w32(&bob,W32_F_STAT,"C:\\Windows\\Profiles\\u00000003\\private.txt",0,0,0)==5);
    CHECK(w32(&bob,W32_F_DELETE,"C:\\Windows\\Profiles\\u00000003\\private.txt",0,0,0)==5);
    CHECK(w32(&bob,W32_F_MKDIR,"C:\\Windows\\Profiles\\u00000003\\X",0,0,0)==5);
    CHECK(w32(&bob,W32_F_ATTR,"C:\\Program Files\\tool.exe",0,0,0)==5);
    CHECK(w32_move(&bob,"C:\\Program Files\\tool.exe","C:\\Documents\\tool.exe")==5);
    as(&bob); aput(&bob,USER,"C:\\Windows\\Profiles\\u00000003\\private.txt");
    struct ciuki_w32_file_args full={.action=W32_F_FULLPATH,.flags=1,.path=USER,.buffer=DATA,.bytes=260};
    CHECK(!w32_file_call(bob.t,&full) && full.error==5);
    spy_end("Win32 denial");
    CHECK(!w32(&alice,W32_F_OPEN,"C:\\Windows\\Profiles\\u00000003\\private.txt",0x80000000u,3,&h)); w32_close(h);
    CHECK(!w32(&alice,W32_F_FIRST,"C:\\Windows\\Profiles\\u00000003\\*.*",0,0,&h)); w32_close(h);
    /* K-Q4: managed volume, common policy for C:\System, builtin store and
     * Standard denial retained. */
    CHECK(!w32(&admin,W32_F_OPEN,"C:\\System\\other.dat",0xc0000000u,3,&h)); w32_close(h);
    CHECK(w32(&admin,W32_F_OPEN,"C:\\System\\Win32\\kernel32.dll",0xc0000000u,3,&h)==5);
    CHECK(w32(&admin,W32_F_OPEN,"C:\\System\\Win32\\new.dll",0xc0000000u,1,&h)==5);
    CHECK(w32(&bob,W32_F_OPEN,"C:\\System\\other.dat",0xc0000000u,3,&h)==5);
    /* DELETE access (delete-on-close) needs REMOVE at open. */
    CHECK(w32(&bob,W32_F_OPEN,"C:\\Program Files\\tool.exe",0x80010000u,3,&h)==5);
    /* Win32 DeleteFile: authorized intent, deferred to the last close of the
     * same identity. */
    int32_t fd=sys_open(&bob,"/Documents/later.txt",O_CREAT|O_RDWR); CHECK(fd>=0);
    CHECK(!w32(&bob,W32_F_DELETE,"C:\\Documents\\later.txt",0,0,0)); CHECK(kernel_exists("C:/Documents/later.txt"));
    CHECK(!sys_close(&bob,fd)); CHECK(!kernel_exists("C:/Documents/later.txt"));
    CHECK(!core_allocations);
    puts("H04 Win32: open/find/stat/delete/mkdir/attr/move/fullpath denied as Bob, allowed as Alice, K-Q4 builtin store, DELETE access needs REMOVE, deferred delete by identity PASS");
}
static void test_legacy_tables(void)
{
    struct vfs_table tb,ta,tadm; CHECK(vfs_table_init_subject(&vfs,&tb,2,0)==-FS_EACCES);
    CHECK(!vfs_table_init_subject(&vfs,&tb,2,bob.p->subject)); CHECK(!vfs_table_init_subject(&vfs,&ta,2,alice.p->subject));
    CHECK(!vfs_table_init_subject(&vfs,&tadm,2,admin.p->subject));
    struct fat_entry e; struct vfs_find f;
    as(&bob); spy_begin();
    CHECK(vfs_open(&tb,"C:" ALICE_HOME "/private.txt",VFS_READ,VFS_DENY_NONE,0)==-FS_EACCES);
    CHECK(vfs_stat(&tb,"C:" ALICE_HOME "/private.txt",&e)==-FS_EACCES);
    CHECK(vfs_find_first(&tb,"C:" ALICE_HOME "/*.*",0x37,&f,&e)==-FS_EACCES);
    CHECK(vfs_delete(&tb,"C:" ALICE_HOME "/private.txt")==-FS_EACCES);
    CHECK(vfs_rename(&tb,"C:/Program Files/tool.exe","C:/Documents/t.exe")==-FS_EACCES);
    CHECK(vfs_mkdir(&tb,"C:/DOSROOT",0)==-FS_EACCES);
    CHECK(vfs_attrib(&tb,"C:/Program Files/tool.exe",0)==-FS_EACCES);
    CHECK(vfs_chdir(&tb,"C:" ALICE_HOME)==-FS_EACCES);
    CHECK(vfs_open(&tb,"C:/Program Files/tool.exe",VFS_WRITE,VFS_DENY_NONE,0)==-FS_EACCES);
    spy_end("legacy denial");
    int h=vfs_open(&tb,"C:/Program Files/tool.exe",VFS_READ,VFS_DENY_NONE,0); CHECK(h>=0);
    size_t n; CHECK(vfs_write(&tb,h,"x",1,&n)==-FS_EACCES); CHECK(vfs_handle_times(&tb,h,0x2821,0)==-FS_EACCES);
    CHECK(vfs_dup(&tb,h,&ta,-1)==-FS_EACCES); CHECK(!vfs_close(&tb,h));
    as(&alice);
    h=vfs_open(&ta,"C:" ALICE_HOME "/private.txt",VFS_READ,VFS_DENY_NONE,0); CHECK(h>=0);
    char data[32]; CHECK(!vfs_read(&ta,h,data,4,&n) && n==4);
    CHECK(!vfs_handle_times(&ta,h,0x2821,0)); /* META decided at open */
    alice.p->subject->live=false; CHECK(vfs_read(&ta,h,data,4,&n)==-FS_EACCES); alice.p->subject->live=true;
    CHECK(!vfs_close(&ta,h));
    CHECK(!vfs_find_first(&ta,"C:" ALICE_HOME "/*.*",0x37,&f,&e));
    CHECK(!vfs_chdir(&ta,"C:" ALICE_HOME)); CHECK(!vfs_stat(&ta,"private.txt",&e));
    CHECK(vfs_mkdir(&ta,"C:/DOSROOT",0)==-FS_EACCES); /* creator roots: native/Win32 only */
    as(&admin); CHECK(!vfs_mkdir(&tadm,"C:/DOSADMIN",0));
    CHECK(vfs_log_reserve(&tadm,0,4096)==-FS_EACCES);
    vfs_table_destroy(&tb); vfs_table_destroy(&ta); vfs_table_destroy(&tadm);
    puts("H04 legacy tables: subject required, Bob denied open/stat/find/delete/rename/mkdir/attrib/chdir/write/times/dup, Alice allowed, dead subject no I/O, kernel-only interfaces refused PASS");
}
static void test_lease(void)
{
    struct owner_vfs *lease=calloc(1,sizeof(*lease)); CHECK(lease);
    lease->sealed_g=7; lease->request.count=1; memcpy(lease->request.bindings[0].path,BOB_HOME,strlen(BOB_HOME)+1);
    policy.lease=lease;
    CHECK(sys_open(&systemp,BOB_HOME "/held.txt",O_CREAT|O_RDWR)==-EBUSY);
    CHECK(sys1(&admin,CIUKI_SYS_UNLINK,BOB_HOME "/bob.txt",0,0)==-EBUSY);
    CHECK(sys_rename(&admin,BOB_HOME "/bob.txt","/Documents/bob.txt")==-EBUSY);
    int32_t fd=sys_open(&systemp,"/Documents/free.txt",O_CREAT|O_RDWR); CHECK(fd>=0); CHECK(!sys_close(&systemp,fd));
    policy.lease=0; free(lease);
    fd=sys_open(&bob,BOB_HOME "/held.txt",O_CREAT|O_RDWR); CHECK(fd>=0); CHECK(!sys_close(&bob,fd));
    puts("H04 OWNER lease: profile batch fences SYSTEM/administrator create/delete/rename of reserved names, siblings free PASS");
}
static void test_missing_subject(void)
{
    struct file_description *d=file_description_new(O_RDONLY); CHECK(d);
    vfs_lock(&vfs,0);
    CHECK(px_open_as_locked(space,space->root,"/Documents/shared2.txt",O_RDONLY,d,0)==-EACCES);
    struct px_path r; CHECK(px_resolve_as_locked(space,space->root,"/Documents",false,0,&r)==-EACCES);
    CHECK(px_mkdir_as_locked(space,space->root,"/Documents/n",0777,0)==-EACCES);
    CHECK(px_remove_as_locked(space,space->root,"/Documents/shared2.txt",false,0)==-EACCES);
    CHECK(px_rename_as_locked(space,space->root,"/Documents/shared2.txt","/Documents/z",0)==-EACCES);
    vfs_unlock(&vfs); CHECK(!file_description_close(d));
    CHECK(file_image_open_as(space->root,0,"/Program Files/tool.exe",false,&(struct ciuki_file){0})==-EACCES);
    puts("H04 missing subject: every attributed entry point denies, never SYSTEM PASS");
}
int main(int argc,char **argv)
{
    CHECK(argc==3); setvbuf(stdout,0,_IONBF,0);
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram); controller.state=T_RUNNING; g_current=&controller; proc_init();
    CHECK(!fake_open(&disk[0],argv[1]) && !fake_open(&disk[1],argv[2])); disk[0].undo_enabled=disk[1].undo_enabled=true;
    real_read=disk[0].dev.read; real_write=disk[0].dev.write; disk[0].dev.read=spy_read; disk[0].dev.write=spy_write;
    CHECK(!cache_init(&cache,0)); vfs_init(&vfs);
    for(unsigned i=0;i<2;i++) { CHECK(!fat_mount(&volumes[i],&cache,&disk[i].dev,0,disk[i].dev.capacity,true)); CHECK(!vfs_attach(&vfs,volume_drive(i),&volumes[i])); }
    CHECK(!files_attach(&vfs,&space)); CHECK(!files_bootstrap(&vfs)); /* production executable ops */
    client_thread=make_process(proc_supervisor(),&client); g_current=client_thread->task; g_current->state=T_RUNNING;
    CHECK(client->cwd==space->root);
    layout();
    identity(&alice,"alice",1007,1,545,0); identity(&bob,"bob",1001,1,545,0); identity(&admin,"admin",1000,2,544,545);
    identity(&guest,"guest",501,1,546,0); identity(&systemp,"system",0,0,0,0);
    test_unbound_win32_ban();
    bind_policy();
    test_profiles_native(); test_descriptions(); test_creator_roots(); test_moves(); test_unmanaged_and_guest();
    test_win32(); test_legacy_tables(); test_lease(); test_missing_subject();
    struct actor *all[]={&alice,&bob,&admin,&guest,&systemp};
    for(unsigned i=0;i<5;i++) { as(all[i]); for(int fd=0;fd<CIUKI_OPEN_MAX;fd++) if(all[i]->p->fds[fd].object) file_close(all[i]->p,fd); stop_collect(all[i]->p); }
    g_current=client_thread->task; all_closed(); stop_collect(client);
    for(int fd=0;fd<3;fd++) if(file_fd_object(proc_supervisor(),fd)) CHECK(!file_close(proc_supervisor(),fd));
    px_release(proc_supervisor()->cwd); proc_supervisor()->cwd=0;
    struct file_ledger ledger; files_snapshot(space,&ledger);
    if(ledger.descriptions || ledger.pins) {
        printf("ledger descriptions=%u pins=%u\n",ledger.descriptions,ledger.pins);
        for(struct px_node *n=space->nodes;n;n=n->next) if(n->refs) printf("pinned %s refs=%u linked=%d\n",n->entry.name,n->refs,n->linked);
        for(struct file_description *d=space->descriptions;d;d=d->next) printf("open %s refs=%u\n",d->node->entry.name,d->references);
    }
    CHECK(!ledger.descriptions && !ledger.pins);
    vfs_lock(&vfs,0); policy_unbind_locked(&vfs,2); vfs_unlock(&vfs);
    owner_close(owners); free(owners); for(unsigned i=0;i<5;i++) free(owner_files[i].data);
    for(unsigned i=0;i<2;i++) CHECK(!vfs_detach(&vfs,volume_drive(i)));
    vfs_destroy(&vfs); cache_destroy(&cache);
    disk[0].dev.read=real_read; disk[0].dev.write=real_write;
    for(unsigned i=0;i<2;i++) { fake_reset(&disk[i]); fake_close(&disk[i]); }
    kfree(supervisor_task); supervisor_task=0;
    CHECK(!pages_used); free(ram);
    printf("H04 file enforcement: production native/Win32/legacy/exec/OWNER paths, checks=%u PASS\n",checks);
    return 0;
}
