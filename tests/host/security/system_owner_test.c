/* sec-own (H15): the system volume's OWNER binding with the production
 * ACCOUNT service end to end. The kernel half is production code on the
 * kernel host harness (credentials, roles, service channels, PID 1's
 * owner_service_poll, the real FAT32/VFS backend, policy binding); the
 * ACCOUNT half is the sealed service's production server, kernel adapter,
 * first-boot adapter and kernel OWNER adapter (owner_kernel.c). Only the
 * native file binding of ACCOUNT's protected storage and of the adapter's
 * tree access are harness tables over the same mounted volume (the image
 * binds them to the native file API: storage_posix.c, tree_posix.c).
 * The volume comes from scripts/build_image.py's ready provisioning.
 * SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_DESKTOP_HOST_MAIN system_owner_unused_desktop_main
#define CIUKI_DESKTOP_REAL_VFS
#include "../proc/desktop_test.c"
#include "../fs/fake.h"
#include "../../../src/kernel/fs/owner_internal.h"
#include "../../../src/kernel/fs/owner_vfs.h"
#include "../../../src/kernel/fs/owner_channel.h"
#include "../../../src/kernel/fs/owner_system.h"
#include "../../../src/kernel/fs/security_policy.h"
#include "accounts.h"
#include "kernel_adapter.h"
#include "kernel_sys.h"
#include "production.h"
#include "server.h"

#define U_ARGS (CIUKI_IMAGE_BASE + PAGE_SIZE + 0x100)
#define U_OUT (U_ARGS + 0x100)
#define U_IN (U_ARGS + 0x200)
#define U_MSG (U_ARGS + 0x300)

/* ---- kernel_sys.h bound to the harness: the service acts as ACCOUNT ---- */
static struct proc_thread *account_thread;
static unsigned entropy_counter, owner_polls, entropy_unready;
static struct process *account_process(void) { desktop_select(account_thread); return account_thread->process; }
int acct_sys_security(struct ciuki_security_args *a, const void *in, uint32_t in_bytes, void *out, uint32_t out_bytes)
{
    struct process *p=account_process(); struct ciuki_security_args copy=*a;
    if (in) { CHECK(!ua_write(p->memory,U_IN,in,in_bytes)); copy.spawn_address=U_IN; }
    if (out) { copy.output_address=U_OUT; copy.output_bytes=out_bytes; }
    CHECK(!ua_write(p->memory,U_ARGS,&copy,64));
    struct trap_frame tf={.eax=101,.ebx=U_ARGS,.ecx=64};
    int e=proc_syscall(&tf);
    if (e) return e;
    CHECK(!ua_read(p->memory,&copy,U_ARGS,64));
    copy.spawn_address=a->spawn_address; copy.output_address=a->output_address; copy.output_bytes=a->output_bytes;
    *a=copy;
    if (out) CHECK(!ua_read(p->memory,out,U_OUT,out_bytes));
    return 0;
}
int acct_sys_channel_send(int fd, const struct ciuki_message *m)
{
    struct process *p=account_process();
    CHECK(!ua_write(p->memory,U_MSG,m,sizeof(*m)));
    return channel_send(p,fd,U_MSG,DONTWAIT);
}
int acct_sys_channel_recv(int fd, struct ciuki_message *m, int wait)
{
    (void)wait; struct process *p=account_process();
    int r=channel_recv(p,fd,U_MSG,DONTWAIT);
    if (r==1) CHECK(!ua_read(p->memory,m,U_MSG,sizeof(*m)));
    return r;
}
int acct_sys_close(int fd) { return desktop_close(account_process(),fd); }
/* While ACCOUNT waits, PID 1 runs its maintenance turn: one OWNER exchange. */
int acct_sys_wait_readable(const int *fds, unsigned count, unsigned timeout_ms)
{
    (void)fds; (void)timeout_ms;
    owner_service_poll(); owner_polls++; g_ticks+=1;
    return (int)count;
}
uint32_t acct_sys_pid(void) { return account_thread->process->pid; }
int acct_sys_pids(uint32_t *pids, unsigned capacity)
{
    unsigned n=0;
    for (unsigned i=1;i<CIUKI_PROCESS_MAX && n<capacity;i++) {
        struct process *p=proc_at_slot(i);
        if (p && p->state==PROC_LIVE) pids[n++]=p->pid;
    }
    return (int)n;
}
uint64_t acct_sys_monotonic_ms(void) { return g_ticks; }
int acct_sys_entropy(void *buffer, size_t bytes)
{ if (entropy_unready) { entropy_unready--; return -EAGAIN; }
  uint8_t *b=buffer; for (size_t i=0;i<bytes;i++) b[i]=(uint8_t)(++entropy_counter*7u+1u); return 0; }
int acct_sys_boot_flags(uint32_t *flags) { *flags=0; return 0; }
int acct_sys_file_present(const char *path) { (void)path; return 0; }
void acct_sys_log(const char *line) { printf("  [service] %s\n",line); }

