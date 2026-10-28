/* H16 (sec-sysvc): the SYSTEM services of call 99 on the production kernel
 * path: native syscalls, fd table, POSIX namespace, legacy VFS tables, the
 * managed-volume policy and OWNER profile records on the real FAT/VFS. The
 * services (fs/sysvc.c, fs/sysvc_drivers.c) and their call-99 adapter
 * (proc/syscalls_sysvc.c) are production code; the only fakes are the disk,
 * the OWNER blobs and the NDIS device observations. Every service runs as an
 * administrator, a standard user and Guest; the direct file routes stay
 * refused by the policy (K-Q4) while the services work.
 * SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "../proc/files_test.c"
#include "../../../src/kernel/fs/owner_internal.h"
#include "../../../src/kernel/fs/sysvc.h"
#include "../../../src/kernel/ndis/package.h"
#include "../../../src/kernel/proc/syscalls_sysvc.c"
#include <stdarg.h>
/* The services' kernel log lines ([sysvc] ...) on stdout. */
void klog(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }

/* ---- OWNER storage: in-memory blobs (the H04 fixture) ------------------- */
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

/* ---- identities and sessions --------------------------------------------- */
struct actor { struct process *p; struct proc_thread *t; const char *name; uint32_t data; };
static struct actor alice, bob, admin, guest, interim, systemp;
#define ALICE_HOME "/Windows/Profiles/u000003ef"
#define BOB_HOME "/Windows/Profiles/u000003e9"
#define GUEST_HOME "/Windows/Profiles/u000001f5"
#define ADMIN_HOME "/Windows/Profiles/u000003e8"
#define ALICE_LOGS ALICE_HOME SYSVC_USER_LOGS
static void identity(struct actor *a,const char *name,uint32_t uid,unsigned count,uint32_t g0,uint32_t g1)
{
    a->t=make_process(proc_supervisor(),&a->p); a->name=name;
    /* 128 KiB of anonymous memory for service data (a manifest is 48 KiB). */
    struct ciuki_mmap_args mapping={.size=sizeof(mapping),.length=128*1024,.prot=PROT_READ|PROT_WRITE,.flags=MAP_PRIVATE|MAP_ANONYMOUS,.fd=-1};
    a->data=(uint32_t)ua_mmap(a->p->memory,&mapping); CHECK(a->data>=CIUKI_MMAP_BASE && a->data<CIUKI_MMAP_LIMIT);
    struct ciuki_credential *c=&a->p->subject->credential;
    if(uid) { c->uid=uid; c->primary_gid=g0==544?545:g0; c->group_count=count; c->groups[0]=g0; c->groups[1]=count>1?g1:0; c->flags=1; }
}
/* An active session of this credential, with (path) or without a profile:
 * the session slot the kernel's SESSION_CREATE/bootstrap would hold. */
static void session(struct actor *a,unsigned slot,const char *path)
{
    struct security_session *s=&security_sessions[slot]; struct security_subject *subject=a->p->subject;
    CHECK(!s->state);
    subject->credential.session_id=100+slot;
    s->credential=subject->credential; s->state=CIUKI_SECURITY_SESSION_ACTIVE; s->refs=1; s->generation=1;
    s->profile=(struct ciuki_security_profile){.version=1,.size=112,.uid=subject->credential.uid};
    if(path) { s->profile.path_bytes=(uint32_t)strlen(path)+1; memcpy(s->profile.path,path,strlen(path)); }
    subject->session=s;
}
static void as(struct actor *a) { g_current=a->t->task; g_current->state=T_RUNNING; }

/* ---- kernel-side file access (SYSTEM) ----------------------------------- */
static void kernel_file(const char *path,const void *data,size_t n)
{
    struct vfs_table t; CHECK(!vfs_table_init(&vfs,&t,2));
    int fd=vfs_open(&t,path,VFS_CREATE|VFS_WRITE|VFS_TRUNCATE,VFS_DENY_NONE,0); CHECK(fd>=0);
    size_t done; CHECK(!vfs_write(&t,fd,data,n,&done) && done==n); CHECK(!vfs_close(&t,fd));
    vfs_table_destroy(&t);
}
static int kernel_read(const char *path,void *out,size_t cap,size_t *n)
{
    struct vfs_table t; CHECK(!vfs_table_init(&vfs,&t,2)); *n=0;
    int fd=vfs_open(&t,path,VFS_READ,VFS_DENY_NONE,0);
    if(fd<0) { vfs_table_destroy(&t); return fd; }
    int e=vfs_read(&t,fd,out,cap,n); CHECK(!vfs_close(&t,fd)); vfs_table_destroy(&t); return e;
}
static bool kernel_exists(const char *path)
{
    struct vfs_table t; CHECK(!vfs_table_init(&vfs,&t,2)); struct fat_entry e;
    int r=vfs_stat(&t,path,&e); vfs_table_destroy(&t); return !r;
}

static struct owner_store *owners;
static struct policy_volume policy;
static void profile_record(unsigned index,uint32_t uid,const char *path)
{
    struct px_path r; vfs_lock(&vfs,0); CHECK(!px_resolve_locked(space,space->root,path,false,&r));
    struct owner_record *o=&owners->profiles[index]; memset(o,0,sizeof(*o));
    o->binding=(struct owner_binding){.uid=uid,.action=OWNER_CREATE,.volume=31,.key=100+index,.locator=100+index,.generation=1};
    memcpy(o->binding.path,path,strlen(path)+1); CHECK(!px_object_proof_locked(r.node,o->proof)); vfs_unlock(&vfs);
}
static void layout(void)
{
    const char *dirs[]={"/Windows","/Windows/Profiles",ALICE_HOME,BOB_HOME,GUEST_HOME,ADMIN_HOME,"/Program Files","/Program Files/pack",
        "/System","/System/Logs","/System/Security","/System/Security/pack","/Documents"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++) { int e=make_dir(dirs[i]); CHECK(!e || e==-EEXIST); }
    const char boot[]="[boot] CiukiOS line 1\n[boot] line 2\n";
    kernel_file("C:/System/Logs/BOOT.LOG",boot,sizeof(boot)-1);
    kernel_file("C:/System/BOOT.CFG","safe=0 serial=1\n",16);
}
static void bind_policy(void)
{
    owners=calloc(1,sizeof(*owners)); CHECK(owners);
    struct owner_backend io={0,o_read,o_write,o_flush,o_barrier,o_manifest,o_tree,o_reserve,o_publish,o_release,
        o_intent,o_current,o_marker,o_import,o_occupied,o_verify};
    CHECK(!owner_open(owners,&io,sid,31,true));
    owners->profile_count=4; profile_record(0,501,GUEST_HOME); profile_record(1,1000,ADMIN_HOME);
    profile_record(2,1001,BOB_HOME); profile_record(3,1007,ALICE_HOME); owners->sequence++;
    CHECK(!policy_init(&policy,31,1,true,false,false,owners));
    vfs_lock(&vfs,0); CHECK(!policy_bind_locked(&vfs,2,&policy)); vfs_unlock(&vfs);
}

/* ---- call 99 through the production adapter ----------------------------- */
#define REC (USER + 2048)
static int32_t service(struct actor *a,uint32_t op,void *record,uint32_t size)
{
    as(a); CHECK(!ua_write(a->p->memory,REC,record,size));
    struct trap_frame tf={.eax=CIUKI_SYS_RECOVERY,.ebx=op,.ecx=REC,.edx=size}; int32_t r=0;
    CHECK(sysvc_syscall(a->p,a->p->subject,&tf,&r));
    CHECK(!ua_read(a->p->memory,record,REC,size));
    return r;
}
static int32_t save_boot(struct actor *a,const char *text)
{
    struct ciuki_boot_config r={.version=1,.size=sizeof(r),.action=CIUKI_BOOT_CONFIG_SET,.length=(uint32_t)strlen(text)};
    memcpy(r.text,text,r.length); return service(a,CIUKI_RECOVERY_BOOT_CONFIG,&r,sizeof(r));
}
static int32_t log_call(struct actor *a,uint32_t action,uint32_t scope,const char *name,uint32_t index,uint32_t offset,
                        const void *data,uint32_t bytes,uint32_t limit,struct ciuki_system_log *out)
{
    struct ciuki_system_log r={.version=1,.size=sizeof(r),.action=action,.scope=scope,.index=index,.offset=offset,
        .buffer=bytes?a->data:0,.bytes=bytes,.limit=limit};
    if(name) snprintf(r.name,sizeof(r.name),"%s",name);
    as(a); if(data && bytes) CHECK(!ua_write(a->p->memory,a->data,data,bytes));
    int32_t e=service(a,CIUKI_RECOVERY_LOG,&r,sizeof(r));
    if(out) *out=r;
    return e;
}
static int32_t log_read(struct actor *a,uint32_t scope,const char *name,char *out,uint32_t cap,uint32_t *total)
{
    struct ciuki_system_log r; int32_t e=log_call(a,CIUKI_SYSTEM_LOG_READ,scope,name,0,0,0,cap,0,&r);
    if(!e) { CHECK(r.bytes<=cap); CHECK(!ua_read(a->p->memory,out,a->data,r.bytes)); out[r.bytes]=0; if(total) *total=r.total; }
    return e;
}
static int32_t log_append(struct actor *a,uint32_t scope,const char *name,const char *text,uint32_t limit)
{ return log_call(a,CIUKI_SYSTEM_LOG_APPEND,scope,name,0,0,text,(uint32_t)strlen(text),limit,0); }
static int32_t sys_open_as(struct actor *a,const char *path,uint32_t flags)
{ as(a); CHECK(!ua_write(a->p->memory,USER,path,(uint32_t)strlen(path)+1)); return call(CIUKI_SYS_OPEN,USER,flags,0666,0,0); }
static void sys_close_as(struct actor *a,int32_t fd) { as(a); CHECK(!call(CIUKI_SYS_CLOSE,(uint32_t)fd,0,0,0,0)); }

static void test_boot_config(void)
{
    /* The sealed file refuses every direct writer, administrators included. */
    CHECK(sys_open_as(&admin,"/System/BOOT.CFG",O_RDWR)==-EACCES);
    int32_t fd=sys_open_as(&alice,"/System/BOOT.CFG",O_RDONLY); CHECK(fd>=0); sys_close_as(&alice,fd);
    CHECK(save_boot(&alice,"serial=0\n")==-EPERM && save_boot(&guest,"serial=0\n")==-EPERM);
    char text[256]; size_t n;
    CHECK(!kernel_read("C:/System/BOOT.CFG",text,sizeof(text),&n) && n==16 && !memcmp(text,"safe=0 serial=1\n",16));
    CHECK(!save_boot(&admin,"safe=0 serial=0 menu=1\n"));
    CHECK(!kernel_read("C:/System/BOOT.CFG",text,sizeof(text),&n) && n==23 && !memcmp(text,"safe=0 serial=0 menu=1\n",23));
    CHECK(!save_boot(&admin,"x\n"));
    CHECK(!kernel_read("C:/System/BOOT.CFG",text,sizeof(text),&n) && n==2 && !memcmp(text,"x\n",2));
    CHECK(save_boot(&admin,"bad\001byte")==-EINVAL);
    struct ciuki_boot_config r={.version=1,.size=sizeof(r),.action=CIUKI_BOOT_CONFIG_SET,.length=128};
    memset(r.text,'a',sizeof(r.text)); CHECK(service(&admin,CIUKI_RECOVERY_BOOT_CONFIG,&r,sizeof(r))==-EINVAL);
    r.length=4; r.version=2; CHECK(service(&admin,CIUKI_RECOVERY_BOOT_CONFIG,&r,sizeof(r))==-EINVAL);
    CHECK(!save_boot(&systemp,"safe=0 serial=1\n"));
    CHECK(!kernel_read("C:/System/BOOT.CFG",text,sizeof(text),&n) && n==16);
    puts("H16 boot configuration: sealed BOOT.CFG refuses direct writes (administrator included); the SYSTEM service saves it for administrators/SYSTEM only, validated text, standard user and Guest EPERM PASS");
}