/* ---- native file bindings of the service over the mounted volume ---- */
static struct vfs *volume_vfs;
static struct vfs_table service_table;
static int native(const char *path, char out[FS_PATH_BYTES])
{
    size_t n=strlen(path);
    if (path[0]!='/' || n+3>FS_PATH_BYTES) return -EINVAL;
    out[0]='C'; out[1]=':'; memcpy(out+2,path,n+1); return 0;
}
static int make_parents(const char *path)
{
    char p[FS_PATH_BYTES]; memcpy(p,path,strlen(path)+1);
    for (char *s=p+3;(s=strchr(s,'/'));s++) { *s=0; int e=vfs_mkdir(&service_table,p,0); *s='/'; if (e && e!=-EEXIST) return e; }
    return 0;
}
static int t_stat(void *ctx, const char *path, int *directory, uint64_t *size)
{
    (void)ctx; char p[FS_PATH_BYTES]; int e=native(path,p); if (e) return e;
    struct fat_entry entry; e=vfs_stat(&service_table,p,&entry);
    if (e==-FS_ENOTDIR) e=-ENOENT;
    if (!e) { *directory=!!(entry.attr&FAT_ATTR_DIR); *size=*directory?0:entry.size; }
    return e;
}
static int t_mkdir(void *ctx, const char *path)
{ (void)ctx; char p[FS_PATH_BYTES]; int e=native(path,p); return e?e:vfs_mkdir(&service_table,p,0); }
static int t_list(void *ctx, const char *path, int (*each)(void *, const struct acct_tree_entry *), void *arg)
{
    (void)ctx; char p[FS_PATH_BYTES]; int e=native(path,p); if (e) return e;
    size_t n=strlen(p); if (n+3>=sizeof(p)) return -ENAMETOOLONG;
    memcpy(p+n,"/*",3);
    struct vfs_find find; struct fat_entry entry;
    int found=vfs_find_first(&service_table,p,FAT_ATTR_DIR|FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM,&find,&entry);
    while (!found) {
        if (strcmp(entry.name,".") && strcmp(entry.name,"..")) {
            struct acct_tree_entry out; memset(&out,0,sizeof(out));
            if (strlen(entry.name)>=sizeof(out.name)) return -ENAMETOOLONG;
            strcpy(out.name,entry.name); out.directory=!!(entry.attr&FAT_ATTR_DIR); out.size=out.directory?0:entry.size;
            int r=each(arg,&out); if (r) return r;
        }
        found=vfs_find_next(&service_table,&find,&entry);
    }
    return found==-ENOENT?0:found;
}
static int t_read(void *ctx, const char *path, uint64_t offset, void *buffer, size_t bytes, size_t *done)
{
    (void)ctx; *done=0; char p[FS_PATH_BYTES]; int e=native(path,p); if (e) return e;
    int fd=vfs_open(&service_table,p,VFS_READ,VFS_DENY_NONE,0); if (fd<0) return fd;
    uint64_t at=0; e=vfs_seek(&service_table,fd,(int64_t)offset,VFS_SEEK_SET,&at);
    while (!e && *done<bytes) { size_t n=0; e=vfs_read(&service_table,fd,(uint8_t *)buffer+*done,bytes-*done,&n); if (!n) break; *done+=n; }
    int c=vfs_close(&service_table,fd); return e?e:c;
}
static int t_write(void *ctx, const char *path, uint64_t offset, const void *data, size_t bytes)
{
    (void)ctx; char p[FS_PATH_BYTES]; int e=native(path,p); if (e) return e;
    int fd=vfs_open(&service_table,p,offset?VFS_WRITE:VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_ALL,0); if (fd<0) return fd;
    uint64_t at=0; e=offset?vfs_seek(&service_table,fd,(int64_t)offset,VFS_SEEK_SET,&at):0;
    size_t n=0; if (!e && bytes) e=vfs_write(&service_table,fd,data,bytes,&n);
    if (!e && n!=bytes) e=-EIO;
    int c=vfs_close(&service_table,fd); return e?e:c;
}
static int t_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx; char a[FS_PATH_BYTES], b[FS_PATH_BYTES]; int e=native(from,a); if (!e) e=native(to,b); if (e) return e;
    struct fat_entry entry; if (!vfs_stat(&service_table,b,&entry)) return -EEXIST;
    e=vfs_rename(&service_table,a,b);
    /* The image's native rename (posixpath: fat_rename_replace) reserves a
     * new directory slot; the legacy vfs_rename renames in place and cannot
     * lengthen a name without free slots before it. Same outcome for a file. */
    if (e!=-FS_ENOSPC || vfs_stat(&service_table,a,&entry) || (entry.attr&FAT_ATTR_DIR)) return e;
    uint8_t *data=malloc(entry.size?entry.size:1); size_t got=0; CHECK(data);
    e=t_read(ctx,from,0,data,entry.size,&got);
    if (!e && got!=entry.size) e=-EIO;
    if (!e) e=t_write(ctx,to,0,data,got);
    if (!e) e=vfs_delete(&service_table,a);
    free(data); return e;
}
static int t_remove(void *ctx, const char *path)
{ (void)ctx; char p[FS_PATH_BYTES]; int e=native(path,p); return e?e:vfs_delete(&service_table,p); }
static int t_rmdir(void *ctx, const char *path)
{ (void)ctx; char p[FS_PATH_BYTES]; int e=native(path,p); return e?e:vfs_rmdir(&service_table,p); }
static int t_sync(void *ctx, const char *path) { (void)ctx; (void)path; return vfs_flush_drive(&service_table,2); }
static const struct acct_tree_io service_tree={ NULL, t_stat, t_mkdir, t_list, t_read, t_write, t_rename, t_remove, t_rmdir, t_sync };