static void test_machine_logs(void)
{
    /* SYSTEM-only file: no direct read or enumeration for anyone. */
    CHECK(sys_open_as(&admin,"/System/Logs/BOOT.LOG",O_RDONLY)==-EACCES);
    CHECK(sys_open_as(&alice,"/System/Logs/BOOT.LOG",O_RDONLY)==-EACCES);
    char text[4096]; uint32_t total=0; struct ciuki_system_log r;
    CHECK(!log_read(&admin,CIUKI_SYSTEM_LOG_MACHINE,"BOOT.LOG",text,sizeof(text)-1,&total) && total==36 && strstr(text,"line 2"));
    CHECK(log_read(&alice,CIUKI_SYSTEM_LOG_MACHINE,"BOOT.LOG",text,sizeof(text)-1,&total)==-EPERM);
    CHECK(log_read(&guest,CIUKI_SYSTEM_LOG_MACHINE,"BOOT.LOG",text,sizeof(text)-1,&total)==-EPERM);
    CHECK(!log_read(&systemp,CIUKI_SYSTEM_LOG_MACHINE,"BOOT.LOG",text,sizeof(text)-1,&total));
    /* Offset paging and the size of a zero-byte read. */
    CHECK(!log_call(&admin,CIUKI_SYSTEM_LOG_READ,CIUKI_SYSTEM_LOG_MACHINE,"BOOT.LOG",0,15,0,6,0,&r) && r.bytes==6 && r.total==36);
    CHECK(!ua_read(admin.p->memory,text,admin.data,6) && !memcmp(text,"line 1",6));
    CHECK(!log_call(&admin,CIUKI_SYSTEM_LOG_READ,CIUKI_SYSTEM_LOG_MACHINE,"BOOT.LOG",0,0,0,0,0,&r) && !r.bytes && r.total==36);
    CHECK(log_read(&admin,CIUKI_SYSTEM_LOG_MACHINE,"NONE.LOG",text,16,&total)==-ENOENT);
    /* Names are 8.3 leaves (no traversal, no separators, no long names). */
    static const char *const bad[]={"../BOOT.CFG","Security/x","BOOT.LOGS","LONGNAME12.LOG","A B.LOG",".LOG","Users/u000003e8/../X.LOG","Users/u3e8/X.LOG",""};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) CHECK(log_read(&admin,CIUKI_SYSTEM_LOG_MACHINE,bad[i],text,16,&total)==-EINVAL);
    /* Administrators append the maintenance reports; nothing else. */
    CHECK(!log_append(&admin,CIUKI_SYSTEM_LOG_MACHINE,"SCANDISK.LOG","ScanDisk, drive C:, no errors\n",0));
    CHECK(!log_append(&admin,CIUKI_SYSTEM_LOG_MACHINE,"SFC.LOG","System File Checker: 9 checked\n",0));
    CHECK(log_append(&admin,CIUKI_SYSTEM_LOG_MACHINE,"BOOT.LOG","forged\n",0)==-EPERM);
    CHECK(log_append(&alice,CIUKI_SYSTEM_LOG_MACHINE,"SCANDISK.LOG","x\n",0)==-EPERM);
    CHECK(log_call(&admin,CIUKI_SYSTEM_LOG_REPLACE,CIUKI_SYSTEM_LOG_MACHINE,"SCANDISK.LOG",0,0,"x",1,0,0)==-EPERM);
    CHECK(!log_read(&admin,CIUKI_SYSTEM_LOG_MACHINE,"SCANDISK.LOG",text,sizeof(text)-1,&total) && !strcmp(text,"ScanDisk, drive C:, no errors\n"));
    /* LIST: the system logs, administrators only. */
    char names[8][48]; unsigned count=0;
    while(!log_call(&admin,CIUKI_SYSTEM_LOG_LIST,CIUKI_SYSTEM_LOG_MACHINE,0,count,0,0,0,0,&r)) { CHECK(count<8); memcpy(names[count++],r.name,48); }
    CHECK(count==3);
    bool boot=false,scan=false,sfc=false;
    for(unsigned i=0;i<count;i++) { boot|=!strcmp(names[i],"BOOT.LOG"); scan|=!strcmp(names[i],"SCANDISK.LOG"); sfc|=!strcmp(names[i],"SFC.LOG"); }
    CHECK(boot && scan && sfc);
    CHECK(log_call(&alice,CIUKI_SYSTEM_LOG_LIST,CIUKI_SYSTEM_LOG_MACHINE,0,0,0,0,0,0,&r)==-EPERM);
    CHECK(log_call(&guest,CIUKI_SYSTEM_LOG_LIST,CIUKI_SYSTEM_LOG_MACHINE,0,0,0,0,0,0,&r)==-EPERM);
    /* Malformed records. */
    struct ciuki_system_log bad_record={.version=1,.size=sizeof(bad_record),.action=9,.scope=1};
    CHECK(service(&admin,CIUKI_RECOVERY_LOG,&bad_record,sizeof(bad_record))==-EINVAL);
    bad_record.action=CIUKI_SYSTEM_LOG_READ; bad_record.scope=3; snprintf(bad_record.name,48,"BOOT.LOG");
    CHECK(service(&admin,CIUKI_RECOVERY_LOG,&bad_record,sizeof(bad_record))==-EINVAL);
    bad_record.scope=1; bad_record.bytes=CIUKI_SYSTEM_LOG_DATA_MAX+1; bad_record.buffer=admin.data;
    CHECK(service(&admin,CIUKI_RECOVERY_LOG,&bad_record,sizeof(bad_record))==-EINVAL);
    puts("H16 system logs: C:\\SYSTEM\\LOGS refuses every direct read; administrators/SYSTEM LIST and READ through the service (paging, 8.3 names), append only SCANDISK.LOG/SFC.LOG; standard user and Guest EPERM PASS");
}

static void test_user_logs(void)
{
    char text[8192]; uint32_t total=0; struct ciuki_system_log r;
    /* NetSurf's log is the user's own: <profile>\Local Settings\Application Data\CiukiOS\Logs. */
    CHECK(log_read(&alice,CIUKI_SYSTEM_LOG_USER,"NETSURF.LOG",text,64,&total)==-ENOENT);
    CHECK(log_call(&alice,CIUKI_SYSTEM_LOG_LIST,CIUKI_SYSTEM_LOG_USER,0,0,0,0,0,0,&r)==-ENOENT);
    const char *first="2026-10-28T10:00:00Z [netsurf] exit reason=quit\n", *second="2026-10-28T10:01:00Z [netsurf] fetch error\n";
    const uint32_t both=(uint32_t)(strlen(first)+strlen(second));
    CHECK(!log_append(&alice,CIUKI_SYSTEM_LOG_USER,"NETSURF.LOG",first,0));
    CHECK(!log_append(&alice,CIUKI_SYSTEM_LOG_USER,"NETSURF.LOG",second,0));
    size_t n; CHECK(!kernel_read("C:" ALICE_LOGS "/NETSURF.LOG",text,sizeof(text),&n) && n==both && strstr(text,"fetch error"));
    CHECK(!log_read(&alice,CIUKI_SYSTEM_LOG_USER,"NETSURF.LOG",text,sizeof(text)-1,&total) && total==both);
    /* The owner reads it directly as well; another user cannot; an administrator may (PROFILE rule). */
    int32_t fd=sys_open_as(&alice,ALICE_LOGS "/NETSURF.LOG",O_RDONLY); CHECK(fd>=0); sys_close_as(&alice,fd);
    CHECK(sys_open_as(&bob,ALICE_LOGS "/NETSURF.LOG",O_RDONLY)==-EACCES);
    fd=sys_open_as(&admin,ALICE_LOGS "/NETSURF.LOG",O_RDONLY); CHECK(fd>=0); sys_close_as(&admin,fd);
    /* Bob's USER scope is Bob's folder, never Alice's. */
    CHECK(log_read(&bob,CIUKI_SYSTEM_LOG_USER,"NETSURF.LOG",text,64,&total)==-ENOENT);
    CHECK(!log_append(&bob,CIUKI_SYSTEM_LOG_USER,"NETSURF.LOG","bob\n",0));
    CHECK(!log_read(&alice,CIUKI_SYSTEM_LOG_USER,"NETSURF.LOG",text,sizeof(text)-1,&total) && total==both);
    CHECK(kernel_exists("C:" BOB_HOME SYSVC_USER_LOGS "/NETSURF.LOG"));
    /* Crash reports: REPLACE of a slot; LIST of the user's logs. */
    CHECK(!log_call(&alice,CIUKI_SYSTEM_LOG_REPLACE,CIUKI_SYSTEM_LOG_USER,"CRASH0.TXT",0,0,"CiukiOS crash sequence=1\n",25,0,0));
    CHECK(!log_call(&alice,CIUKI_SYSTEM_LOG_REPLACE,CIUKI_SYSTEM_LOG_USER,"CRASH0.TXT",0,0,"CiukiOS crash sequence=2\n",25,0,&r) && r.total==25);
    CHECK(!log_call(&alice,CIUKI_SYSTEM_LOG_REPLACE,CIUKI_SYSTEM_LOG_USER,"WCRASH0.TXT",0,0,"CiukiOS crash sequence=1\n",25,0,0));
    CHECK(log_call(&alice,CIUKI_SYSTEM_LOG_REPLACE,CIUKI_SYSTEM_LOG_USER,"CRASH0.TXT",0,0,"x",1,7,0)==-EINVAL);
    unsigned count=0; bool netsurf=false,crash=false,wcrash=false;
    while(!log_call(&alice,CIUKI_SYSTEM_LOG_LIST,CIUKI_SYSTEM_LOG_USER,0,count,0,0,0,0,&r)) {
        count++; netsurf|=!strcmp(r.name,"NETSURF.LOG") && r.total==both; crash|=!strcmp(r.name,"CRASH0.TXT") && r.total==25; wcrash|=!strcmp(r.name,"WCRASH0.TXT");
    }
    CHECK(count==3 && netsurf && crash && wcrash);
    /* APPEND starts over past the limit (NETSURF.LOG's 64 KiB rule). */
    CHECK(!log_append(&alice,CIUKI_SYSTEM_LOG_USER,"LIMIT.LOG","0123456789",16));
    CHECK(!log_call(&alice,CIUKI_SYSTEM_LOG_APPEND,CIUKI_SYSTEM_LOG_USER,"LIMIT.LOG",0,0,"abcdefgh",8,16,&r) && r.total==8);
    CHECK(log_append(&alice,CIUKI_SYSTEM_LOG_USER,"LIMIT.LOG","0123456789abcdefgh",16)==-EFBIG);
    CHECK(log_append(&alice,CIUKI_SYSTEM_LOG_USER,"LIMIT.LOG","x",CIUKI_SYSTEM_LOG_LIMIT_MAX+1)==-EINVAL);
    /* Guest keeps its own logs in its own profile, Bob cannot read them. */
    CHECK(!log_append(&guest,CIUKI_SYSTEM_LOG_USER,"NETSURF.LOG","guest\n",0));
    CHECK(sys_open_as(&bob,GUEST_HOME SYSVC_USER_LOGS "/NETSURF.LOG",O_RDONLY)==-EACCES);
    /* SYSTEM work has no USER scope; a session without a profile that is not
     * an administrator gets none either. */
    CHECK(log_append(&systemp,CIUKI_SYSTEM_LOG_USER,"NETSURF.LOG","x\n",0)==-EINVAL);
    puts("H16 user logs: NetSurf and crash logs in <profile>\\Local Settings\\Application Data\\CiukiOS\\Logs written as the caller, owner/administrator readable, other users denied, per-user LIST/REPLACE, APPEND limit start-over PASS");

    /* The interim administrator session of a first boot has no profile: its
     * logs wait with the system logs (SYSTEM-owned) and show in MACHINE LIST. */
    CHECK(!log_append(&interim,CIUKI_SYSTEM_LOG_USER,"NETSURF.LOG","first start\n",0));
    CHECK(kernel_exists("C:/System/Logs/Users/u000003e8/NETSURF.LOG"));
    CHECK(sys_open_as(&interim,"/System/Logs/Users/u000003e8/NETSURF.LOG",O_RDONLY)==-EACCES);
    CHECK(!log_read(&interim,CIUKI_SYSTEM_LOG_USER,"NETSURF.LOG",text,sizeof(text)-1,&total) && !strcmp(text,"first start\n"));
    CHECK(!log_read(&admin,CIUKI_SYSTEM_LOG_MACHINE,"Users/u000003e8/NETSURF.LOG",text,sizeof(text)-1,&total) && total==12);
    bool held=false; count=0;
    while(!log_call(&admin,CIUKI_SYSTEM_LOG_LIST,CIUKI_SYSTEM_LOG_MACHINE,0,count,0,0,0,0,&r)) { count++; held|=!strcmp(r.name,"Users/u000003e8/NETSURF.LOG"); }
    CHECK(held && count==4);
    puts("H16 profile-less administrator session: logs held in C:\\SYSTEM\\LOGS\\Users\\u000003e8 (SYSTEM-owned), listed with the system logs PASS");
}

/* ---- driver packs ---- */
static int dev_present=1, dev_busy, dev_qualified;
static int fake_present(void *ctx,const struct ndp_device *d) { (void)ctx; (void)d; return dev_present; }
static int fake_busy(void *ctx,const struct ndp_device *d) { (void)ctx; (void)d; return dev_busy; }
static int fake_qualified(void *ctx,const struct ndp_device *d,const uint8_t h[32],const uint8_t s[32]) { (void)ctx; (void)d; (void)h; (void)s; return dev_qualified; }
static struct ndp_manifest *manifest;
static char sources[NDP_FILES][192];
static const char driver_bytes[]="MZ fake NT miniport image bytes for the package store";
static const char inf_bytes[]="[Version]\nSignature=\"$Windows NT$\"\nClass=Net\n";
static void make_manifest(int enabled)
{
    memset(manifest,0,sizeof(*manifest)); memset(sources,0,sizeof(sources));
    manifest->version=NDP_VERSION; manifest->size=sizeof(*manifest); manifest->enabled=(uint32_t)enabled; manifest->file_count=2; manifest->binary=0;
    manifest->parameter_count=1;
    manifest->device=(struct ndp_device){0,3,0,0x10ec,0x8139,0x10ec,0x8139,0x10};
    strcpy(manifest->package,"netpkg"); strcpy(manifest->inf,"netdrv.inf"); strcpy(manifest->service,"netdrv");
    strcpy(manifest->name,"Fixture Fast Ethernet"); strcpy(manifest->provider,"Fixture"); strcpy(manifest->reason,enabled?NDP_QUALIFIED:NDP_DISABLED);
    strcpy(manifest->files[0].name,"netdrv.sys"); manifest->files[0].bytes=sizeof(driver_bytes)-1; sha256(driver_bytes,sizeof(driver_bytes)-1,manifest->files[0].hash);
    strcpy(manifest->files[1].name,"netdrv.inf"); manifest->files[1].bytes=sizeof(inf_bytes)-1; sha256(inf_bytes,sizeof(inf_bytes)-1,manifest->files[1].hash);
    struct ndp_parameter *p=&manifest->parameters[0]; strcpy(p->key,"adapter"); strcpy(p->name,"Speed"); p->type=NDP_DWORD; p->bytes=4; p->data[0]=100;
    strcpy(sources[0],"bin/netdrv.sys"); strcpy(sources[1],"netdrv.inf");
    ndp_seal(manifest);
}
static int install(struct actor *a,const char *root,char reason[160])
{
    as(a); struct pack_caller caller={a->p,a->p->subject,root};
    struct sysvc_pack_ops ops={&caller,pack_open,fake_present,fake_busy,fake_qualified};
    return sysvc_pack_install(a->p->subject,&ops,manifest,(const char (*)[192])sources,reason);
}
static int uninstall(struct actor *a,char reason[160])
{
    as(a); struct pack_caller caller={a->p,a->p->subject,""};
    struct sysvc_pack_ops ops={&caller,pack_open,fake_present,fake_busy,fake_qualified};
    return sysvc_pack_remove(a->p->subject,&ops,&manifest->device,reason);
}
struct store_reader { struct vfs_table t; };
static int verify_read(void *ctx,const char *name,uint32_t offset,void *out,size_t cap,size_t *got)
{
    struct store_reader *r=ctx; char path[160]; snprintf(path,sizeof(path),"C:/System/drivers/netpkg/%s",name);
    int fd=vfs_open(&r->t,path,VFS_READ,VFS_DENY_NONE,0); if(fd<0) return -1;
    uint64_t at; int e=vfs_seek(&r->t,fd,offset,VFS_SEEK_SET,&at); if(!e) e=vfs_read(&r->t,fd,out,cap,got);
    CHECK(!vfs_close(&r->t,fd)); return e?-1:0;
}
static bool installed_valid(void)
{
    struct store_reader *r=calloc(1,sizeof(*r)); CHECK(r); CHECK(!vfs_table_init(&vfs,&r->t,2));
    bool ok=ndp_verify(manifest,&manifest->device,verify_read,r)!=0;
    vfs_table_destroy(&r->t); free(r); return ok;
}
static void test_driver_packs(void)
{
    manifest=calloc(1,sizeof(*manifest)); CHECK(manifest); char reason[160];
    { int made=make_dir("/Program Files/pack/bin"); CHECK(!made || made==-EEXIST); }
    kernel_file("C:/Program Files/pack/bin/netdrv.sys",driver_bytes,sizeof(driver_bytes)-1);
    kernel_file("C:/Program Files/pack/netdrv.inf",inf_bytes,sizeof(inf_bytes)-1);
    make_manifest(0);
    /* Standard user and Guest: refused before anything is read or written. */
    CHECK(install(&alice,"/Program Files/pack",reason)==-EPERM && strstr(reason,"administrator"));
    CHECK(install(&guest,"/Program Files/pack",reason)==-EPERM);
    CHECK(!kernel_exists("C:/System/drivers"));
    /* Administrator: staged, every byte verified, renamed into the sealed store. */
    CHECK(!install(&admin,"/Program Files/pack",reason) && !strcmp(reason,NDP_DISABLED));
    CHECK(kernel_exists("C:/System/drivers/netpkg/manifest.ndp") && kernel_exists("C:/System/drivers/netpkg/registry.ndp"));
    CHECK(!kernel_exists("C:/System/drivers/netpkg.stage") && !kernel_exists("C:/System/drivers/netpkg/transaction.ndp"));
    CHECK(installed_valid());
    /* The store stays sealed to the administrator's own file routes. */
    CHECK(sys_open_as(&admin,"/System/drivers/netpkg/evil.sys",O_CREAT|O_RDWR)==-EACCES);
    CHECK(sys_open_as(&admin,"/System/drivers/netpkg/netdrv.sys",O_RDWR)==-EACCES);
    int32_t fd=sys_open_as(&alice,"/System/drivers/netpkg/netdrv.sys",O_RDONLY); CHECK(fd>=0); sys_close_as(&alice,fd);
    CHECK(install(&admin,"/Program Files/pack",reason)==-EEXIST && strstr(reason,"Remove the existing package"));
    /* Removal: administrators only, once the host is stopped. */
    CHECK(uninstall(&alice,reason)==-EPERM && kernel_exists("C:/System/drivers/netpkg/manifest.ndp"));
    dev_present=0; CHECK(uninstall(&admin,reason)==-EBUSY && strstr(reason,"Device changed")); dev_present=1;
    CHECK(!uninstall(&admin,reason) && strstr(reason,"Driver package removed") && !kernel_exists("C:/System/drivers/netpkg"));
    CHECK(uninstall(&admin,reason)==-ENOENT && strstr(reason,"No removable package"));
    puts("H16 driver packs: standard user/Guest EPERM, administrator install verified byte for byte into the sealed store, direct writes refused, duplicate refused, removal checked PASS");

    /* The package folder is read as the caller: a folder the administrator
     * cannot read (SYSTEM-only) is never carried into the store by SYSTEM. */
    kernel_file("C:/System/Security/pack/netdrv.inf",inf_bytes,sizeof(inf_bytes)-1);
    struct vfs_table t; CHECK(!vfs_table_init(&vfs,&t,2)); CHECK(!vfs_mkdir(&t,"C:/System/Security/pack/bin",0)); vfs_table_destroy(&t);
    kernel_file("C:/System/Security/pack/bin/netdrv.sys",driver_bytes,sizeof(driver_bytes)-1);
    CHECK(install(&admin,"/System/Security/pack",reason)==-EACCES && strstr(reason,"rolled back"));
    CHECK(!kernel_exists("C:/System/drivers/netpkg") && !kernel_exists("C:/System/drivers/netpkg.stage"));
    CHECK(!install(&systemp,"/System/Security/pack",reason)); /* SYSTEM itself may */
    CHECK(!uninstall(&systemp,reason));
    /* A file that does not match the sealed manifest. */
    const char tampered[]="MZ fake NT miniport image bytes for the package stor!";
    kernel_file("C:/Program Files/pack/bin/netdrv.sys",tampered,sizeof(tampered)-1);
    CHECK(install(&admin,"/Program Files/pack",reason)==-EILSEQ && strstr(reason,"does not match"));
    CHECK(!kernel_exists("C:/System/drivers/netpkg") && !kernel_exists("C:/System/drivers/netpkg.stage"));
    kernel_file("C:/Program Files/pack/bin/netdrv.sys",driver_bytes,sizeof(driver_bytes)-1);
    /* A modified plan (the seal no longer matches) or an unsafe source path. */
    manifest->files[0].bytes++; CHECK(install(&admin,"/Program Files/pack",reason)==-EINVAL); manifest->files[0].bytes--;
    strcpy(sources[1],"../netdrv.inf"); CHECK(install(&admin,"/Program Files/pack",reason)==-EINVAL && strstr(reason,"Unsafe")); strcpy(sources[1],"netdrv.inf");
    /* A qualified (enabled) package needs the qualification and an idle host. */
    make_manifest(1);
    CHECK(install(&admin,"/Program Files/pack",reason)==-EBUSY && strstr(reason,"Qualification"));
    dev_qualified=1; dev_busy=1; CHECK(install(&admin,"/Program Files/pack",reason)==-EBUSY);
    dev_busy=0; CHECK(!install(&admin,"/Program Files/pack",reason) && !strcmp(reason,NDP_QUALIFIED) && installed_valid());
    dev_busy=1; CHECK(uninstall(&admin,reason)==-EBUSY && strstr(reason,"host stop") && kernel_exists("C:/System/drivers/netpkg/manifest.ndp"));
    dev_busy=0;
    /* An interrupted removal (remove.ndp written, manifest.ndp gone) is never
     * loaded again and is finished by the next request. */
    CHECK(!vfs_table_init(&vfs,&t,2));
    int out=vfs_open(&t,"C:/System/drivers/netpkg/remove.ndp",VFS_CREATE|VFS_WRITE,VFS_DENY_ALL,0); CHECK(out>=0);
    size_t done; CHECK(!vfs_write(&t,out,manifest,sizeof(*manifest),&done) && done==sizeof(*manifest)); CHECK(!vfs_close(&t,out));
    CHECK(!vfs_delete(&t,"C:/System/drivers/netpkg/manifest.ndp")); vfs_table_destroy(&t);
    CHECK(!install(&admin,"/Program Files/pack",reason) && installed_valid()); /* finishes the removal, then installs */
    CHECK(!vfs_table_init(&vfs,&t,2));
    out=vfs_open(&t,"C:/System/drivers/netpkg/remove.ndp",VFS_CREATE|VFS_WRITE,VFS_DENY_ALL,0); CHECK(out>=0);
    CHECK(!vfs_write(&t,out,manifest,sizeof(*manifest),&done)); CHECK(!vfs_close(&t,out));
    CHECK(!vfs_delete(&t,"C:/System/drivers/netpkg/manifest.ndp")); vfs_table_destroy(&t);
    CHECK(!uninstall(&admin,reason) && !strcmp(reason,"Driver package removed.") && !kernel_exists("C:/System/drivers/netpkg"));
    /* An empty folder an interrupted removal left is reused; a foreign file is kept. */
    CHECK(!vfs_table_init(&vfs,&t,2)); CHECK(!vfs_mkdir(&t,"C:/System/drivers/netpkg",0)); vfs_table_destroy(&t);
    CHECK(!install(&admin,"/Program Files/pack",reason));
    CHECK(!uninstall(&admin,reason));
    CHECK(!vfs_table_init(&vfs,&t,2)); CHECK(!vfs_mkdir(&t,"C:/System/drivers/netpkg",0)); vfs_table_destroy(&t);
    kernel_file("C:/System/drivers/netpkg/foreign.dat","x",1);
    CHECK(install(&admin,"/Program Files/pack",reason)==-EEXIST && kernel_exists("C:/System/drivers/netpkg/foreign.dat"));
    puts("H16 driver packs: caller-read sources (SYSTEM-only folder refused to the administrator), tampered file and modified plan refused with rollback, qualified host busy checks, interrupted removal finished, leftover folders PASS");

    /* The call-99 record through the adapter: validation, administrator check
     * before the device observation (no NIC in this fixture: EBUSY). */
    struct ciuki_driver_pack_job job={.version=1,.size=sizeof(job),.action=CIUKI_DRIVER_PACK_INSTALL,.manifest=alice.data,
        .manifest_bytes=sizeof(*manifest),.sources=alice.data+sizeof(*manifest),.source_bytes=2*192};
    strcpy(job.root,"/Program Files/pack"); make_manifest(0);
    as(&alice); CHECK(!ua_write(alice.p->memory,alice.data,manifest,sizeof(*manifest)) && !ua_write(alice.p->memory,alice.data+sizeof(*manifest),sources,2*192));
    CHECK(service(&alice,CIUKI_RECOVERY_DRIVER_PACK,&job,sizeof(job))==-EPERM && strstr(job.reason,"administrator"));
    job.manifest=admin.data; job.sources=admin.data+sizeof(*manifest);
    as(&admin); CHECK(!ua_write(admin.p->memory,admin.data,manifest,sizeof(*manifest)) && !ua_write(admin.p->memory,admin.data+sizeof(*manifest),sources,2*192));
    memset(job.reason,0,sizeof(job.reason));
    CHECK(service(&admin,CIUKI_RECOVERY_DRIVER_PACK,&job,sizeof(job))==-EBUSY && strstr(job.reason,"Device changed"));
    job.source_bytes=3*192; CHECK(service(&admin,CIUKI_RECOVERY_DRIVER_PACK,&job,sizeof(job))==-EINVAL);
    job.source_bytes=2*192; job.root[0]='x'; CHECK(service(&admin,CIUKI_RECOVERY_DRIVER_PACK,&job,sizeof(job))==-EINVAL);
    struct ciuki_driver_pack_job remove={.version=1,.size=sizeof(remove),.action=CIUKI_DRIVER_PACK_REMOVE,.bus=0,.slot=3,.vendor=0x10ec,.device=0x8139,
        .sub_vendor=0x10ec,.sub_device=0x8139,.revision=0x10};
    CHECK(service(&guest,CIUKI_RECOVERY_DRIVER_PACK,&remove,sizeof(remove))==-EPERM);
    CHECK(service(&admin,CIUKI_RECOVERY_DRIVER_PACK,&remove,sizeof(remove))==-ENOENT && strstr(remove.reason,"No removable"));
    remove.manifest=admin.data; CHECK(service(&admin,CIUKI_RECOVERY_DRIVER_PACK,&remove,sizeof(remove))==-EINVAL);
    free(manifest); manifest=0;
    puts("H16 driver pack record: adapter validation, administrator check first (standard user/Guest EPERM), device observation, reason returned PASS");
}