/* acct_io: plain names below /System/Security or absolute staging names. */
static int io_path(const char *name, char out[FS_PATH_BYTES])
{
    if (name[0]=='/') return strncmp(name,"/System/Security/",17)?-EINVAL:native(name,out);
    if (strchr(name,'/')) return -EINVAL;
    char full[FS_PATH_BYTES]; int n=ksnprintf(full,sizeof(full),"/System/Security/%s",name);
    return n<0 || n>=(int)sizeof(full)?-ENAMETOOLONG:native(full,out);
}
static int io_read(void *ctx, const char *name, void *buffer, size_t capacity, size_t *bytes)
{
    (void)ctx; char p[FS_PATH_BYTES]; int e=io_path(name,p); if (e) return e;
    struct fat_entry entry; e=vfs_stat(&service_table,p,&entry); if (e==-FS_ENOTDIR) e=-ENOENT; if (e) return e;
    if (entry.size>capacity) return -EIO;
    size_t done=0; e=t_read(ctx,p+2,0,buffer,entry.size,&done);
    if (!e && done!=entry.size) e=-EIO;
    if (!e) *bytes=done;
    return e;
}
static int fail_manifest, fail_txn; /* one injected write failure each */
static int io_write(void *ctx, const char *name, const void *data, size_t bytes)
{
    (void)ctx; char p[FS_PATH_BYTES]; int e=io_path(name,p); if (e) return e;
    if (fail_manifest && strstr(name,"/manifest")) { fail_manifest=0; return -EIO; }
    if (fail_txn && !strcmp(name,"accounts.db.txn")) { fail_txn=0; return -EIO; }
    if (name[0]=='/' && (e=make_parents(p))) return e;
    return t_write(ctx,p+2,0,data,bytes);
}
static int io_flush(void *ctx, const char *name)
{
    (void)ctx; char p[FS_PATH_BYTES]; int e=io_path(name,p); if (e) return e;
    int fd=vfs_open(&service_table,p,VFS_READ,VFS_DENY_NONE,0); if (fd<0) return fd;
    e=vfs_commit(&service_table,fd); int c=vfs_close(&service_table,fd); return e?e:c;
}
static int io_barrier(void *ctx) { (void)ctx; return vfs_flush_drive(&service_table,2); }
static int io_rename(void *ctx, const char *from, const char *to)
{
    char a[FS_PATH_BYTES], b[FS_PATH_BYTES]; int e=io_path(from,a); if (!e) e=io_path(to,b); if (e) return e;
    return t_rename(ctx,a+2,b+2);
}
static int io_remove(void *ctx, const char *name)
{ (void)ctx; char p[FS_PATH_BYTES]; int e=io_path(name,p); return e?e:vfs_delete(&service_table,p); }

/* ---- fixture ---- */
static struct fake *sys_disk; static struct block_cache *sys_cache; static struct fat_volume *system_volume; static uint64_t volume_id;
static void mount_image(const char *image)
{
    sys_disk=calloc(1,sizeof(*sys_disk)); sys_cache=calloc(1,sizeof(*sys_cache)); volume_vfs=calloc(1,sizeof(*volume_vfs)); system_volume=calloc(1,sizeof(*system_volume));
    CHECK(sys_disk && sys_cache && volume_vfs && system_volume);
    CHECK(!fake_open(sys_disk,image)); CHECK(!cache_init(sys_cache,0)); vfs_init(volume_vfs);
    CHECK(!fat_mount(system_volume,sys_cache,&sys_disk->dev,0,sys_disk->dev.capacity,true)); CHECK(!vfs_attach(volume_vfs,2,system_volume));
    CHECK(!vfs_table_init(volume_vfs,&service_table,2));
    /* scripts/build_image.py volume_identity: partition LBA 2048 << 32 | BPB serial. */
    volume_id=(uint64_t)2048<<32|system_volume->volume_serial; CHECK(system_volume->volume_serial);
}
static void unmount_image(void)
{
    vfs_table_destroy(&service_table); CHECK(!vfs_detach(volume_vfs,2)); vfs_destroy(volume_vfs); cache_destroy(sys_cache); fake_close(sys_disk);
    free(system_volume); free(volume_vfs); free(sys_cache); free(sys_disk);
}
static struct proc_thread *role_fixture(uint32_t role, struct process **out)
{
    CHECK(!proc_prepare(proc_supervisor(),out));
    CHECK(!security_role_prepare(proc_supervisor(),*out,role,security_role_image(role)));
    struct ciuki_file f=fixture_file(); struct proc_strings *s=proc_strings_new();
    CHECK(s && !proc_strings_add(s,"accounts",9,false)); CHECK(!prepare_image(*out,&f,s,0)); proc_strings_free(s);
    proc_publish(*out,0);
    for (unsigned i=0;i<CIUKI_THREAD_MAX;i++) if (proc_thread_slot(i) && proc_thread_slot(i)->process==*out) return proc_thread_slot(i);
    CHECK(false); return 0;
}
static struct acct_server service_server;
static struct acct_owner_link owner_link;
static struct acct_store service_tables(void)
{
    struct acct_store tables; memset(&tables,0,sizeof(tables));
    CHECK(!acct_owner_kernel(&owner_link,&service_tree,&tables.owner));
    uint64_t v=0; uint32_t open=9; int identity=acct_owner_kernel_identity(&owner_link,&v,&open);
    if (identity) printf("identity=%d\n",identity);
    CHECK(!identity); CHECK(v==volume_id);
    tables.volume=v;
    tables.io=(struct acct_io){ .ctx=NULL, .real=1, .read=io_read, .write=io_write, .flush=io_flush, .barrier=io_barrier,
        .rename=io_rename, .remove=io_remove };
    return tables;
}
static bool exists_at(const char *path) { int d=0; uint64_t s=0; return !t_stat(0,path,&d,&s); }
static struct security_subject admin1000={.credential={.version=1,.size=64,.uid=1000,.primary_gid=545,.group_count=2,.groups={544,545},.flags=1},.live=true};
static struct security_subject standard1001={.credential={.version=1,.size=64,.uid=1001,.primary_gid=545,.group_count=1,.groups={545},.flags=1},.live=true};
static struct security_subject guest501={.credential={.version=1,.size=64,.uid=501,.primary_gid=546,.group_count=1,.groups={546},.flags=1},.live=true};
static struct security_subject system0={.credential={.version=1,.size=64,.uid=0,.group_count=1,.groups={0}},.live=true};
/* Labels of the bound policy along a path of the system volume. */
static int label_of(const char *const *names, unsigned count, struct policy_label *out)
{
    struct policy_volume *p=volume_vfs->policy[2]; CHECK(p && p->owners);
    struct policy_label l; int e=policy_root(p,1,&l); if (e) return e;
    for (unsigned i=0;i<count;i++) {
        uint64_t locator=0;
        if (l.kind==POLICY_PROFILES || l.kind==POLICY_ROOT) {
            const struct owner_store *s=p->owners; unsigned total=l.kind==POLICY_PROFILES?s->profile_count:s->root_count;
            for (unsigned j=0;j<total;j++) {
                const struct owner_record *r=l.kind==POLICY_PROFILES?&s->profiles[j]:&s->roots[j];
                const char *leaf=strrchr(r->binding.path,'/')+1;
                if (path_equal(leaf,names[i])) locator=r->binding.locator;
            }
        }
        e=policy_child(p,&l,names[i],i+2,locator,&l); if (e) return e;
    }
    *out=l; return 0;
}