/* ---- System File Checker restore ---- */
static int32_t sfc(struct actor *a,const char *path,const char *source)
{
    struct ciuki_sfc_restore r={.version=1,.size=sizeof(r)};
    snprintf(r.path,sizeof(r.path),"%s",path); snprintf(r.source,sizeof(r.source),"%s",source);
    return service(a,CIUKI_RECOVERY_SFC_RESTORE,&r,sizeof(r));
}
static void test_sfc_restore(void)
{
    static const char original[]="\177ELF the original CiukiOS VMM image bytes";
    uint8_t digest[32]; char hex[65], list[256];
    sha256(original,sizeof(original)-1,digest); sha256_hex(digest,hex);
    int n=snprintf(list,sizeof(list),"; CiukiOS System File Checker baseline: sha256 size path\n%s %u /System/VMM.ELF\n",hex,(unsigned)sizeof(original)-1);
    kernel_file("C:/System/SFC.LST",list,(size_t)n);
    kernel_file("C:/System/VMM.ELF","corrupt",7);
    struct vfs_table t; CHECK(!vfs_table_init(&vfs,&t,2));
    static const char *const dirs[]={"C:/Program Files/cd","C:/Program Files/cd/System","C:/Program Files/bad","C:/Program Files/bad/System","C:/System/Security/cd","C:/System/Security/cd/System"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++) { int e=vfs_mkdir(&t,dirs[i],0); CHECK(!e || e==-FS_EEXIST); }
    vfs_table_destroy(&t);
    kernel_file("C:/Program Files/cd/System/VMM.ELF",original,sizeof(original)-1);
    kernel_file("C:/Program Files/bad/System/VMM.ELF","\177ELF something else entirely, same length!",sizeof(original)-1);
    kernel_file("C:/System/Security/cd/System/VMM.ELF",original,sizeof(original)-1);
    /* The kernel image and the baseline are sealed: no direct writer. */
    CHECK(sys_open_as(&admin,"/System/VMM.ELF",O_RDWR)==-EACCES && sys_open_as(&admin,"/System/SFC.LST",O_RDWR)==-EACCES);
    int32_t fd=sys_open_as(&alice,"/System/SFC.LST",O_RDONLY); CHECK(fd>=0); sys_close_as(&alice,fd);
    CHECK(sfc(&alice,"/System/VMM.ELF","/Program Files/cd")==-EPERM && sfc(&guest,"/System/VMM.ELF","/Program Files/cd")==-EPERM);
    char text[128]; size_t got;
    CHECK(sfc(&admin,"/System/VMM.ELF","/Program Files/bad")==-EILSEQ);
    CHECK(sfc(&admin,"/System/VMM.ELF","/System/Security/cd")==-EACCES); /* read as the administrator */
    CHECK(sfc(&admin,"/System/OTHER.BIN","/Program Files/cd")==-ENOENT);
    CHECK(sfc(&admin,"/System/../System/VMM.ELF","/Program Files/cd")==-EINVAL && sfc(&admin,"System/VMM.ELF","/Program Files/cd")==-EINVAL);
    CHECK(!kernel_read("C:/System/VMM.ELF",text,sizeof(text),&got) && got==7 && !memcmp(text,"corrupt",7));
    CHECK(!kernel_exists("C:/System/VMM.ELF.sfn"));
    CHECK(!sfc(&admin,"/system/vmm.elf","/Program Files/cd"));
    CHECK(!kernel_read("C:/System/VMM.ELF",text,sizeof(text),&got) && got==sizeof(original)-1 && !memcmp(text,original,got));
    CHECK(!kernel_read("C:/System/Helpdesk/SFC/vmm.elf",text,sizeof(text),&got) && got==7 && !memcmp(text,"corrupt",7));
    CHECK(!kernel_exists("C:/System/VMM.ELF.sfn"));
    /* A deleted file comes back; SYSTEM may restore from a folder only it reads. */
    struct vfs_table d; CHECK(!vfs_table_init(&vfs,&d,2)); CHECK(!vfs_delete(&d,"C:/System/VMM.ELF")); vfs_table_destroy(&d);
    CHECK(!sfc(&systemp,"/System/VMM.ELF","/System/Security/cd"));
    CHECK(!kernel_read("C:/System/VMM.ELF",text,sizeof(text),&got) && got==sizeof(original)-1 && !memcmp(text,original,got));
    puts("H16 System File Checker restore: sealed VMM.ELF and SFC.LST refuse direct writes; administrators restore the baseline's original (read as the caller, size and SHA-256 checked, Helpdesk backup, one replacing rename); EPERM for standard user/Guest, EILSEQ/ENOENT/EINVAL PASS");
}

/* ---- sec-sysvc2: SYSTEM jobs, sealed boot settings, Registry Checker's
 * SYSTEM-only backups (fs/sysvc_jobs.c, fs/security_policy.c) ---- */
#define SCANDISK_TEXT_VFORMAT vsnprintf
#include <ciuki/scandisk_text.h>
#include <strings.h>
static struct actor service_subject; /* uid 1002, no session: not a desktop */
/* The kernel's at-start ScanDisk records (mount.c in the kernel). */
static struct ciuki_scandisk at_start[26]; static bool at_start_ran[26];
int storage_scandisk_report(unsigned d,struct ciuki_scandisk *out)
{
    if(d>=26) return -EINVAL;
    if(at_start_ran[d]) *out=at_start[d];
    else { memset(out,0,sizeof(*out)); out->version=CIUKI_SCANDISK_VERSION; out->size=sizeof(*out); out->drive=d; }
    return 0;
}
/* The display registry's declared attempt files (drivers/video/registry.c). */
bool display_attempt_file(const char *leaf) { return !strcasecmp(leaf,"trident.cfg") || !strcasecmp(leaf,"nv2d0110.cfg"); }
static int32_t job(struct actor *a,uint32_t which,uint32_t *done)
{
    struct ciuki_system_job r={.version=1,.size=sizeof(r),.job=which};
    int32_t e=service(a,CIUKI_RECOVERY_JOB,&r,sizeof(r)); if(done) *done=r.done; return e;
}
static int32_t setting(struct actor *a,uint32_t which,const char *text,uint32_t length)
{
    struct ciuki_system_setting r={.version=1,.size=sizeof(r),.action=CIUKI_SYSTEM_SETTING_SET,.setting=which,
        .buffer=length?a->data:0,.length=length};
    as(a); if(length) CHECK(!ua_write(a->p->memory,a->data,text,length));
    return service(a,CIUKI_RECOVERY_SETTING,&r,sizeof(r));
}
static void scandisk_record(unsigned d,uint32_t flags,uint32_t files)
{
    struct ciuki_scandisk *r=&at_start[d]; memset(r,0,sizeof(*r));
    r->version=CIUKI_SCANDISK_VERSION; r->size=sizeof(*r); r->drive=d; r->flags=CIUKI_SCANDISK_RAN|CIUKI_SCANDISK_AT_START|flags;
    r->cluster_bytes=4096; r->clusters=130000; r->free_clusters=100000; r->files=files; r->directories=56;
    r->file_clusters=25000; r->directory_clusters=60; at_start_ran[d]=true;
}
static void test_jobs(void)
{
    char text[8192]; size_t n, before; uint32_t done=99;
    /* (test_machine_logs left an administrator's entry in SCANDISK.LOG.) */
    CHECK(!kernel_read("C:/System/Logs/SCANDISK.LOG",text,sizeof(text)-1,&before) && before);
    /* Nothing ran at this start: nothing is written, for anyone. */
    CHECK(!job(&guest,CIUKI_SYSTEM_JOB_STARTUP_REPORT,&done) && !done);
    CHECK(!kernel_read("C:/System/Logs/SCANDISK.LOG",text,sizeof(text)-1,&n) && n==before);
    scandisk_record(2,CIUKI_SCANDISK_FIX|CIUKI_SCANDISK_ERRORS|CIUKI_SCANDISK_FIXED,1234);
    scandisk_record(3,0,77);
    /* A Guest session's desktop asks; SYSTEM appends both reports, once. */
    CHECK(!job(&guest,CIUKI_SYSTEM_JOB_STARTUP_REPORT,&done) && done==2);
    CHECK(!kernel_read("C:/System/Logs/SCANDISK.LOG",text,sizeof(text)-1,&n) && n>before); text[n]=0;
    CHECK(!strncmp(text+before,"ScanDisk, ",10));
    char expected[2048]; scandisk_report_text(&at_start[2],expected,sizeof(expected));
    CHECK(strstr(text,"drive C:, Standard (at start)\n") && strstr(text,expected) && strstr(text,"fixed them all"));
    scandisk_report_text(&at_start[3],expected,sizeof(expected));
    CHECK(strstr(text,"drive D:, Standard (at start)\n") && strstr(text,expected) && strstr(text,"77 user files"));
    const size_t first=n;
    /* The log stays SYSTEM's: no direct access, administrators read it through LOG. */
    CHECK(sys_open_as(&guest,"/System/Logs/SCANDISK.LOG",O_RDONLY)==-EACCES && sys_open_as(&admin,"/System/Logs/SCANDISK.LOG",O_RDONLY)==-EACCES);
    CHECK(log_read(&guest,CIUKI_SYSTEM_LOG_MACHINE,"SCANDISK.LOG",text,64,0)==-EPERM);
    uint32_t total=0; CHECK(!log_read(&admin,CIUKI_SYSTEM_LOG_MACHINE,"SCANDISK.LOG",text,sizeof(text)-1,&total) && total==first);
    /* Every other session, and SYSTEM, find the work done. */
    struct actor *again[]={&alice,&bob,&admin,&interim,&systemp};
    for(unsigned i=0;i<5;i++) { done=99; CHECK(!job(again[i],CIUKI_SYSTEM_JOB_STARTUP_REPORT,&done) && !done); }
    CHECK(!kernel_read("C:/System/Logs/SCANDISK.LOG",text,sizeof(text)-1,&n) && n==first);
    /* A report that appears later is appended by the next request (a Standard user's). */
    scandisk_record(4,CIUKI_SCANDISK_THOROUGH,5);
    CHECK(!job(&alice,CIUKI_SYSTEM_JOB_STARTUP_REPORT,&done) && done==1);
    CHECK(!kernel_read("C:/System/Logs/SCANDISK.LOG",text,sizeof(text)-1,&n)); text[n]=0;
    CHECK(n>first && strstr(text,"drive E:, Thorough (at start)\n"));
    /* A subject without a session that is not SYSTEM has no session to maintain. */
    CHECK(job(&service_subject,CIUKI_SYSTEM_JOB_STARTUP_REPORT,&done)==-EPERM);
    /* Records: unknown job, version, reserved words, a nonzero `done` in. */
    CHECK(job(&admin,7,&done)==-EINVAL);
    struct ciuki_system_job bad={.version=2,.size=sizeof(bad),.job=1};
    CHECK(service(&admin,CIUKI_RECOVERY_JOB,&bad,sizeof(bad))==-EINVAL);
    bad.version=1; bad.reserved[3]=1; CHECK(service(&admin,CIUKI_RECOVERY_JOB,&bad,sizeof(bad))==-EINVAL);
    bad.reserved[3]=0; bad.done=1; CHECK(service(&admin,CIUKI_RECOVERY_JOB,&bad,sizeof(bad))==-EINVAL);
    puts("H16 sec-sysvc2 SYSTEM job: the at-start ScanDisk reports go to C:\\SYSTEM\\LOGS\\SCANDISK.LOG as SYSTEM for a Guest, standard user, administrator, interim or SYSTEM request, once per start and drive, in the tools' format; the log stays SYSTEM's; no-session service subject EPERM PASS");
}
static void test_settings(void)
{
    char text[512]; size_t n;
    { int e=make_dir("/System/Settings"); CHECK(!e || e==-EEXIST); }
    kernel_file("C:/System/HWPROF.CFG","0:name=Original Configuration\n",30);
    kernel_file("C:/System/Settings/performance.cfg","version=1\nrole=0\n",17);
    kernel_file("C:/System/Settings/accel.cfg","accel=attempt\n",14);
    /* Boot inputs are sealed by name: every direct writer is refused (the
     * administrator too), everyone may read them; ordinary settings stay
     * administrator-writable. */
    static const char *const sealed[]={"/System/HWPROF.CFG","/System/Settings/performance.cfg","/System/Settings/hwprofiles.cfg",
        "/System/Settings/accel-retry.cfg","/System/Settings/accel.cfg","/System/Settings/ACCEL-S3SAVAGE-5333-8C2E.CFG",
        "/System/Settings/accel-i810.cfg","/System/Settings/trident.cfg","/System/Settings/nv2d0110.cfg"};
    for(unsigned i=0;i<sizeof(sealed)/sizeof(*sealed);i++) {
        CHECK(sys_open_as(&admin,sealed[i],O_RDWR|O_CREAT)==-EACCES);
        CHECK(sys_open_as(&interim,sealed[i],O_RDWR|O_CREAT)==-EACCES);
    }
    int32_t fd=sys_open_as(&alice,"/System/Settings/performance.cfg",O_RDONLY); CHECK(fd>=0); sys_close_as(&alice,fd);
    fd=sys_open_as(&guest,"/System/HWPROF.CFG",O_RDONLY); CHECK(fd>=0); sys_close_as(&guest,fd);
    fd=sys_open_as(&admin,"/System/Settings/desktop.cfg",O_RDWR|O_CREAT); CHECK(fd>=0); sys_close_as(&admin,fd);
    fd=sys_open_as(&admin,"/System/Settings/trident2.cfg",O_RDWR|O_CREAT); CHECK(fd>=0); sys_close_as(&admin,fd);
    CHECK(sys_open_as(&alice,"/System/Settings/desktop.cfg",O_RDWR)==-EACCES);
    /* The SYSTEM service: administrators (the interim session too) and SYSTEM. */
    const char perf[]="version=1\nrole=1\nread_ahead=2\ngraphics=0\nvm=1\nmin_mb=32\nmax_mb=256\n";
    const char hw[]="0:name=Docked\n0:device=00010203 disabled=1 removed=0 driver=-\n1:name=Undocked\n";
    const char retry[]="driver=s3savage\npci=5333:8c2e\n";
    struct actor *refused[]={&alice,&bob,&guest,&service_subject};
    for(unsigned i=0;i<4;i++) {
        CHECK(setting(refused[i],CIUKI_SYSTEM_SETTING_PERFORMANCE,perf,sizeof(perf)-1)==-EPERM);
        CHECK(setting(refused[i],CIUKI_SYSTEM_SETTING_HWPROFILES,hw,sizeof(hw)-1)==-EPERM);
        CHECK(setting(refused[i],CIUKI_SYSTEM_SETTING_ACCEL_RETRY,retry,sizeof(retry)-1)==-EPERM);
    }
    CHECK(!kernel_read("C:/System/Settings/performance.cfg",text,sizeof(text),&n) && n==17);
    CHECK(!setting(&admin,CIUKI_SYSTEM_SETTING_PERFORMANCE,perf,sizeof(perf)-1));
    CHECK(!kernel_read("C:/System/Settings/performance.cfg",text,sizeof(text),&n) && n==sizeof(perf)-1 && !memcmp(text,perf,n));
    CHECK(!setting(&interim,CIUKI_SYSTEM_SETTING_HWPROFILES,hw,sizeof(hw)-1));
    CHECK(!kernel_read("C:/System/HWPROF.CFG",text,sizeof(text),&n) && n==sizeof(hw)-1 && !memcmp(text,hw,n));
    CHECK(!kernel_exists("C:/System/Settings/accel-retry.cfg"));
    CHECK(!setting(&systemp,CIUKI_SYSTEM_SETTING_ACCEL_RETRY,retry,sizeof(retry)-1));
    CHECK(!kernel_read("C:/System/Settings/accel-retry.cfg",text,sizeof(text),&n) && n==sizeof(retry)-1 && !memcmp(text,retry,n));
    /* A shorter text replaces the whole file. */
    CHECK(!setting(&admin,CIUKI_SYSTEM_SETTING_HWPROFILES,"0:name=A\n",9));
    CHECK(!kernel_read("C:/System/HWPROF.CFG",text,sizeof(text),&n) && n==9);
    /* The kernel accepts only what it and the loader read at the next start. */
    static const char *const bad_perf[]={"role=1\n","version=1\nrole=9\n","version=1\nmin_mb=8\n","version=2\n","version=1\nrole=1\nrole=2\n"};
    for(unsigned i=0;i<sizeof(bad_perf)/sizeof(*bad_perf);i++) CHECK(setting(&admin,CIUKI_SYSTEM_SETTING_PERFORMANCE,bad_perf[i],(uint32_t)strlen(bad_perf[i]))==-EINVAL);
    static const char *const bad_retry[]={"driver=S3\npci=5333:8c2e\n","driver=s3\npci=5333:8C2E\n","driver=s3\npci=5333:8c2e","driver=\npci=5333:8c2e\n",
        "driver=abcdefghijklmnop\npci=5333:8c2e\n","driver=s3\npci=5333-8c2e\n","driver=s3\npci=5333:8c2e\nx"};
    for(unsigned i=0;i<sizeof(bad_retry)/sizeof(*bad_retry);i++) CHECK(setting(&admin,CIUKI_SYSTEM_SETTING_ACCEL_RETRY,bad_retry[i],(uint32_t)strlen(bad_retry[i]))==-EINVAL);
    CHECK(!setting(&admin,CIUKI_SYSTEM_SETTING_ACCEL_RETRY,"driver=abcdefghijklmno\npci=0000:ffff\n",37));
    CHECK(setting(&admin,CIUKI_SYSTEM_SETTING_HWPROFILES,"0:name=\001\n",10)==-EINVAL);
    CHECK(setting(&admin,9,"x",1)==-EINVAL);
    CHECK(!kernel_read("C:/System/Settings/performance.cfg",text,sizeof(text),&n) && n==sizeof(perf)-1);
    /* The record: the size bound, version, reserved words, the action. */
    struct ciuki_system_setting r={.version=1,.size=sizeof(r),.action=CIUKI_SYSTEM_SETTING_SET,.setting=CIUKI_SYSTEM_SETTING_HWPROFILES,
        .buffer=admin.data,.length=CIUKI_SYSTEM_SETTING_MAX+1};
    CHECK(service(&admin,CIUKI_RECOVERY_SETTING,&r,sizeof(r))==-EINVAL);
    r.length=4; r.reserved[1]=1; CHECK(service(&admin,CIUKI_RECOVERY_SETTING,&r,sizeof(r))==-EINVAL);
    r.reserved[1]=0; r.action=2; CHECK(service(&admin,CIUKI_RECOVERY_SETTING,&r,sizeof(r))==-EINVAL);
    r.action=1; r.version=2; CHECK(service(&admin,CIUKI_RECOVERY_SETTING,&r,sizeof(r))==-EINVAL);
    r.version=1; r.buffer=0; CHECK(service(&admin,CIUKI_RECOVERY_SETTING,&r,sizeof(r))==-EINVAL);
    /* A large hardware-profile file (the loader reads up to 24575 bytes). */
    char *big=malloc(CIUKI_SYSTEM_SETTING_MAX); CHECK(big);
    for(unsigned i=0;i<CIUKI_SYSTEM_SETTING_MAX;i++) big[i]=i%64==63?'\n':'a'+(char)(i%26);
    as(&admin); struct ciuki_mmap_args mapping={.size=sizeof(mapping),.length=32*1024,.prot=PROT_READ|PROT_WRITE,.flags=MAP_PRIVATE|MAP_ANONYMOUS,.fd=-1};
    uint32_t region=(uint32_t)ua_mmap(admin.p->memory,&mapping); CHECK(region>=CIUKI_MMAP_BASE);
    CHECK(!ua_write(admin.p->memory,region,big,CIUKI_SYSTEM_SETTING_MAX));
    r.buffer=region; r.length=CIUKI_SYSTEM_SETTING_MAX; CHECK(!service(&admin,CIUKI_RECOVERY_SETTING,&r,sizeof(r)));
    struct vfs_table t; CHECK(!vfs_table_init(&vfs,&t,2)); struct fat_entry entry;
    CHECK(!vfs_stat(&t,"C:/System/HWPROF.CFG",&entry) && entry.size==CIUKI_SYSTEM_SETTING_MAX); vfs_table_destroy(&t);
    free(big);
    puts("H16 sec-sysvc2 sealed boot settings: HWPROF.CFG, performance.cfg, hwprofiles.cfg and the display attempt files (accel*.cfg, declared driver files) refuse every direct writer, administrators included; the SYSTEM setting service saves validated text for administrators, the interim session and SYSTEM; standard users, Guest and a no-session subject EPERM PASS");

    /* Registry Checker's backups are SYSTEM-only (the registry broker's). */
    { int e=make_dir("/System/SysBckup"); CHECK(!e || e==-EEXIST); }
    kernel_file("C:/System/SysBckup/RB000.CRB","CRB1",4);
    CHECK(sys_open_as(&admin,"/System/SysBckup/RB000.CRB",O_RDONLY)==-EACCES && sys_open_as(&alice,"/System/SysBckup/RB000.CRB",O_RDONLY)==-EACCES);
    CHECK(sys_open_as(&admin,"/System/SysBckup/RESTORE.PND",O_RDWR|O_CREAT)==-EACCES);
    puts("H16 sec-sysvc2 Registry Checker backups: C:\\SYSTEM\\SYSBCKUP refuses every direct reader and writer, administrators included PASS");
}
/* ---- end sec-sysvc2 ---- */

/* ---- sec-k08: clock.cfg sealed behind SETTING CLOCK (lead decision S2-5)
 * and the machine power buttons power.cfg behind SETTING POWER (U4-6). */
static int32_t sys_unlink_as(struct actor *a,const char *path)
{ as(a); CHECK(!ua_write(a->p->memory,USER,path,(uint32_t)strlen(path)+1)); return call(CIUKI_SYS_UNLINK,USER,0,0,0,0); }
static void test_k08_settings(void)
{
    char text[512]; size_t n;
    const char clock[]="version=2\nrtc_local=1\noffset_minutes=60\nauto_dst=1\nrtc_bias_minutes=120\nzone=Europe/Rome\n"
                       "tz=CET-1CEST,M3.5.0,M10.5.0/3\n";
    const char power[]="power_button=standby\npower_lid=shutdown\n";
    kernel_file("C:/System/Settings/clock.cfg","version=1\nrtc_local=0\noffset_minutes=0\n",37);
    kernel_file("C:/System/Settings/power.cfg","power_button=shutdown\n",22);
    static const char *const sealed[]={"/System/Settings/clock.cfg","/System/Settings/power.cfg","/System/Settings/CLOCK.CFG",
        "/System/Settings/Power.Cfg"};
    struct actor *direct[]={&admin,&interim,&alice,&guest};
    for(unsigned i=0;i<sizeof(sealed)/sizeof(*sealed);i++) for(unsigned w=0;w<4;w++) {
        CHECK(sys_open_as(direct[w],sealed[i],O_RDWR)==-EACCES);
        CHECK(sys_open_as(direct[w],sealed[i],O_WRONLY|O_TRUNC)==-EACCES);
    }
    CHECK(sys_unlink_as(&admin,"/System/Settings/clock.cfg")==-EACCES);
    /* Everyone reads them: the desktop of every session reads power.cfg. */
    for(unsigned w=0;w<4;w++) for(unsigned i=0;i<2;i++) {
        int32_t fd=sys_open_as(direct[w],sealed[i],O_RDONLY); CHECK(fd>=0); sys_close_as(direct[w],fd);
    }
    /* Look-alike names and the same names elsewhere keep their own class. */
    int32_t fd=sys_open_as(&admin,"/System/Settings/clock2.cfg",O_RDWR|O_CREAT); CHECK(fd>=0); sys_close_as(&admin,fd);
    struct actor *refused[]={&alice,&bob,&guest,&service_subject};
    for(unsigned i=0;i<4;i++) {
        CHECK(setting(refused[i],CIUKI_SYSTEM_SETTING_CLOCK,clock,sizeof(clock)-1)==-EPERM);
        CHECK(setting(refused[i],CIUKI_SYSTEM_SETTING_POWER,power,sizeof(power)-1)==-EPERM);
    }
    CHECK(!kernel_read("C:/System/Settings/clock.cfg",text,sizeof(text),&n) && n==37);
    CHECK(!kernel_read("C:/System/Settings/power.cfg",text,sizeof(text),&n) && n==22);
    CHECK(!setting(&admin,CIUKI_SYSTEM_SETTING_CLOCK,clock,sizeof(clock)-1));
    CHECK(!kernel_read("C:/System/Settings/clock.cfg",text,sizeof(text),&n) && n==sizeof(clock)-1 && !memcmp(text,clock,n));
    CHECK(!setting(&interim,CIUKI_SYSTEM_SETTING_POWER,power,sizeof(power)-1));
    CHECK(!kernel_read("C:/System/Settings/power.cfg",text,sizeof(text),&n) && n==sizeof(power)-1 && !memcmp(text,power,n));
    CHECK(!setting(&systemp,CIUKI_SYSTEM_SETTING_POWER,"power_lid=none\n",15));
    CHECK(!kernel_read("C:/System/Settings/power.cfg",text,sizeof(text),&n) && n==15);
    /* Only what the time zone service and the desktop accept. */
    static const char *const bad_clock[]={"","version=3\nrtc_local=0\n","rtc_local=0\n","version=1\nrtc_local=0","version=1\nrtc_local=2\n",
        "version=2\nrtc_local=0\noffset_minutes=900\n","version=2\nrtc_local=0\ntz=@@@\n","version=2\nrtc_local=0\nzone\n"};
    for(unsigned i=0;i<sizeof(bad_clock)/sizeof(*bad_clock);i++)
        CHECK(setting(&admin,CIUKI_SYSTEM_SETTING_CLOCK,bad_clock[i],(uint32_t)strlen(bad_clock[i]))==-EINVAL);
    static const char *const bad_power[]={"","power_button=standby","power_button=sleep\n","power_lid=open\n","power_scheme=0\n",
        "power_button=standby\npower_button=shutdown\n","power_lid=none\npower_lid=none\n","power_button=standby\n\n",
        " power_button=standby\n","power_button=standby\r\n"};
    for(unsigned i=0;i<sizeof(bad_power)/sizeof(*bad_power);i++)
        CHECK(setting(&admin,CIUKI_SYSTEM_SETTING_POWER,bad_power[i],(uint32_t)strlen(bad_power[i]))==-EINVAL);
    char long_power[160]; memset(long_power,'x',sizeof(long_power));
    CHECK(setting(&admin,CIUKI_SYSTEM_SETTING_POWER,long_power,128)==-EINVAL);
    char long_clock[600]; memcpy(long_clock,clock,sizeof(clock)-1);
    for(size_t i=sizeof(clock)-1;i<sizeof(long_clock);i++) long_clock[i]=(i%40==39)?'\n':'x';
    long_clock[sizeof(long_clock)-1]='\n';
    CHECK(setting(&admin,CIUKI_SYSTEM_SETTING_CLOCK,long_clock,sizeof(long_clock))==-EINVAL);
    CHECK(setting(&admin,6,"x\n",2)==-EINVAL);
    CHECK(!kernel_read("C:/System/Settings/clock.cfg",text,sizeof(text),&n) && n==sizeof(clock)-1);
    CHECK(!kernel_read("C:/System/Settings/power.cfg",text,sizeof(text),&n) && n==15);
    puts("H16 sec-k08 sealed clock.cfg and power.cfg: every direct writer refused (administrators and the interim session too), everyone reads; SETTING CLOCK/POWER for administrators, interim and SYSTEM with the time zone service's and the desktop's own validation; standard users, Guest and a no-session subject EPERM PASS");
}
#ifdef CIUKI_POLICY_COVERAGE
/* H13 native frontend (tests/host/security/test_policy_coverage.py): a
 * Standard user (Alice, uid 1007) and Guest try to reach SYSTEM state,
 * another user's (Bob's) profile, hive, crash logs, process and token, and
 * the sealed boot/service set, through the production native syscalls, call
 * 99 and call 101 on the real FAT/VFS/policy/OWNER stack. Every attempt is
 * refused; after one warm attempt, three more change no fd, open
 * description, pin, namespace node, page, heap block or subject/session
 * reference (the teardown in main then requires zero of each). Unknown
 * selectors of every native dispatcher deny without side effects. */