static void test_first_boot(void)
{
    /* Boot 1: the image's ready seed, no SID, no store: unmanaged, gated. */
    CHECK(!owner_system_attach(volume_vfs,2,volume_id,false));
    CHECK(owner_system_state()==OWNER_SYSTEM_UNPROVISIONED && !volume_vfs->policy[2] && owner_system_volume()==volume_id);
    struct ciuki_security_profile profile; CHECK(owner_system_profile(1000,&profile)==-ENOENT);
    struct acct_store tables=service_tables();
    /* The gate: a RESERVE without the installation's SID evidence never
     * creates a store; a wrong volume is refused before anything else. */
    struct acct_owner_request q; memset(&q,0,sizeof(q)); q.sid[0]=7; q.sid[1]=8; q.sid[2]=9; q.count=1; q.volume=volume_id; q.transaction=1;
    memset(q.target_digest,1,32); memset(q.manifest_digest,2,32);
    q.bindings[0]=(struct acct_owner_binding){.uid=1000,.action=ACCT_OWNER_CREATE,.volume=volume_id}; strcpy(q.bindings[0].root,"/Windows/Profiles/u000003e8");
    uint8_t wire[ACCT_OWNER_BODY_MAX], reply[ACCT_OWNER_REPLY_MAX]; size_t n=0, got=0;
    CHECK(!acct_owner_encode(&q,ACCT_OWNER_RESERVE,wire,sizeof(wire),&n));
    CHECK(tables.owner.call(tables.owner.ctx,wire,n,reply,sizeof(reply),&got)==-EIO);
    CHECK(owner_system_state()==OWNER_SYSTEM_UNPROVISIONED && !exists_at("/System/Security/Owner/commit.1"));
    CHECK(tables.owner.root_occupied(tables.owner.ctx,volume_id+1,"/GAME")==-EACCES);
    uint64_t g=9; CHECK(!tables.owner.generation(tables.owner.ctx,&g) && !g);
    CHECK(!acct_owner_encode(&q,ACCT_OWNER_STATUS,wire,sizeof(wire),&n));
    CHECK(!tables.owner.call(tables.owner.ctx,wire,n,reply,sizeof(reply),&got) && got==96 && fs_rd32(reply+8)==OWNER_UNKNOWN);
    puts("H15 sec-own first boot: unprovisioned volume unmanaged, gate refuses a RESERVE without installation evidence, queries answer before any store PASS");

    /* An interrupted first batch: the installation SID intent is durable,
     * the staging manifest write fails before any OWNER record. The next boot
     * keeps that SID, the volume stays unprovisioned and the same SID is
     * promoted (contract 3.6 "Before SID persistence completes"/resume). */
    fail_manifest=1;
    CHECK(!acct_server_init(&service_server,&tables,0)); service_server.tree=&service_tree;
    CHECK(acct_server_open(&service_server,0)==1 && !service_server.service.available && service_server.firstboot_result==-EIO);
    CHECK(!fail_manifest && exists_at("/System/Security/machine.sid") && exists_at("/System/Security/bootstrap.sid"));
    CHECK(!exists_at("/System/Security/Owner/commit.1") && owner_system_state()==OWNER_SYSTEM_UNPROVISIONED && !volume_vfs->policy[2]);
    uint8_t sid_before[64], sid_after[64]; size_t sid_bytes=0;
    CHECK(!t_read(0,"/System/Security/machine.sid",0,sid_before,64,&sid_bytes) && sid_bytes==64);
    acct_server_close(&service_server);
    owner_system_detach(); CHECK(!owner_system_attach(volume_vfs,2,volume_id,false) && owner_system_state()==OWNER_SYSTEM_UNPROVISIONED);
    tables=service_tables();
    puts("H15 sec-own interrupted first batch: SID intent kept, no OWNER record, volume still unprovisioned at the next boot PASS");

    /* The production server: lease, open, first-boot promotion, migration. */
    CHECK(!acct_server_init(&service_server,&tables,0)); service_server.tree=&service_tree;
    unsigned polls=owner_polls;
    /* A slow first boot opens ACCOUNT before the kernel generator is seeded:
     * getentropy EAGAIN closes the service and the next open resumes. */
    entropy_unready=1;
    CHECK(!acct_server_open(&service_server,0) && service_server.firstboot_result==-EAGAIN && !entropy_unready);
    CHECK(!service_server.opened && owner_system_state()==OWNER_SYSTEM_UNPROVISIONED);
    CHECK(!acct_server_open(&service_server,ACCT_SERVER_FIRSTBOOT_RETRY_MS-1));
    CHECK(acct_server_open(&service_server,ACCT_SERVER_FIRSTBOOT_RETRY_MS)==1 && service_server.service.available);
    CHECK(service_server.firstboot_result==ACCT_FIRSTBOOT_PROMOTED);
    printf("  owner exchanges for the first boot: %u\n",owner_polls-polls);
    CHECK(owner_system_state()==OWNER_SYSTEM_OPEN && volume_vfs->policy[2] && volume_vfs->policy[2]->lease);
    const struct acct_db *d=&service_server.service.db;
    CHECK(d->count==3 && d->origin==ACCT_READY && d->auto_logon_uid==1000 && d->generation==1);
    CHECK(d->records[0].uid==500 && d->records[0].flags==ACCT_BUILTIN && !strcmp(d->records[0].name,"CiukiAdministrator"));
    CHECK(d->records[1].uid==501 && d->records[1].flags==ACCT_BUILTIN && !strcmp(d->records[1].name,"Guest"));
    CHECK(d->records[2].uid==1000 && d->records[2].flags==(ACCT_ENABLED|ACCT_PASSWORD_SET|ACCT_READY_BLANK) &&
          d->records[2].groups==ACCT_ADMIN && !strcmp(d->records[2].name,"CiukiUser"));
    CHECK(acct_verify(&d->records[2],"") && !acct_verify(&d->records[2],"x"));
    CHECK(!t_read(0,"/System/Security/machine.sid",0,sid_after,64,&sid_bytes) && sid_bytes==64 && !memcmp(sid_before,sid_after,64));
    struct acct_bootstrap b; CHECK(!acct_boot_read(&service_server.service.store,&b) && b.state==ACCT_CONSUMED && b.volume==volume_id);
    CHECK(!exists_at("/System/Security/provision.seed") && exists_at("/System/Security/accounts.db") && exists_at("/System/Security/machine.sid"));
    /* OWNER: three committed private roots, published from staging. */
    struct owner_store *s=volume_vfs->policy[2]->owners;
    CHECK(s->ready && s->profile_count==3 && s->generation && !s->active && s->sid[0]==d->sid[0] && s->receipt_count==1);
    static const char *const roots[3]={"/Windows/Profiles/u000001f4","/Windows/Profiles/u000001f5","/Windows/Profiles/u000003e8"};
    for (unsigned i=0;i<3;i++) {
        char path[96]; CHECK(!strcmp(s->profiles[i].binding.path,roots[i]));
        ksnprintf(path,sizeof(path),"%s/Documents/Desktop",roots[i]); CHECK(exists_at(path));
        ksnprintf(path,sizeof(path),"%s/Settings",roots[i]); CHECK(exists_at(path));
        ksnprintf(path,sizeof(path),"%s/Start Menu/Programs/StartUp",roots[i]); CHECK(exists_at(path));
    }
    char staged[96]; ksnprintf(staged,sizeof(staged),"/System/Security/Staging/g%016llx/p000003e8",(unsigned long long)d->generation);
    CHECK(!exists_at(staged) && !exists_at("/System/Security/accounts.db.txn"));
    /* ACCOUNT reads its bindings through OWNER's query operations. */
    char root[64]; struct acct_owner_binding binding;
    CHECK(!tables.owner.binding(tables.owner.ctx,volume_id,1000,root,&binding) && !strcmp(root,roots[2]) && binding.key && binding.generation);
    CHECK(tables.owner.binding(tables.owner.ctx,volume_id,1001,root,&binding)==-ENOENT);
    CHECK(tables.owner.root_occupied(tables.owner.ctx,volume_id,roots[2])==1 && !tables.owner.root_occupied(tables.owner.ctx,volume_id,"/Windows/Profiles/u000003e9"));
    uint64_t floor=9; unsigned receipts=0; CHECK(!tables.owner.checkpoint_floor(tables.owner.ctx,&floor,&receipts) && !floor && receipts==1);
    /* One receipt leaves no checkpoint candidate: SNAPSHOT says so, writes nothing. */
    uint8_t *image=malloc(ACCT_CHECKPOINT_MAX); size_t image_bytes=0; uint64_t next_floor=0; CHECK(image);
    CHECK(tables.owner.snapshot(tables.owner.ctx,d,service_server.service.store.high_generation,service_server.service.store.high_uid,
                                image,ACCT_CHECKPOINT_MAX,&image_bytes,&next_floor)==-EAGAIN && !exists_at("/System/Security/Owner/snapshot"));
    /* Transport of a candidate image through the SYSTEM-only snapshot file
     * (in-memory floor only; the durable store keeps floor 0). */
    uint64_t saved_floor=s->floor; s->floor=d->generation;
    CHECK(!tables.owner.snapshot(tables.owner.ctx,d,service_server.service.store.high_generation,service_server.service.store.high_uid,
                                 image,ACCT_CHECKPOINT_MAX,&image_bytes,&next_floor));
    CHECK(next_floor==d->generation && image_bytes==160+112*3+8+(160+128*3)+(96+32*3) && !memcmp(image,"CIUKIOCP",8) &&
          fs_rd32(image+8)==1 && fs_rd32(image+16)==3 && fs_rd32(image+20)==1 && exists_at("/System/Security/Owner/snapshot"));
    s->floor=saved_floor;
    free(image);
    CHECK(!tables.owner.recover_orphans(tables.owner.ctx,volume_id,0) && s->receipt_count==1);
    /* The interim session's profile is the committed descriptor. */
    CHECK(!owner_system_profile(1000,&profile) && !strcmp(profile.path,roots[2]) && profile.path_bytes==strlen(roots[2])+1);
    CHECK(!security_owner_verify(&profile));
    /* DOS saves of the machine store moved into the first administrator's profile. */
    CHECK(exists_at("/Windows/Profiles/u000003e8/Settings/DOS/G1K1LEGACY.COW") && !exists_at("/System/DOS/SAVES/G1K1LEGACY.COW"));
    int dir=0; uint64_t size=0; CHECK(!t_stat(0,"/Windows/Profiles/u000003e8/Settings/DOS/G1K1LEGACY.COW",&dir,&size) && !dir && size==4096);
    puts("H15 sec-own first boot: ready seed promoted through the kernel OWNER (CiukiUser blank admin, 500/501 disabled), three profiles published from staging, seed consumed, DOS saves migrated PASS");

    /* Bound policy: contract rule table on the managed system volume. */
    struct policy_volume *p=volume_vfs->policy[2]; struct policy_label l;
    CHECK(!label_of((const char *[]){"System","Security"},2,&l) && l.kind==POLICY_SECRET);
    CHECK(policy_check(p,&l,&admin1000,POLICY_READ)==-EACCES && !policy_check(p,&l,&system0,POLICY_READ|POLICY_WRITE));
    CHECK(!label_of((const char *[]){"Windows","Profiles","u000003e8"},3,&l) && l.kind==POLICY_PROFILE && l.uid==1000);
    CHECK(!policy_check(p,&l,&admin1000,POLICY_READ|POLICY_WRITE) && policy_check(p,&l,&standard1001,POLICY_READ)==-EACCES &&
          policy_check(p,&l,&guest501,POLICY_READ)==-EACCES);
    CHECK(!label_of((const char *[]){"Windows","Profiles","u000001f5"},3,&l) && l.kind==POLICY_PROFILE && l.uid==501);
    CHECK(!policy_check(p,&l,&guest501,POLICY_READ) && policy_check(p,&l,&standard1001,POLICY_READ)==-EACCES);
    CHECK(!label_of((const char *[]){"Games"},1,&l) && l.kind==POLICY_PUBLIC);
    CHECK(!policy_check(p,&l,&admin1000,POLICY_WRITE|POLICY_ADD) && policy_check(p,&l,&standard1001,POLICY_WRITE)==-EACCES &&
          !policy_check(p,&l,&guest501,POLICY_READ));
    CHECK(!label_of((const char *[]){"Documents"},1,&l) && l.kind==POLICY_SHARED && !policy_check(p,&l,&standard1001,POLICY_WRITE));
    CHECK(!label_of((const char *[]){"Windows","Profiles","stray"},3,&l) && l.kind==POLICY_DENY);
    /* A transaction in flight does not make committed labels unavailable. */
    s->busy=true; CHECK(!label_of((const char *[]){"Games"},1,&l) && !policy_check(p,&l,&admin1000,POLICY_READ)); s->busy=false;
    CHECK(!owner_vfs_reserved(p->lease,"/Windows/Profiles/u000003e8"));
    puts("H15 sec-own policy: /System/Security SYSTEM-only, private profiles (owner/admin), Guest's own profile, public root entries, shared /Documents, unmapped profile names denied, busy store keeps committed labels PASS");

    /* A later ownership batch (sec-user-04's account creation): CREATE plus
     * KEEP of the three committed bindings, empty KEEP staging, prune of the
     * finished first generation, one more receipt and owner generation. */
    struct acct_store *st=&service_server.service.store;
    struct acct_db next=*d; next.generation=st->high_generation+1; next.next_uid=1002; next.count=4;
    next.records[3]=(struct acct_record){.uid=1001,.groups=ACCT_STANDARD}; strcpy(next.records[3].name,"Alice");
    char first[96]; ksnprintf(first,sizeof(first),"/System/Security/Staging/g%016llx",(unsigned long long)d->generation);
    CHECK(exists_at(first));
    uint64_t owner_generation=s->generation;
    int committed=acct_store_commit(st,d,&next,NULL,0); if (committed) printf("second batch=%d\n",committed);
    CHECK(!committed);
    service_server.service.db=next; d=&service_server.service.db;
    CHECK(s->profile_count==4 && s->receipt_count==2 && s->generation>owner_generation && !s->active && !exists_at(first));
    CHECK(!strcmp(s->profiles[3].binding.path,"/Windows/Profiles/u000003e9") && exists_at("/Windows/Profiles/u000003e9/Documents"));
    for (unsigned i=0;i<3;i++) CHECK(s->profiles[i].binding.generation==owner_generation);
    CHECK(!label_of((const char *[]){"Windows","Profiles","u000003e9"},3,&l) && l.kind==POLICY_PROFILE && l.uid==1001);
    CHECK(!policy_check(p,&l,&standard1001,POLICY_WRITE) && !policy_check(p,&l,&admin1000,POLICY_READ) && policy_check(p,&l,&guest501,POLICY_READ)==-EACCES);
    CHECK(!tables.owner.binding(tables.owner.ctx,volume_id,1001,root,&binding) && !strcmp(root,"/Windows/Profiles/u000003e9"));
    puts("H15 sec-own second batch: CREATE beside KEEP of committed bindings through the kernel OWNER, finished staging pruned, new private profile labelled PASS");
    acct_server_close(&service_server);
}
static void test_reboot(void)
{
    owner_system_detach(); CHECK(!volume_vfs->policy[2] && owner_system_state()==OWNER_SYSTEM_NONE);
    /* Boot 2: machine.sid opens the same store; policy and lease bound at boot. */
    CHECK(!owner_system_attach(volume_vfs,2,volume_id,false));
    CHECK(owner_system_state()==OWNER_SYSTEM_OPEN && volume_vfs->policy[2] && volume_vfs->policy[2]->owners->profile_count==4);
    struct ciuki_security_profile profile; CHECK(!owner_system_profile(1000,&profile) && !security_owner_verify(&profile));
    struct acct_store tables=service_tables();
    CHECK(!acct_server_init(&service_server,&tables,0)); service_server.tree=&service_tree;
    CHECK(acct_server_open(&service_server,0)==1 && service_server.service.available && service_server.firstboot_result==ACCT_FIRSTBOOT_NOTHING);
    CHECK(service_server.service.db.count==4 && service_server.service.db.records[2].uid==1000 && service_server.service.db.records[3].uid==1001);
    /* A seed restored after consumption is never replayed. */
    acct_server_close(&service_server);
    puts("H15 sec-own reboot: store reopened from the installation SID before any service, policy bound at boot, service opens without promotion PASS");
    owner_system_detach();
    /* Boot 3: an installation SID without its store is contradictory: no
     * store opens, the volume stays unmanaged, the service stays closed. */
    CHECK(!vfs_rename(&service_table,"C:/System/Security/Owner","C:/System/Security/Owner.moved"));
    CHECK(owner_system_attach(volume_vfs,2,volume_id,false)==-EIO && owner_system_state()==OWNER_SYSTEM_FAILED && !volume_vfs->policy[2]);
    tables=service_tables();
    CHECK(!acct_server_init(&service_server,&tables,0)); service_server.tree=&service_tree;
    CHECK(!acct_server_open(&service_server,0) && service_server.last_open_error<0);
    acct_server_close(&service_server);
    owner_system_detach();
    CHECK(!vfs_rename(&service_table,"C:/System/Security/Owner.moved","C:/System/Security/Owner"));
    CHECK(!owner_system_attach(volume_vfs,2,volume_id,false) && owner_system_state()==OWNER_SYSTEM_OPEN);
    owner_system_detach();
    puts("H15 sec-own contradiction: SID without OWNER store fails closed (volume unmanaged, service closed), restored store reopens PASS");
}
/* A first boot cut after OWNER's durable RESERVE and before ACCOUNT's
 * intent (second volume with the same ready provisioning). The next boot
 * opens the store and OWNER aborts the orphan from ACCOUNT's evidence (no
 * COMMIT_REQUIRED, contract 3.5). sec-sysvc (lead decision): because every
 * receipt was aborted before any batch committed, the recipe's uid 1000 may
 * be reserved again, so the installation opens at the next generation; the
 * aborted generation and transaction stay retired. A uid that was never
 * reserved, or any uid once a batch has committed, stays retired (H02). */