static unsigned h13_cases;
struct h13_state { unsigned pages, heap, fds; uint32_t descriptions, pins, nodes, refs, session_refs, owner_reads, disk_reads; };
static unsigned h13_owner_reads;
static int (*h13_owner_read_next)(void *, unsigned, void *, size_t, size_t *);
static int h13_owner_read(void *c, unsigned i, void *p, size_t cap, size_t *n) { h13_owner_reads++; return h13_owner_read_next(c, i, p, cap, n); }
static void h13_snapshot(struct actor *a, struct h13_state *s)
{
    struct file_ledger l; files_snapshot(space, &l); unsigned fds = 0;
    for (int i = 0; i < CIUKI_OPEN_MAX; i++) fds += !!a->p->fds[i].object;
    *s = (struct h13_state){pages_used, heap_blocks, fds, l.descriptions, l.pins, l.nodes, a->p->subject->refs,
        a->p->subject->session ? a->p->subject->session->refs : 0, h13_owner_reads, disk[0].read_calls + disk[1].read_calls};
}
static void h13_same(const char *what, const struct h13_state *b, const struct h13_state *e)
{
    if (memcmp(b, e, sizeof(*b))) printf("H13 leak in %s: pages %u/%u heap %u/%u fds %u/%u descriptions %u/%u pins %u/%u nodes %u/%u refs %u/%u\n",
        what, b->pages, e->pages, b->heap, e->heap, b->fds, e->fds, b->descriptions, e->descriptions, b->pins, e->pins, b->nodes, e->nodes, b->refs, e->refs);
    CHECK(b->pages == e->pages && b->heap == e->heap && b->fds == e->fds && b->descriptions == e->descriptions &&
          b->pins == e->pins && b->nodes == e->nodes && b->refs == e->refs && b->session_refs == e->session_refs);
}
/* One warm attempt, then three counted ones: each must be one of the two
 * expected refusals and leave every resource counter unchanged. */
#define H13_REFUSED(a, what, expr, e1, e2) do { \
    int32_t h13_r = (expr); \
    if (h13_r != (e1) && h13_r != (e2)) printf("H13 %s %s: %d\n", (a)->name, what, h13_r); \
    CHECK(h13_r == (e1) || h13_r == (e2)); \
    struct h13_state h13_b, h13_e; h13_snapshot(a, &h13_b); \
    for (unsigned h13_i = 0; h13_i < 3; h13_i++) { h13_r = (expr); CHECK(h13_r == (e1) || h13_r == (e2)); } \
    h13_snapshot(a, &h13_e); h13_same(what, &h13_b, &h13_e); h13_cases++; } while (0)
static int32_t h13_path_call(struct actor *a, uint32_t nr, const char *path, const char *path2, uint32_t c)
{
    as(a); CHECK(!ua_write(a->p->memory, USER, path, (uint32_t)strlen(path) + 1));
    if (path2) CHECK(!ua_write(a->p->memory, USER2, path2, (uint32_t)strlen(path2) + 1));
    return call(nr, USER, path2 ? USER2 : c, 0666, 0, 0);
}
static int32_t h13_open(struct actor *a, const char *path, uint32_t flags)
{
    int32_t fd = sys_open_as(a, path, flags);
    if (fd >= 0) sys_close_as(a, fd);
    return fd;
}
static int32_t h13_info(struct actor *a, const char *path, uint32_t operation)
{
    struct ciuki_file_info r = {.version = CIUKI_FILE_INFO_VERSION, .size = sizeof(r), .operation = operation,
        .mask = operation == CIUKI_FILE_INFO_SET ? CIUKI_FILE_INFO_ATTRIBUTES : 0, .attributes = 1};
    as(a); CHECK(!ua_write(a->p->memory, USER, path, (uint32_t)strlen(path) + 1));
    CHECK(!ua_write(a->p->memory, USER2, &r, sizeof(r)));
    return call(CIUKI_SYS_FILE_INFO, USER, USER2, sizeof(r), 0, 0);
}
static int32_t h13_proc(struct actor *a, uint32_t nr, uint32_t b, uint32_t c, uint32_t d)
{
    as(a); struct trap_frame tf = {.eax = nr, .ebx = b, .ecx = c, .edx = d};
    CHECK(!file_syscall(&tf));
    return proc_syscall(&tf);
}
static int32_t h13_security(struct actor *a, uint32_t operation, uint32_t target_pid, uint32_t session)
{
    struct ciuki_security_args r = {.version = 1, .size = 64, .operation = operation, .target_pid = target_pid,
        .session_id = session};
    if (operation >= 1 && operation <= 3) { r.output_address = USER2; r.output_bytes = 64; }
    as(a); CHECK(!ua_write(a->p->memory, USER, &r, sizeof(r)));
    return h13_proc(a, CIUKI_SYS_SECURITY, USER, 64, 0);
}
static int32_t h13_recovery(struct actor *a, uint32_t op, uint32_t bytes)
{
    uint8_t record[512] = {0}; uint32_t v[2] = {1, bytes}; memcpy(record, v, sizeof(v));
    as(a); CHECK(!ua_write(a->p->memory, REC, record, bytes));
    struct trap_frame tf = {.eax = CIUKI_SYS_RECOVERY, .ebx = op, .ecx = REC, .edx = bytes}; int32_t r = 0;
    return sysvc_syscall(a->p, a->p->subject, &tf, &r) ? r : -ENOSYS;
}
static void h13_layout(void)
{
    static const char *const dirs[] = {BOB_HOME "/Local Settings", BOB_HOME "/Local Settings/Application Data",
        BOB_HOME "/Local Settings/Application Data/CiukiOS", BOB_HOME SYSVC_USER_LOGS, BOB_HOME "/Settings",
        "/System/Services", "/System/Drivers", "/Programs", "/Programs/setup", "/CIUKILDR", "/System/Registry", "/System/SysBckup",
        "/System/Services/empty"};
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) { int e = make_dir(dirs[i]); CHECK(!e || e == -EEXIST); }
    static const char *const files[] = {"C:" BOB_HOME "/notes.txt", "C:" BOB_HOME "/USER.CRG", "C:" BOB_HOME "/Settings/Registry",
        "C:" BOB_HOME SYSVC_USER_LOGS "/CRASH1.TXT", "C:/System/Security/accounts.dat", "C:/System/VMM.ELF",
        "C:/System/Services/logon.elf", "C:/System/Drivers/ata.sys", "C:/Programs/setup/setup.elf", "C:/CIUKILDR/stage2.bin",
        "C:/BOOT.CFG", "C:/System/PAGEFILE.SWP", "C:/Windows/USER.CRG", "C:/System/SFC.LST", "C:/System/Registry/System.reg",
        "C:/System/SysBckup/RB000001.CRB"};
    for (unsigned i = 0; i < sizeof(files) / sizeof(*files); i++) kernel_file(files[i], "private\n", 8);
}
static void h13_native(void)
{
    h13_layout();
    h13_owner_read_next = owners->io.read; owners->io.read = h13_owner_read;
    struct process *bob_p = bob.p, *system_p = systemp.p;
    struct actor *intruders[] = {&alice, &guest};
    for (unsigned w = 0; w < 2; w++) {
        struct actor *a = intruders[w];
        /* SYSTEM state: SECRET files and folders, read or write. */
        static const char *const secret[] = {"/System/Security/accounts.dat", "/System/Security", "/System/Logs/BOOT.LOG",
            "/System/PAGEFILE.SWP", "/Windows/USER.CRG", "/System/SysBckup/x", "/System/Registry/System.reg"};
        for (unsigned i = 0; i < sizeof(secret) / sizeof(*secret); i++) {
            H13_REFUSED(a, secret[i], h13_open(a, secret[i], O_RDONLY), -EACCES, -ENOENT);
            H13_REFUSED(a, secret[i], h13_open(a, secret[i], O_RDWR | O_CREAT), -EACCES, -EACCES);
        }
        H13_REFUSED(a, "chdir security", h13_path_call(a, CIUKI_SYS_CHDIR, "/System/Security", 0, 0), -EACCES, -EACCES);
        /* call 101: every LOGON-only operation, then SYSTEM's token. */
        static const uint32_t logon_only[] = {CIUKI_SECURITY_SESSION_CREATE, CIUKI_SECURITY_SPAWN_CREDENTIAL,
            CIUKI_SECURITY_SESSION_END, CIUKI_SECURITY_SESSION_LAUNCH, CIUKI_SECURITY_LAUNCH_POLL,
            CIUKI_SECURITY_SECURE_ENTER, CIUKI_SECURITY_SECURE_LEAVE};
        for (unsigned i = 0; i < sizeof(logon_only) / sizeof(*logon_only); i++)
            H13_REFUSED(a, "call 101 LOGON operation", h13_security(a, logon_only[i], 0, 100), -EPERM, -EPERM);
        H13_REFUSED(a, "SYSTEM token", h13_security(a, CIUKI_SECURITY_QUERY_PROCESS, system_p->pid, 0), -EACCES, -EACCES);
        H13_REFUSED(a, "PID 1", h13_proc(a, CIUKI_SYS_PROC_TERMINATE, 1, SIGKILL, CIUKI_PROC_NO_AUTHORITY), -EPERM, -EPERM);
        H13_REFUSED(a, "SYSTEM process", h13_proc(a, CIUKI_SYS_PROC_TERMINATE, system_p->pid, SIGKILL, CIUKI_PROC_NO_AUTHORITY), -EPERM, -EPERM);
        /* call 99: every administrator service. */
        static const struct { uint32_t op, bytes; } admin_ops[] = {{CIUKI_RECOVERY_BOOT_CONFIG, sizeof(struct ciuki_boot_config)},
            {CIUKI_RECOVERY_DRIVER_PACK, sizeof(struct ciuki_driver_pack_job)}, {CIUKI_RECOVERY_SFC_RESTORE, sizeof(struct ciuki_sfc_restore)},
            {CIUKI_RECOVERY_SETTING, sizeof(struct ciuki_system_setting)}};
        for (unsigned i = 0; i < sizeof(admin_ops) / sizeof(*admin_ops); i++)
            H13_REFUSED(a, "call 99 administrator service", h13_recovery(a, admin_ops[i].op, admin_ops[i].bytes), -EPERM, -EINVAL);
        H13_REFUSED(a, "SETTING CLOCK", setting(a, CIUKI_SYSTEM_SETTING_CLOCK, "version=1\nrtc_local=0\n", 22), -EPERM, -EPERM);
        H13_REFUSED(a, "SETTING POWER", setting(a, CIUKI_SYSTEM_SETTING_POWER, "power_lid=none\n", 15), -EPERM, -EPERM);
        H13_REFUSED(a, "system log", log_read(a, CIUKI_SYSTEM_LOG_MACHINE, "BOOT.LOG", (char[96]){0}, 64, 0), -EPERM, -EPERM);
        /* Another user's profile, hive, crash logs, process and token. */
        static const char *const bobs[] = {BOB_HOME "/notes.txt", BOB_HOME "/USER.CRG", BOB_HOME "/Settings/Registry",
            BOB_HOME SYSVC_USER_LOGS "/CRASH1.TXT", BOB_HOME};
        for (unsigned i = 0; i < sizeof(bobs) / sizeof(*bobs); i++) {
            H13_REFUSED(a, bobs[i], h13_open(a, bobs[i], O_RDONLY), -EACCES, -EACCES);
            H13_REFUSED(a, bobs[i], h13_open(a, bobs[i], O_RDWR), -EACCES, -EISDIR);
            H13_REFUSED(a, bobs[i], h13_info(a, bobs[i], CIUKI_FILE_INFO_GET), -EACCES, -EACCES);
            H13_REFUSED(a, bobs[i], h13_info(a, bobs[i], CIUKI_FILE_INFO_SET), -EACCES, -EACCES);
            H13_REFUSED(a, bobs[i], h13_path_call(a, CIUKI_SYS_STAT, bobs[i], 0, USER2), -EACCES, -EACCES);
        }
        H13_REFUSED(a, "create in Bob's profile", h13_open(a, BOB_HOME "/planted.txt", O_RDWR | O_CREAT), -EACCES, -EACCES);
        H13_REFUSED(a, "mkdir in Bob's profile", h13_path_call(a, CIUKI_SYS_MKDIR, BOB_HOME "/planted", 0, 0777), -EACCES, -EACCES);
        H13_REFUSED(a, "unlink in Bob's profile", h13_path_call(a, CIUKI_SYS_UNLINK, BOB_HOME "/notes.txt", 0, 0), -EACCES, -EACCES);
        H13_REFUSED(a, "rename out of Bob's profile", h13_path_call(a, CIUKI_SYS_RENAME, BOB_HOME "/notes.txt", "/Documents/stolen.txt", 0),
                    -EACCES, -EACCES);
        H13_REFUSED(a, "chdir into Bob's profile", h13_path_call(a, CIUKI_SYS_CHDIR, BOB_HOME, 0, 0), -EACCES, -EACCES);
        H13_REFUSED(a, "Bob's process", h13_proc(a, CIUKI_SYS_PROC_TERMINATE, bob_p->pid, SIGKILL, CIUKI_PROC_NO_AUTHORITY), -EPERM, -EPERM);
        H13_REFUSED(a, "Bob's token", h13_security(a, CIUKI_SECURITY_QUERY_PROCESS, bob_p->pid, 0), -EACCES, -EACCES);
        H13_REFUSED(a, "Bob's crash log by name", log_read(a, CIUKI_SYSTEM_LOG_USER, "../../u000003e9/CRASH1.TXT", (char[96]){0}, 64, 0),
                    -EINVAL, -ENOENT);
        /* The sealed boot/service set: write, truncate, remove, rename, metadata. */
        static const char *const sealed[] = {"/System/VMM.ELF", "/System/BOOT.CFG", "/BOOT.CFG", "/CIUKILDR/stage2.bin",
            "/System/Services/logon.elf", "/System/Drivers/ata.sys", "/Programs/setup/setup.elf", "/System/SFC.LST",
            "/System/HWPROF.CFG", "/System/Settings/clock.cfg", "/System/Settings/power.cfg", "/System/Settings/performance.cfg"};
        for (unsigned i = 0; i < sizeof(sealed) / sizeof(*sealed); i++) {
            H13_REFUSED(a, sealed[i], h13_open(a, sealed[i], O_WRONLY), -EACCES, -EACCES);
            H13_REFUSED(a, sealed[i], h13_open(a, sealed[i], O_WRONLY | O_TRUNC), -EACCES, -EACCES);
            H13_REFUSED(a, sealed[i], h13_path_call(a, CIUKI_SYS_UNLINK, sealed[i], 0, 0), -EACCES, -EACCES);
            H13_REFUSED(a, sealed[i], h13_path_call(a, CIUKI_SYS_RENAME, sealed[i], "/Documents/moved.bin", 0), -EACCES, -EACCES);
            H13_REFUSED(a, sealed[i], h13_info(a, sealed[i], CIUKI_FILE_INFO_SET), -EACCES, -EACCES);
        }
        H13_REFUSED(a, "plant in /System/Services", h13_open(a, "/System/Services/evil.elf", O_RDWR | O_CREAT), -EACCES, -EACCES);
        H13_REFUSED(a, "replace /System/Services", h13_path_call(a, CIUKI_SYS_RENAME, "/Documents", "/System/Services", 0), -EACCES, -ENOTEMPTY);
        /* An empty folder of the caller's over an empty sealed folder, and into the sealed set. */
        if (a == &alice) { int32_t e = h13_path_call(a, CIUKI_SYS_MKDIR, "/Documents/mine", 0, 0777); CHECK(!e || e == -EEXIST); }
        H13_REFUSED(a, "replace an empty sealed folder", h13_path_call(a, CIUKI_SYS_RENAME, "/Documents/mine", "/System/Services/empty", 0),
                    -EACCES, -EACCES);
        H13_REFUSED(a, "move into the sealed set", h13_path_call(a, CIUKI_SYS_RENAME, "/Documents/mine", "/System/Services/new", 0),
                    -EACCES, -EACCES);
    }
    /* Unknown selectors of the native dispatchers deny with no side effect:
     * numbers outside every table reach proc_syscall's default (core/syscall.c
     * routes the default case file -> desktop fd -> process). */
    static const uint32_t unknown_nr[] = {6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 92, 93, 94, 95, 102, 103, 255, 0x7fffffffu, 0xffffffffu};
    for (unsigned i = 0; i < sizeof(unknown_nr) / sizeof(*unknown_nr); i++)
        H13_REFUSED(&alice, "unknown syscall", h13_proc(&alice, unknown_nr[i], USER, 64, 0), -ENOSYS, -ENOSYS);
    static const uint32_t unknown_101[] = {0, 15, 16, 255, 0xffffffffu};
    for (unsigned i = 0; i < sizeof(unknown_101) / sizeof(*unknown_101); i++)
        H13_REFUSED(&alice, "unknown call-101 operation", h13_security(&alice, unknown_101[i], 0, 0), -EINVAL, -EINVAL);
    static const uint32_t unknown_99[] = {11, 12, 64, 0x7fffffffu, 0xffffffffu};
    for (unsigned i = 0; i < sizeof(unknown_99) / sizeof(*unknown_99); i++)
        H13_REFUSED(&admin, "unknown call-99 service", h13_recovery(&admin, unknown_99[i], 32), -ENOSYS, -ENOSYS);
    H13_REFUSED(&admin, "unknown SETTING", setting(&admin, 99, "x\n", 2), -EINVAL, -EINVAL);
    H13_REFUSED(&admin, "unknown JOB", job(&admin, 2, 0), -EINVAL, -EINVAL);
    H13_REFUSED(&admin, "unknown LOG action", log_call(&admin, 9, CIUKI_SYSTEM_LOG_MACHINE, "BOOT.LOG", 0, 0, 0, 0, 0, 0), -EINVAL, -EINVAL);
    H13_REFUSED(&admin, "unknown LOG scope", log_call(&admin, CIUKI_SYSTEM_LOG_READ, 7, "BOOT.LOG", 0, 0, 0, 0, 0, 0), -EINVAL, -EINVAL);
    {   /* An unknown device command on a file descriptor, and on a bad one. */
        int32_t fd = sys_open_as(&alice, "/Documents", O_RDONLY); CHECK(fd >= 0);
        H13_REFUSED(&alice, "unknown devctl", call(CIUKI_SYS_DEVCTL, (uint32_t)fd, 0x7e7e7e7eu, USER2, 16, 0), -ENOTTY, -EINVAL);
        sys_close_as(&alice, fd);
        H13_REFUSED(&alice, "devctl bad fd", call(CIUKI_SYS_DEVCTL, 77, 0x7e7e7e7eu, USER2, 16, 0), -EBADF, -EBADF);
    }
    {   /* VFS: unknown access bits and an unknown (corrupt) label kind deny,
         * for every subject including SYSTEM. */
        struct px_path documents; struct policy_label root; CHECK(!resolve_path("/Documents", &documents));
        vfs_lock(&vfs, 0); CHECK(!px_policy_label_locked(documents.node, &root)); vfs_unlock(&vfs);   /* borrowed, not retained */
        struct actor *every[] = {&alice, &guest, &admin, &systemp};
        for (unsigned i = 0; i < 4; i++) {
            CHECK(policy_check(&policy, &root, every[i]->p->subject, 0) == -EACCES);
            CHECK(policy_check(&policy, &root, every[i]->p->subject, 256) == -EACCES);
            CHECK(policy_check(&policy, &root, every[i]->p->subject, POLICY_READ | 0x8000u) == -EACCES);
            struct policy_label corrupt = root; corrupt.kind = POLICY_DENY + 1;
            CHECK(policy_check(&policy, &corrupt, every[i]->p->subject, POLICY_READ) == -EACCES);
            corrupt.kind = 0xff;
            CHECK(policy_check(&policy, &corrupt, every[i]->p->subject, POLICY_READ | POLICY_TRAVERSE) == -EACCES);
            h13_cases += 5;
        }
        /* Every label kind against SYSTEM, an administrator, the owner
         * (Alice), another Standard user (Bob) and Guest, read and write,
         * expected from the contract's rule table (user-security-kernel.md),
         * not from policy_check. 1 allowed, 0 EACCES, 2 EROFS. */
        static const struct { unsigned kind; const char *name; uint8_t read[5], write[5]; } kinds[] = {
            {POLICY_PUBLIC, "POLICY_PUBLIC", {1,1,1,1,1}, {1,1,0,0,0}},
            {POLICY_PROGRAM, "POLICY_PROGRAM", {1,1,1,1,1}, {1,1,0,0,0}},
            {POLICY_SECRET, "POLICY_SECRET", {1,0,0,0,0}, {1,0,0,0,0}},
            {POLICY_SEALED, "POLICY_SEALED", {1,1,1,1,1}, {1,0,0,0,0}},
            {POLICY_PROFILES, "POLICY_PROFILES", {1,1,1,1,1}, {1,0,0,0,0}},
            {POLICY_PROFILE, "POLICY_PROFILE", {1,1,1,0,0}, {1,1,1,0,0}},
            {POLICY_SHARED, "POLICY_SHARED", {1,1,1,1,1}, {1,1,1,1,0}},
            {POLICY_CREATOR, "POLICY_CREATOR", {1,1,1,1,1}, {1,1,1,0,0}},
            {POLICY_REMOVABLE, "POLICY_REMOVABLE", {1,1,1,1,1}, {1,1,1,1,0}},
            {POLICY_READONLY, "POLICY_READONLY", {1,1,1,1,1}, {2,2,2,2,2}},
            {POLICY_DENY, "POLICY_DENY", {0,0,0,0,0}, {0,0,0,0,0}}};
        struct actor *five[] = {&systemp, &admin, &alice, &bob, &guest};
        for (unsigned k = 0; k < sizeof(kinds) / sizeof(*kinds); k++) for (unsigned i = 0; i < 5; i++) {
            struct policy_label l = root; l.kind = (uint8_t)kinds[k].kind; l.uid = 1007; l.anchor = 0; l.owner_key = 7;
            int r = policy_check(&policy, &l, five[i]->p->subject, POLICY_READ), w = policy_check(&policy, &l, five[i]->p->subject, POLICY_WRITE);
            int er = kinds[k].read[i] == 1 ? 0 : -EACCES, ew = kinds[k].write[i] == 1 ? 0 : kinds[k].write[i] == 2 ? -EROFS : -EACCES;
            if (r != er || w != ew) printf("H13 %s %s: read %d (%d) write %d (%d)\n", kinds[k].name, five[i]->name, r, er, w, ew);
            CHECK(r == er && w == ew); h13_cases += 2;
        }
        /* POLICY_ROOT: Standard users add (creator-owned top-level entries,
         * lead decision Q7) but change nothing else; Guest adds nothing. */
        struct policy_label top; vfs_lock(&vfs, 0); CHECK(!px_policy_label_locked(space->root, &top)); vfs_unlock(&vfs);
        CHECK(top.kind == POLICY_ROOT);
        static const int add[5] = {0, 0, 0, 0, -EACCES}, meta[5] = {0, 0, -EACCES, -EACCES, -EACCES};
        for (unsigned i = 0; i < 5; i++) {
            CHECK(policy_check(&policy, &top, five[i]->p->subject, POLICY_READ | POLICY_TRAVERSE) == 0);
            CHECK(policy_check(&policy, &top, five[i]->p->subject, POLICY_ADD) == add[i]);
            CHECK(policy_check(&policy, &top, five[i]->p->subject, POLICY_META) == meta[i]); h13_cases += 3;
        }
    }
    {   /* Public device queries a Guest may make, and their administrative
         * neighbours it may not (NET_QUERY_DRIVER_PACK, MEDIA_GET_FORMAT,
         * MEDIA_GET_CONVERT; CIUKI_SETUP_NET_QUERY, MEDIA_FORMAT). */
        int32_t fd = sys_open_as(&guest, "/dev/net", O_RDONLY); CHECK(fd >= 0);
        struct ciuki_driver_pack_query q = {.version = CIUKI_DRIVER_PACK_VERSION, .size = sizeof(q), .vendor = 0x10ec, .device = 0x8139};
        as(&guest); CHECK(!ua_write(guest.p->memory, USER2, &q, sizeof(q)));
        CHECK(!call(CIUKI_SYS_DEVCTL, (uint32_t)fd, NET_QUERY_DRIVER_PACK, USER2, sizeof(q), 0));
        CHECK(!ua_read(guest.p->memory, &q, USER2, sizeof(q)) && !q.qualified && !q.busy);
        struct ciuki_setup_net sq = {.version = CIUKI_SETUP_FAT_VERSION, .size = sizeof(sq)};
        CHECK(!ua_write(guest.p->memory, USER2, &sq, sizeof(sq)));
        H13_REFUSED(&guest, "CIUKI_SETUP_NET_QUERY", call(CIUKI_SYS_DEVCTL, (uint32_t)fd, CIUKI_SETUP_NET_QUERY, USER2, sizeof(sq), 0), -EPERM, -EPERM);
        H13_REFUSED(&guest, "net unknown command", call(CIUKI_SYS_DEVCTL, (uint32_t)fd, MEDIA_FORMAT, USER2, 16, 0), -ENOTTY, -ENOTTY);
        sys_close_as(&guest, fd);
        fd = sys_open_as(&guest, "/dev/media", O_RDONLY); CHECK(fd >= 0);
        struct ciuki_media_format f = {.version = CIUKI_MEDIA_VERSION, .size = sizeof(f), .drive = 2, .generation = 1};
        struct ciuki_media_convert cv = {.version = CIUKI_MEDIA_VERSION, .size = sizeof(cv), .drive = 2};
        as(&guest); CHECK(!ua_write(guest.p->memory, USER2, &f, sizeof(f)));
        int32_t e = call(CIUKI_SYS_DEVCTL, (uint32_t)fd, MEDIA_GET_FORMAT, USER2, sizeof(f), 0); CHECK(e != -EPERM && e != -EACCES);
        H13_REFUSED(&guest, "MEDIA_FORMAT", call(CIUKI_SYS_DEVCTL, (uint32_t)fd, MEDIA_FORMAT, USER2, sizeof(f), 0), -EPERM, -EPERM);
        CHECK(!ua_write(guest.p->memory, USER2, &cv, sizeof(cv)));
        e = call(CIUKI_SYS_DEVCTL, (uint32_t)fd, MEDIA_GET_CONVERT, USER2, sizeof(cv), 0); CHECK(e != -EPERM && e != -EACCES);
        H13_REFUSED(&guest, "media unknown command", call(CIUKI_SYS_DEVCTL, (uint32_t)fd, 0x4dffu, USER2, 16, 0), -ENOTTY, -ENOTTY);
        sys_close_as(&guest, fd); h13_cases += 2;
    }
    owners->io.read = h13_owner_read_next;
    printf("H13 native: %u refusals (Standard user and Guest: SYSTEM state, Bob's profile/hive/crash log/process/token, sealed boot and service set; unknown selectors of syscalls, call 101, call 99, devctl and VFS access/kind) without fd/description/pin/node/page/heap/subject leaks PASS\n", h13_cases);
}
/* Bounded cost (docs/validation/sec-k08.md): warm descriptor checks perform
 * no storage lookup; the metadata at 32 profiles and 256 creator-owned
 * roots; the cold lookup bound. Figures are printed as "H13 cost" lines. */