static void test_reserve_cut(void)
{
    CHECK(!owner_system_attach(volume_vfs,2,volume_id,false) && owner_system_state()==OWNER_SYSTEM_UNPROVISIONED);
    struct acct_store tables=service_tables();
    fail_txn=1;
    CHECK(!acct_server_init(&service_server,&tables,0)); service_server.tree=&service_tree;
    CHECK(acct_server_open(&service_server,0)==1 && !service_server.service.available && service_server.firstboot_result==-EIO && !fail_txn);
    CHECK(owner_system_state()==OWNER_SYSTEM_OPEN && volume_vfs->policy[2] && volume_vfs->policy[2]->owners->active);
    acct_server_close(&service_server);
    owner_system_detach(); CHECK(!owner_system_attach(volume_vfs,2,volume_id,false) && owner_system_state()==OWNER_SYSTEM_OPEN);
    const struct owner_store *o=volume_vfs->policy[2]->owners;
    CHECK(!o->active && !o->profile_count && o->receipt_count==1 && !o->generation && o->transaction_high==1 && o->uid_high==1001);
    CHECK(o->receipts[0]->state==OWNER_ABORTED);
    tables=service_tables();
    CHECK(!acct_server_init(&service_server,&tables,0)); service_server.tree=&service_tree;
    int opened=acct_server_open(&service_server,0);
    if (opened!=1 || service_server.firstboot_result!=ACCT_FIRSTBOOT_PROMOTED)
        printf("  resume after the cut: open=%d available=%d firstboot=%d\n",opened,service_server.service.available,service_server.firstboot_result);
    CHECK(opened==1 && service_server.service.available && service_server.firstboot_result==ACCT_FIRSTBOOT_PROMOTED);
    const struct acct_db *d=&service_server.service.db;
    CHECK(d->count==3 && d->generation==2 && d->auto_logon_uid==1000 && d->records[2].uid==1000 && !strcmp(d->records[2].name,"CiukiUser"));
    struct acct_bootstrap b; CHECK(!acct_boot_read(&service_server.service.store,&b) && b.state==ACCT_CONSUMED && b.accounts_generation==2);
    CHECK(o->profile_count==3 && o->receipt_count==2 && o->receipts[0]->state==OWNER_ABORTED && o->receipts[1]->state==OWNER_COMMITTED);
    CHECK(o->generation && o->transaction_high==2 && o->uid_high==1001 && !o->active);
    CHECK(o->profiles[2].binding.uid==1000 && !strcmp(o->profiles[2].binding.path,"/Windows/Profiles/u000003e8"));
    CHECK(exists_at("/System/Security/accounts.db") && exists_at("/Windows/Profiles/u000003e8/Settings") && !exists_at("/System/Security/provision.seed"));
    acct_server_close(&service_server);
    /* The next boot reopens the published store; nothing is resumed again. */
    owner_system_detach(); CHECK(!owner_system_attach(volume_vfs,2,volume_id,false) && owner_system_state()==OWNER_SYSTEM_OPEN);
    o=volume_vfs->policy[2]->owners; CHECK(o->profile_count==3 && o->receipt_count==2);
    tables=service_tables();
    CHECK(!acct_server_init(&service_server,&tables,0)); service_server.tree=&service_tree;
    CHECK(acct_server_open(&service_server,0)==1 && service_server.service.available && service_server.firstboot_result==ACCT_FIRSTBOOT_NOTHING);
    CHECK(service_server.service.db.generation==2);
    acct_server_close(&service_server);
    owner_system_detach();
    puts("H15 sec-sysvc cut after OWNER RESERVE: orphan aborted at the next boot, uid 1000 (only aborted reservations, nothing ever committed) reserved again, ready recipe promoted at generation 2, reboot opens without resume PASS");
}
int main(int argc, char **argv)
{
    setvbuf(stdout,0,_IONBF,0);
    CHECK(argc==3);
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram); controller.state=T_RUNNING; g_current=&controller; proc_init();
    struct process *account; account_thread=role_fixture(CIUKI_SECURITY_ROLE_ACCOUNT,&account);
    account->actions[SIGCHLD].handler=SIG_IGN;
    mount_image(argv[1]);
    test_first_boot();
    test_reboot();
    CHECK(!vfs_flush_drive(&service_table,2));
    unmount_image();
    mount_image(argv[2]);
    test_reserve_cut();
    CHECK(!vfs_flush_drive(&service_table,2));
    unmount_image();
    g_current=&controller; stop_collect(account);
    for (unsigned i=0;i<CIUKI_OPEN_MAX;i++) if (proc_supervisor()->fds[i].object) CHECK(!desktop_close(proc_supervisor(),(int32_t)i));
    kfree(supervisor_task); supervisor_task=0;
    CHECK(!desktop_objects.channels && !desktop_objects.messages);
    CHECK(!pages_used && !heap_blocks && !fs_host_contig_live); free(ram); ram=0;
    printf("H15 sec-own system OWNER integration: PASS checks=%u owner_exchanges=%u\n",checks,owner_polls);
    return 0;
}