static uint64_t h13_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void h13_cost(void)
{
    /* Warm fds: open (cold resolution) once, then 1000 reads and 1000
     * overwrites of cached data by Alice and by an administrator. */
    kernel_file("C:/Documents/warm.txt", "0123456789abcdef0123456789abcdef", 32);
    struct actor *who[] = {&alice, &admin};
    for (unsigned w = 0; w < 2; w++) {
        struct actor *a = who[w];
        int32_t fd = sys_open_as(a, "/Documents/warm.txt", O_RDWR); CHECK(fd >= 0);
        CHECK(call(CIUKI_SYS_PREAD, (uint32_t)fd, USER2, 32, 0, 0) == 32);
        CHECK(call(CIUKI_SYS_PWRITE, (uint32_t)fd, USER2, 32, 0, 0) == 32);
        struct h13_state b, e; h13_snapshot(a, &b);
        uint64_t t0 = h13_now_ns();
        for (unsigned i = 0; i < 1000; i++) {
            as(a);
            CHECK(call(CIUKI_SYS_PREAD, (uint32_t)fd, USER2, 32, 0, 0) == 32);
            CHECK(call(CIUKI_SYS_PWRITE, (uint32_t)fd, USER2, 32, 0, 0) == 32);
        }
        uint64_t t1 = h13_now_ns();
        h13_snapshot(a, &e);
        printf("H13 cost warm-fd %s: 2000 descriptor reads/writes, disk reads %u, OWNER reads %u, %.0f ns each (host)\n",
               a->name, e.disk_reads - b.disk_reads, e.owner_reads - b.owner_reads, (double)(t1 - t0) / 2000.0);
        CHECK(e.disk_reads == b.disk_reads && e.owner_reads == b.owner_reads);
        sys_close_as(a, fd);
    }
    /* Metadata at the bounds: a full store (32 profiles, 256 creator roots)
     * and a full 32-rule trie; cold child resolution scans both at most once. */
    struct owner_store *full = calloc(1, sizeof(*full)); CHECK(full);
    full->volume = 31; full->ready = true; full->profile_count = OWNER_PROFILES; full->root_count = OWNER_ROOTS;
    for (unsigned i = 0; i < OWNER_PROFILES; i++) {
        struct owner_binding *b = &full->profiles[i].binding;
        *b = (struct owner_binding){.uid = 1000 + i, .action = OWNER_CREATE, .volume = 31, .key = 1 + i, .locator = 1 + i, .generation = 1};
        snprintf(b->path, sizeof(b->path), "/Windows/Profiles/u%08x", 1000 + i);
    }
    for (unsigned i = 0; i < OWNER_ROOTS; i++) {
        struct owner_binding *b = &full->roots[i].binding;
        *b = (struct owner_binding){.uid = 1000 + i % 32, .action = OWNER_CREATE, .volume = 31, .key = 100 + i, .locator = 100 + i, .generation = 1};
        snprintf(b->path, sizeof(b->path), "/GAME%03u", i);
    }
    struct policy_volume *v = calloc(1, sizeof(*v)); CHECK(v);
    CHECK(!policy_init(v, 31, 1, true, false, false, full));
    unsigned rules = v->count; char name[64];
    for (unsigned i = 0; v->count < POLICY_RULES; i++) { snprintf(name, sizeof(name), "/Extra%u", i); CHECK(!policy_rule_add(v, name, POLICY_PROGRAM)); }
    snprintf(name, sizeof(name), "/Extra%u", 99); CHECK(policy_rule_add(v, name, POLICY_PROGRAM) == -ENOSPC);
    struct policy_label root; CHECK(!policy_root(v, 1, &root));
    struct policy_label profiles = root, child, label;
    CHECK(!policy_child(v, &root, "Windows", 2, 0, &child)); CHECK(!policy_child(v, &child, "Profiles", 3, 0, &profiles));
    /* Worst cases: the last root record (288 entries scanned) and the last profile. */
    uint64_t t0 = h13_now_ns();
    for (unsigned i = 0; i < 10000; i++) CHECK(!policy_child(v, &root, "GAME255", 9, 100 + 255, &label));
    uint64_t t1 = h13_now_ns();
    CHECK(label.kind == POLICY_CREATOR && label.uid == 1000 + 255 % 32);
    char profile[16]; snprintf(profile, sizeof(profile), "u%08x", 1000 + 31);
    for (unsigned i = 0; i < 10000; i++) CHECK(!policy_child(v, &profiles, profile, 10, 32, &label));
    uint64_t t2 = h13_now_ns();
    CHECK(label.kind == POLICY_PROFILE && label.uid == 1031);
    static volatile int sink;
    for (unsigned i = 0; i < 100000; i++) sink += policy_check(v, &label, alice.p->subject, POLICY_READ);
    uint64_t t3 = h13_now_ns();
    printf("H13 cost metadata: owner_store %zu bytes (profiles %u x %zu, roots %u x %zu, pending %u x %zu), receipts at most %u x %zu bytes, "
           "policy_volume %zu bytes (%u rules, %u installed), policy_label %zu bytes per namespace node (px_node %zu)\n",
           sizeof(*full), OWNER_PROFILES, sizeof(struct owner_record), OWNER_ROOTS, sizeof(struct owner_record), OWNER_PROFILES,
           sizeof(struct owner_record), OWNER_RECEIPTS, sizeof(struct owner_receipt), sizeof(*v), POLICY_RULES, rules,
           sizeof(struct policy_label), sizeof(struct px_node));
    printf("H13 cost cold-lookup: creator root at record 288 %.0f ns, profile 32 %.0f ns, warm policy_check %.1f ns (host, -O1, sanitizers)\n",
           (double)(t1 - t0) / 10000.0, (double)(t2 - t1) / 10000.0, (double)(t3 - t2) / 100000.0);
    printf("H13 cost identity: credential %zu bytes (%u groups), subject %zu bytes, session %zu bytes x %u, process limit %u, thread limit %u\n",
           sizeof(struct ciuki_credential), CIUKI_SECURITY_GROUP_MAX, sizeof(struct security_subject), sizeof(struct security_session),
           CIUKI_SECURITY_SESSION_MAX, CIUKI_PROCESS_MAX, CIUKI_THREAD_MAX);
    free(v); free(full);
}
#endif
/* ---- end sec-k08 ---- */
int main(int argc,char **argv)
{
    CHECK(argc==3); setvbuf(stdout,0,_IONBF,0);
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram); controller.state=T_RUNNING; g_current=&controller; proc_init();
    CHECK(!fake_open(&disk[0],argv[1]) && !fake_open(&disk[1],argv[2])); disk[0].undo_enabled=disk[1].undo_enabled=true;
    CHECK(!cache_init(&cache,0)); vfs_init(&vfs);
    for(unsigned i=0;i<2;i++) { CHECK(!fat_mount(&volumes[i],&cache,&disk[i].dev,0,disk[i].dev.capacity,true)); CHECK(!vfs_attach(&vfs,volume_drive(i),&volumes[i])); }
    CHECK(!files_attach(&vfs,&space)); CHECK(!files_bootstrap(&vfs));
    client_thread=make_process(proc_supervisor(),&client); g_current=client_thread->task; g_current->state=T_RUNNING;
    layout();
    sysvc_attach(&vfs,2);
    identity(&alice,"alice",1007,1,545,0); identity(&bob,"bob",1001,1,545,0); identity(&admin,"admin",1000,2,544,545);
    identity(&guest,"guest",501,1,546,0); identity(&interim,"interim",1000,2,544,545); identity(&systemp,"system",0,0,0,0);
    identity(&service_subject,"service",1002,1,545,0); /* sec-sysvc2 */
    session(&alice,0,ALICE_HOME); session(&bob,1,BOB_HOME); session(&guest,2,GUEST_HOME); session(&interim,3,0);
    bind_policy();
    test_boot_config(); test_machine_logs(); test_user_logs(); test_driver_packs(); test_sfc_restore();
    test_jobs(); test_settings(); /* sec-sysvc2 */
    test_k08_settings(); /* sec-k08 */
#ifdef CIUKI_POLICY_COVERAGE
    h13_native(); h13_cost(); /* sec-k08: H13 native frontend and bounded cost */
#endif
    struct actor *all[]={&alice,&bob,&admin,&guest,&interim,&systemp,&service_subject};
    for(unsigned i=0;i<7;i++) { as(all[i]); for(int fd=0;fd<CIUKI_OPEN_MAX;fd++) if(all[i]->p->fds[fd].object) file_close(all[i]->p,fd); stop_collect(all[i]->p); }
    g_current=client_thread->task; all_closed(); stop_collect(client);
    for(int fd=0;fd<3;fd++) if(file_fd_object(proc_supervisor(),fd)) CHECK(!file_close(proc_supervisor(),fd));
    px_release(proc_supervisor()->cwd); proc_supervisor()->cwd=0;
    struct file_ledger ledger; files_snapshot(space,&ledger);
    if(ledger.descriptions || ledger.pins) printf("ledger descriptions=%u pins=%u\n",ledger.descriptions,ledger.pins);
    CHECK(!ledger.descriptions && !ledger.pins);
    sysvc_attach(0,2);
    vfs_lock(&vfs,0); policy_unbind_locked(&vfs,2); vfs_unlock(&vfs);
    owner_close(owners); free(owners); for(unsigned i=0;i<5;i++) free(owner_files[i].data);
    for(unsigned i=0;i<2;i++) CHECK(!vfs_detach(&vfs,volume_drive(i)));
    vfs_destroy(&vfs); cache_destroy(&cache);
    for(unsigned i=0;i<2;i++) { fake_reset(&disk[i]); fake_close(&disk[i]); }
    kfree(supervisor_task); supervisor_task=0;
    CHECK(!pages_used); free(ram);
    printf("H16 sec-sysvc SYSTEM services: boot configuration, system and user logs, driver packs, SFC restore, sec-sysvc2 jobs and sealed settings, checks=%u PASS\n",checks);
    return 0;
}
