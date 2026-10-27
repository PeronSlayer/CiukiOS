/* sec-int-01 integration: the production account-service kernel adapter
 * (apps/accounts/kernel_adapter.c) and serving loop (apps/accounts/server.c)
 * run against the kernel host harness. Every call-101 operation goes through
 * proc_syscall as the ACCOUNT process; channels, sessions and roles are the
 * kernel's; LOGON and LOGON_UI are kernel-registered roles of the fixture and
 * LOGON acknowledges AUTH_OUTCOME on the ACCOUNT->LOGON link. OWNER and
 * protected storage use the H03 host fake (live open refuses it); the
 * production interim OWNER and native storage are exercised separately.
 * SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_DESKTOP_HOST_MAIN adapter_unused_desktop_main
#include "../proc/desktop_test.c"
#include "accounts.h"
#include "kernel_adapter.h"
#include "kernel_sys.h"
#include "production.h"
#include "server.h"
#include "fake.h"

#define U_ARGS (CIUKI_IMAGE_BASE + PAGE_SIZE + 0x100)
#define U_OUT (U_ARGS + 0x100)
#define U_IN (U_ARGS + 0x200)
#define U_MSG (U_ARGS + 0x300)
#define U_PEER_MSG (U_ARGS + 0x500)

int security_owner_verify(const struct ciuki_security_profile *p) { (void)p; return 0; }

/* kernel_sys.h bound to the harness: the adapter always acts as ACCOUNT. */
static struct proc_thread *account_thread;
static void (*logon_responder)(void);
static unsigned responder_calls, entropy_counter, idle_waits;
static int picture_present=1, boot_flags_error;
static uint32_t boot_flags_value;
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
/* LOGON answers while ACCOUNT waits; without a responder time passes. */
int acct_sys_wait_readable(const int *fds, unsigned count, unsigned timeout_ms)
{
    (void)fds;
    if (logon_responder) { responder_calls++; logon_responder(); return (int)count; }
    idle_waits++; g_ticks+=timeout_ms<10 ? timeout_ms : 10;
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
{ uint8_t *b=buffer; for (size_t i=0;i<bytes;i++) b[i]=(uint8_t)(++entropy_counter*7u); return 0; }
int acct_sys_boot_flags(uint32_t *flags) { if (boot_flags_error) return boot_flags_error; *flags=boot_flags_value; return 0; }
int acct_sys_file_present(const char *path) { return strstr(path,"/System/Security/Pictures/")!=NULL && picture_present; }
void acct_sys_log(const char *line) { printf("  [service] %s\n",line); }

/* Fixture helpers (same shapes as H01). */
static struct proc_thread *thread_of(struct process *p)
{
    for (unsigned i=0;i<CIUKI_THREAD_MAX;i++) if (proc_thread_slot(i) && proc_thread_slot(i)->process==p) return proc_thread_slot(i);
    CHECK(false); return 0;
}
static struct proc_thread *role_fixture(uint32_t role, struct process **out)
{
    CHECK(!proc_prepare(proc_supervisor(),out));
    CHECK(!security_role_prepare(proc_supervisor(),*out,role,security_role_image(role)));
    struct ciuki_file f=fixture_file(); struct proc_strings *s=proc_strings_new();
    CHECK(s && !proc_strings_add(s,"role",5,false)); CHECK(!prepare_image(*out,&f,s,0)); proc_strings_free(s);
    proc_publish(*out,0);
    return thread_of(*out);
}
static struct ciuki_credential standard(uint32_t uid)
{
    return (struct ciuki_credential){ .version=1, .size=64, .uid=uid,
        .primary_gid=545, .group_count=1, .groups={545}, .flags=1 };
}
static struct ciuki_security_profile profile_for(uint32_t uid)
{
    struct ciuki_security_profile p = { .version=1, .size=112, .uid=uid,
        .volume_id_lo=31, .owner_generation_lo=17, .owner_key_lo=23 };
    ksnprintf(p.path, sizeof(p.path), "/Windows/Profiles/u%08x", uid);
    p.path_bytes=(uint32_t)strlen(p.path)+1;
    return p;
}
static struct proc_thread *session_process(struct process *parent, uint32_t id, struct process **out)
{
    CHECK(!proc_prepare(parent,out));
    CHECK(!security_session_attach(*out,id));
    struct ciuki_file f=fixture_file();struct elf_image im;
    CHECK(!elf_validate(&f,&im));CHECK(!elf_load(&f,&im,(*out)->memory));
    struct proc_thread *t;
    CHECK(!proc_thread_prepare(*out,im.entry,0,0,CIUKI_THREAD_STACK_DEFAULT,0,0,true,&t));
    proc_publish(*out,0);return t;
}
static void session_retire(uint32_t id)
{
    g_current=&controller;
    uint32_t state;
    CHECK(!security_session_end(id,true,&state));
    proc_collect();
    CHECK(!security_session_end(id,false,&state) && state==3);
}
static int peer_call(struct proc_thread *t, uint32_t op, const struct ciuki_security_link *in, struct ciuki_security_link *out)
{
    desktop_select(t);
    CHECK(!ua_write(t->process->memory,U_IN,in,64));
    struct ciuki_security_args a={.version=1,.size=64,.operation=op,.spawn_address=U_IN};
    if (op!=14) { a.output_address=U_OUT; a.output_bytes=64; }
    CHECK(!ua_write(t->process->memory,U_ARGS,&a,64));
    struct trap_frame tf={.eax=101,.ebx=U_ARGS,.ecx=64};
    int err=proc_syscall(&tf);
    if (err) return err;
    CHECK(!ua_read(t->process->memory,&a,U_ARGS,64));
    if (out) CHECK(!ua_read(t->process->memory,out,U_OUT,64));
    return op==14 ? (int)a.result : 0;
}
static struct ciuki_security_link peer_connect(struct proc_thread *t, uint32_t service)
{
    struct ciuki_security_link in={.version=1,.size=64,.service=service},out;
    CHECK(!peer_call(t,12,&in,&out)); return out;
}
static uint64_t frame_generation; /* request header u64@40 */
static int peer_send(struct proc_thread *t, const struct ciuki_security_link *l, uint32_t op, uint64_t id, uint32_t flags,
                     const void *body, uint32_t total, uint32_t offset)
{
    uint32_t bytes=total-offset; if (bytes>192) bytes=192;
    struct ciuki_message m={.length=64+bytes};
    uint32_t h[16]={0x31435343,1,64,l->service,op,flags,(uint32_t)id,(uint32_t)(id>>32),l->endpoint_generation,bytes,
                    flags==1 ? (uint32_t)frame_generation : 0,flags==1 ? (uint32_t)(frame_generation>>32) : 0,0,total,offset,0};
    memcpy(m.data,h,64); if (bytes) memcpy(m.data+64,(const uint8_t *)body+offset,bytes);
    desktop_select(t); CHECK(!ua_write(t->process->memory,U_PEER_MSG,&m,sizeof(m)));
    return channel_send(t->process,l->endpoint_fd,U_PEER_MSG,DONTWAIT);
}
static int peer_recv(struct proc_thread *t, int fd, struct ciuki_message *m)
{
    desktop_select(t);
    int r=channel_recv(t->process,fd,U_PEER_MSG,DONTWAIT);
    if (r==1) CHECK(!ua_read(t->process->memory,m,U_PEER_MSG,sizeof(*m)));
    return r;
}
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v,p,4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p,&v,4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p,&v,8); }
/* Collects one complete reply on the client end; returns its status. */
static int peer_reply(struct proc_thread *t, int fd, uint8_t *reply, uint32_t *reply_bytes)
{
    uint32_t got=0, expected=0; int status=0; struct ciuki_message m;
    for (;;) {
        int r=peer_recv(t,fd,&m);
        if (r!=1) return r ? r : -EPIPE;
        const uint8_t *h=m.data;
        CHECK(rd32(h+20)==2 && rd32(h+56)==got);
        status=(int32_t)rd32(h+48); expected=rd32(h+52);
        if (reply && rd32(h+36)) memcpy(reply+got,h+64,rd32(h+36));
        got+=rd32(h+36);
        if (got>=expected) break;
    }
    if (reply_bytes) *reply_bytes=got;
    return status;
}
/* Sends a whole request, lets ACCOUNT serve it and collects the reply. */
static int peer_exchange(struct proc_thread *t, const struct ciuki_security_link *l, uint32_t op, uint64_t id,
                         const void *body, uint32_t total, uint8_t *reply, uint32_t *reply_bytes,
                         void (*serve)(void))
{
    for (uint32_t offset=0;;offset+=192) {
        CHECK(!peer_send(t,l,op,id,1,body,total,offset));
        if (offset+192>=total) break;
    }
    serve();
    return peer_reply(t,l->endpoint_fd,reply,reply_bytes);
}

static struct acct_server acct_srv, prod_srv;
static struct fake fake;
static struct proc_thread *logon, *ui;
static const char *storage_dir, *production_dir;

/* The ACCOUNT side through the production loop: drain every registered
 * endpoint, then run queued work (reply delivery included). */
static void serve_once(void)
{
    int fds[ACCT_KERNEL_LINKS]; unsigned n=acct_kernel_adapter_fds(&acct_srv.kernel,fds,ACCT_KERNEL_LINKS);
    for (unsigned i=0;i<n;i++) acct_server_serve(&acct_srv,fds[i]);
    (void)acct_server_work(&acct_srv,250);
}
static void serve_production(void)
{
    acct_server_accept(&prod_srv);
    int fds[ACCT_KERNEL_LINKS]; unsigned n=acct_kernel_adapter_fds(&prod_srv.kernel,fds,ACCT_KERNEL_LINKS);
    for (unsigned i=0;i<n;i++) acct_server_serve(&prod_srv,fds[i]);
}
/* LOGON accepts the private ACCOUNT->LOGON link and acknowledges AUTH_OUTCOME. */
static uint8_t last_outcome[304]; static unsigned outcomes;
static struct ciuki_security_link logon_kept; static int logon_have;
static void logon_outcome_responder(void)
{
    /* LOGON's backlog also holds LOGON_UI links; this fixture keeps only the
     * ACCOUNT->LOGON link and closes the others. */
    for (;;) {
        struct ciuki_security_link in={.version=1,.size=64,.service=5},accepted;
        int e=peer_call(logon,13,&in,&accepted);
        if (e) { CHECK(e==-EAGAIN); break; }
        CHECK(accepted.server_role==1);
        if (accepted.peer_role==3) {
            if (logon_have) { desktop_select(logon); (void)desktop_close(logon->process,logon_kept.endpoint_fd); }
            logon_kept=accepted; logon_have=1;
        } else { desktop_select(logon); CHECK(!desktop_close(logon->process,accepted.endpoint_fd)); }
    }
    CHECK(logon_have && logon_kept.peer_role==3);
    struct ciuki_message m; uint32_t got=0,total=0;
    for (;;) {
        int r=peer_recv(logon,logon_kept.endpoint_fd,&m); CHECK(r==1);
        uint32_t bytes=rd32(m.data+36), offset=rd32(m.data+56); total=rd32(m.data+52);
        CHECK(rd32(m.data+16)==1 && rd32(m.data+20)==1 && offset==got && total==304);
        memcpy(last_outcome+offset,m.data+64,bytes); got+=bytes;
        if (got==total) break;
    }
    uint64_t id=(uint64_t)rd32(m.data+24)|((uint64_t)rd32(m.data+28)<<32);
    CHECK(!peer_send(logon,&logon_kept,1,id,2,NULL,0,0));
    outcomes++;
}

static void test_storage_posix(void)
{
    struct acct_posix_io s; struct acct_io io; uint8_t b[64]; size_t n=0;
    CHECK(acct_io_posix(&s,"relative",&io)==-EINVAL && acct_io_posix(&s,"/x/",&io)==-EINVAL);
    CHECK(!acct_io_posix(&s,storage_dir,&io) && io.real);
    CHECK(!io.write(io.ctx,"a.db","hello",5) && !io.read(io.ctx,"a.db",b,sizeof(b),&n) && n==5 && !memcmp(b,"hello",5));
    CHECK(!io.flush(io.ctx,"a.db") && !io.barrier(io.ctx) && !s.dirty_count);
    CHECK(io.read(io.ctx,"a.db",b,4,&n)==-EIO && io.read(io.ctx,"missing",b,4,&n)==-ENOENT);
    char path[256], outside[256];
    static const char *const bad[]={".hidden","x/y","../x","",0};
    for (unsigned i=0;bad[i];i++) CHECK(io.write(io.ctx,bad[i],"x",1)==-EINVAL);
    snprintf(path,sizeof(path),"%s/",storage_dir); CHECK(io.write(io.ctx,path,"x",1)==-EINVAL);
    snprintf(path,sizeof(path),"%s//x",storage_dir); CHECK(io.write(io.ctx,path,"x",1)==-EINVAL);
    snprintf(path,sizeof(path),"%s/Staging/../x",storage_dir); CHECK(io.write(io.ctx,path,"x",1)==-EINVAL);
    snprintf(outside,sizeof(outside),"%sx/y",storage_dir); CHECK(io.write(io.ctx,outside,"x",1)==-EINVAL);
    /* The OWNER staging tree: absolute names below the base, parents created
     * and committed by the next barrier. */
    snprintf(path,sizeof(path),"%s/Staging/g%016llx/manifest",storage_dir,1ull);
    CHECK(!io.write(io.ctx,path,"stage",5) && s.dirty_count==2);
    CHECK(!io.read(io.ctx,path,b,sizeof(b),&n) && n==5 && !io.flush(io.ctx,path) && !io.barrier(io.ctx) && !s.dirty_count);
    CHECK(!io.rename(io.ctx,"a.db","b.db") && !io.write(io.ctx,"a.db","again",5));
    CHECK(io.rename(io.ctx,"a.db","b.db")==-EEXIST && io.rename(io.ctx,"nothing","c.db")==-ENOENT);
    CHECK(!io.read(io.ctx,"b.db",b,sizeof(b),&n) && n==5 && !memcmp(b,"hello",5));
    CHECK(!io.remove(io.ctx,"a.db") && io.remove(io.ctx,"a.db")==-ENOENT && !io.remove(io.ctx,"b.db") && !io.remove(io.ctx,path));
    for (unsigned i=0;i<=ACCT_IO_DIRTY;i++) {
        snprintf(path,sizeof(path),"%s/Staging/g%016llx/manifest",storage_dir,(unsigned long long)(i+2));
        CHECK(!io.write(io.ctx,path,"x",1) && !io.remove(io.ctx,path));
    }
    CHECK(s.overflow && io.barrier(io.ctx)==-EIO && !io.barrier(io.ctx));
    puts("sec-int-01 storage: protected names only, staging parents created, data/directory barriers, no-replace rename, overflow fails closed PASS");
}
static void test_production_tables(void)
{
    struct acct_owner owner; acct_owner_interim(&owner);
    char root[64]; struct acct_owner_binding binding; uint64_t g; uint8_t digest[32]; unsigned count;
    struct acct_owner_request q={0}; uint8_t buffer[64]; size_t bytes;
    CHECK(owner.real && !owner.authenticate(owner.ctx) && owner.generation(owner.ctx,&g)==-ENOSYS);
    CHECK(owner.binding(owner.ctx,1,1000,root,&binding)==-ENOSYS && owner.root_occupied(owner.ctx,1,"/Windows/Profiles/u000003e8")==-ENOSYS);
    CHECK(owner.stage(owner.ctx,&q,buffer,sizeof(buffer),&bytes)==-ENOSYS && owner.tree_digest(owner.ctx,1,1000,digest)==-ENOSYS);
    CHECK(owner.validate_stage(owner.ctx,&q)==-ENOSYS && owner.snapshot(owner.ctx,NULL,0,0,buffer,sizeof(buffer),&bytes,&g)==-ENOSYS);
    CHECK(owner.recover_orphans(owner.ctx,1,0)==-ENOSYS && owner.checkpoint_floor(owner.ctx,&g,&count)==-ENOSYS);
    CHECK(owner.call(owner.ctx,buffer,sizeof(buffer),buffer,sizeof(buffer),&bytes)==-ENOSYS);
    /* main.c's exact tables: real kernel, interim OWNER, native storage. */
    static struct acct_posix_io io_state; struct acct_store tables={0};
    tables.owner=owner; tables.volume=ACCT_INTERIM_VOLUME;
    CHECK(!acct_io_posix(&io_state,production_dir,&tables.io) && !acct_server_init(&prod_srv,&tables,0));
    CHECK(!acct_server_open(&prod_srv,0) && prod_srv.last_open_error==-EIO && !prod_srv.kernel.leased);
    CHECK(prod_srv.next_open==ACCT_SERVER_OPEN_RETRY_MS && !acct_server_open(&prod_srv,ACCT_SERVER_OPEN_RETRY_MS-1));
    /* An unopened service answers every complete request with EIO. */
    struct ciuki_security_link c=peer_connect(ui,1); uint32_t got=9;
    CHECK(peer_exchange(ui,&c,1,1,NULL,0,NULL,&got,serve_production)==-EIO && !got);
    /* Protected UNPROVISIONED evidence lets recovery reach OWNER: ENOSYS. */
    struct acct_bootstrap b={0}; uint8_t state[160];
    b.state=ACCT_UNPROVISIONED; b.origin=ACCT_READY; b.volume=ACCT_INTERIM_VOLUME; b.next_uid=1001; b.generation=1;
    memset(b.seed_digest,7,32);
    CHECK(!acct_boot_encode(&b,state,160) && !tables.io.write(&io_state,"bootstrap.state",state,160));
    CHECK(!tables.io.flush(&io_state,"bootstrap.state") && !tables.io.barrier(&io_state));
    boot_flags_value=CIUKI_BOOT_SAFE_MODE;
    CHECK(!acct_server_open(&prod_srv,ACCT_SERVER_OPEN_RETRY_MS) && prod_srv.last_open_error==-ENOSYS);
    CHECK(prod_srv.kernel.safe_mode && !prod_srv.kernel.leased && !prod_srv.opened);
    boot_flags_value=0; boot_flags_error=-EFAULT; /* Safe Mode cannot be attested: fail closed */
    CHECK(!acct_server_open(&prod_srv,3*ACCT_SERVER_OPEN_RETRY_MS) && prod_srv.last_open_error==-EFAULT);
    boot_flags_error=0;
    CHECK(peer_exchange(ui,&c,1,2,NULL,0,NULL,&got,serve_production)==-EIO && !got);
    desktop_select(ui); CHECK(!desktop_close(ui->process,c.endpoint_fd));
    serve_production(); /* the closed link is retired */
    CHECK(!acct_kernel_adapter_fds(&prod_srv.kernel,(int[ACCT_KERNEL_LINKS]){0},ACCT_KERNEL_LINKS));
    acct_server_close(&prod_srv);
    for (unsigned i=0;i<ACCT_KERNEL_LINKS;i++) CHECK(!prod_srv.kernel.links[i].occupied);
    puts("sec-int-01 production tables: interim OWNER refuses every owner operation (ENOSYS), live open fails closed (EIO without evidence, ENOSYS with it, boot flags unreadable), EIO status replies PASS");
}
static void test_lease_and_gate(void)
{
    struct acct_store store=fake_store(&fake);
    CHECK(!acct_server_init(&acct_srv,&store,1));
    struct acct_kernel *k=&acct_srv.tables.kernel;
    CHECK(k->real && k->ctx==&acct_srv.kernel.binding && k->same_link==ciuki_accounts_same_link);
    /* Live open still refuses the host OWNER/storage fakes even with the real kernel binding. */
    CHECK(acct_service_open(&acct_srv.service,&acct_srv.tables,0)==-ENOSYS);
    CHECK(!acct_server_open(&acct_srv,0) && acct_srv.last_open_error==-EIO); /* no evidence is not fresh */
    fake_provision(&fake,1);
    CHECK(acct_server_open(&acct_srv,ACCT_SERVER_OPEN_RETRY_MS) && acct_srv.opened);
    CHECK(acct_srv.kernel.leased && acct_srv.kernel.pid==account_thread->process->pid && acct_srv.kernel.generation==1);
    CHECK(!acct_srv.kernel.safe_mode);
    struct acct_service other;
    CHECK(acct_service_open(&other,&acct_srv.tables,1)==-EBUSY);
    /* sec-user-02: the server's first-boot adapter promoted the staged seed
     * during open; a second explicit bootstrap is refused. */
    struct acct_db seed; fake_seed(&seed,1); uint8_t bytes[ACCT_FILE_MAX]; size_t n;
    CHECK(acct_srv.firstboot_result==ACCT_FIRSTBOOT_PROMOTED);
    CHECK(!acct_encode(&seed,1,bytes,sizeof(bytes),&n)); CHECK(acct_service_bootstrap(&acct_srv.service,bytes,n,ACCT_READY)==-EPERM);
    CHECK(acct_srv.service.available && acct_srv.service.db.count==3);
    k=&acct_srv.service.store.kernel;
    uint32_t lg=9,ag=9; CHECK(!k->role_generations(k->ctx,&lg,&ag) && lg==1 && ag==1);
    CHECK(acct_srv.kernel.logon.occupied && acct_srv.kernel.logon.peer_pid==logon->process->pid);
    CHECK(k->active_uid(k->ctx,1000)==0 && k->active_uid(k->ctx,CIUKI_UID_INVALID)==-EINVAL);
    CHECK(k->picture_valid(k->ctx,1000,"") && k->picture_valid(k->ctx,1000,"catalogue/ciuki.png"));
    picture_present=0; CHECK(!k->picture_valid(k->ctx,1000,"catalogue/ciuki.png")); picture_present=1;
    CHECK(!k->picture_valid(k->ctx,1000,"../x.png"));
    uint32_t c,a,u; CHECK(k->gate_state(k->ctx,0,&c,&a,&u)==-EPERM);
    uint8_t entropy[16]={0}; CHECK(!k->entropy(k->ctx,entropy,16) && entropy[0]);
    puts("sec-int-01 adapter: lease over QUERY_SELF/self-connect, server open/retry, bootstrap through the real kernel binding, role generations, active uid, pictures PASS");
}
static void test_retained_subjects(void)
{
    struct acct_kernel *k=&acct_srv.service.store.kernel;
    struct ciuki_security_link client=peer_connect(ui,1);
    int fd=-1; CHECK(!acct_kernel_adapter_accept(&acct_srv.kernel,1,&fd) && fd>=0);
    int none=-1; CHECK(acct_kernel_adapter_accept(&acct_srv.kernel,1,&none)==-EAGAIN && acct_kernel_adapter_accept(&acct_srv.kernel,2,&none)==-EAGAIN);
    struct acct_subject a={0},b={0},c={0};
    CHECK(!k->retain_peer(k->ctx,fd,&a) && a.retained && a.role==2 && !a.uid && !a.session_id && a.service==1 &&
          a.endpoint_generation==client.endpoint_generation && a.service_generation==1 && a.group_count==1 && !a.groups[0]);
    CHECK(k->retain_peer(k->ctx,fd+100,&b)==-EBADF);
    CHECK(!k->retain_peer(k->ctx,fd,&b) && b.retained!=a.retained);
    CHECK(k->same_link(k->ctx,&a,&b)==1 && k->same_link(k->ctx,&b,&a)==1 && k->same_link(k->ctx,&a,&a)==1);
    CHECK(k->peer_alive(k->ctx,&a)==1 && k->peer_alive(k->ctx,&b)==1);
    struct ciuki_security_link other=peer_connect(logon,1);
    int fd2=-1; CHECK(!acct_kernel_adapter_accept(&acct_srv.kernel,1,&fd2) && fd2!=fd);
    CHECK(!k->retain_peer(k->ctx,fd2,&c) && c.role==1 && c.role_generation==1 && c.endpoint_generation==other.endpoint_generation);
    CHECK(k->same_link(k->ctx,&a,&c)==0 && k->same_link(k->ctx,&c,&b)==0);
    /* Copied metadata is no identity: equal visible fields on another link. */
    struct acct_subject forged=c; forged.endpoint_generation=a.endpoint_generation; forged.role=a.role;
    CHECK(k->same_link(k->ctx,&forged,&a)==0);
    forged=a; forged.retained=0x12345678;
    CHECK(k->same_link(k->ctx,&forged,&a)==-EACCES && k->peer_alive(k->ctx,&forged)==0);
    forged.retained=(uintptr_t)1<<20|1; CHECK(k->same_link(k->ctx,&forged,&a)==-EACCES);
    forged.retained=0; CHECK(k->same_link(k->ctx,&forged,&a)==-EACCES);
    CHECK(ciuki_accounts_same_link(NULL,&a,&b)==-EIO);
    k->subject_get(k->ctx,&a); k->release_peer(k->ctx,&b);
    CHECK(k->same_link(k->ctx,&a,&a)==1);
    desktop_select(ui); CHECK(!desktop_close(ui->process,client.endpoint_fd));
    CHECK(k->peer_alive(k->ctx,&a)==0 && k->same_link(k->ctx,&a,&a)==-ESRCH && k->same_link(k->ctx,&a,&c)==-ESRCH);
    CHECK(acct_kernel_adapter_link_live(&acct_srv.kernel,fd)==0 && acct_kernel_adapter_link_live(&acct_srv.kernel,fd2)==1);
    /* Retirement waits for the last reference; the link leaves the wait set. */
    CHECK(acct_kernel_adapter_retire(&acct_srv.kernel,fd)==-EBUSY);
    int fds[ACCT_KERNEL_LINKS]; CHECK(acct_kernel_adapter_fds(&acct_srv.kernel,fds,ACCT_KERNEL_LINKS)==1 && fds[0]==fd2);
    CHECK(k->retain_peer(k->ctx,fd,&b)==-ESRCH);
    struct acct_subject copy=a; k->release_peer(k->ctx,&a);
    CHECK(acct_kernel_adapter_link_live(&acct_srv.kernel,fd)==0);
    k->release_peer(k->ctx,&copy);
    CHECK(acct_kernel_adapter_link_live(&acct_srv.kernel,fd)==-EBADF && acct_kernel_adapter_retire(&acct_srv.kernel,fd)==-EBADF);
    /* Descriptor reuse: a new link on the same number is a different identity. */
    struct ciuki_security_link reused=peer_connect(ui,1);
    int fd3=-1; CHECK(!acct_kernel_adapter_accept(&acct_srv.kernel,1,&fd3) && fd3==fd && reused.endpoint_generation!=client.endpoint_generation);
    struct acct_subject d={0}; CHECK(!k->retain_peer(k->ctx,fd3,&d) && d.endpoint_generation==reused.endpoint_generation);
    CHECK(k->same_link(k->ctx,&d,&c)==0 && k->same_link(k->ctx,&d,&d)==1);
    k->release_peer(k->ctx,&d); k->release_peer(k->ctx,&c);
    desktop_select(ui); CHECK(!desktop_close(ui->process,reused.endpoint_fd));
    desktop_select(logon); CHECK(!desktop_close(logon->process,other.endpoint_fd));
    acct_server_sweep(&acct_srv);
    CHECK(acct_kernel_adapter_link_live(&acct_srv.kernel,fd3)==-EBADF && acct_kernel_adapter_link_live(&acct_srv.kernel,fd2)==-EBADF);
    puts("sec-int-01 adapter: retain/get/release leases, same_link through the kernel link identity (two handles, different links, forged handles/metadata, revocation, deferred retirement, descriptor reuse) PASS");
}
static void test_list_exchange(void)
{
    struct ciuki_security_link client=peer_connect(ui,1);
    acct_server_accept(&acct_srv);
    uint8_t reply[16+144*ACCT_MAX]; uint32_t bytes=0;
    CHECK(peer_exchange(ui,&client,1,1,NULL,0,reply,&bytes,serve_once)==0);
    CHECK(bytes==160 && rd32(reply)==1 && rd32(reply+4)==1 && rd32(reply+16)==1000 && !strcmp((const char *)reply+32,"CiukiUser"));
    for (unsigned i=0;i<ACCT_KERNEL_LINKS;i++) CHECK(!acct_srv.kernel.links[i].active_request);
    /* A replayed request id is refused by the kernel before admission. */
    CHECK(peer_send(ui,&client,1,1,1,NULL,0,0)==-EINVAL);
    CHECK(peer_exchange(ui,&client,1,2,NULL,0,reply,&bytes,serve_once)==0 && bytes==160);
    /* ACCOUNTS endpoint from an ordinary SYSTEM-credential process: LIST is public. */
    struct process *user; struct proc_thread *ut=make_process(logon->process,&user);
    struct ciuki_security_link uc=peer_connect(ut,2);
    acct_server_accept(&acct_srv);
    CHECK(peer_exchange(ut,&uc,6,1,NULL,0,reply,&bytes,serve_once)==0 && bytes==160);
    /* Requests the service cannot admit get a status-only reply from the
     * serving loop: malformed UPDATE (no selector), then a password change
     * for another uid. */
    uint8_t body[312]={0}; wr32(body,1); wr32(body+4,9); wr32(body+36,1000); wr64(body+40,acct_srv.service.db.generation);
    strcpy((char *)body+144,"password1"); wr32(body+32,9); wr64(body+304,7);
    frame_generation=acct_srv.service.db.generation;
    CHECK(peer_exchange(ut,&uc,4,2,body,312,reply,&bytes,serve_once)==-EINVAL && bytes==0);
    wr32(body+4,0);
    CHECK(peer_exchange(ut,&uc,2,3,body,312,reply,&bytes,serve_once)==-EPERM && bytes==0);
    frame_generation=0;
    desktop_select(ut); CHECK(!desktop_close(user,uc.endpoint_fd));
    stop_collect(user);
    desktop_select(ui); CHECK(!desktop_close(ui->process,client.endpoint_fd));
    acct_server_sweep(&acct_srv);
    CHECK(!acct_kernel_adapter_fds(&acct_srv.kernel,(int[ACCT_KERNEL_LINKS]){0},ACCT_KERNEL_LINKS));
    puts("sec-int-01 adapter: LIST over AUTH and ACCOUNTS links through kernel framing and the serving loop, replay refusal, EINVAL/EPERM status-only replies, link retirement PASS");
}
/* Retained LOGON subject on a fresh AUTH link. */
static int retained_logon(struct ciuki_security_link *l, struct acct_subject *s)
{
    struct acct_kernel *k=&acct_srv.service.store.kernel;
    *l=peer_connect(logon,1);
    int fd=-1; CHECK(!acct_kernel_adapter_accept(&acct_srv.kernel,1,&fd));
    CHECK(!k->retain_peer(k->ctx,fd,s) && s->role==1);
    return fd;
}
static void test_sessions(void)
{
    struct acct_kernel *k=&acct_srv.service.store.kernel;
    struct ciuki_security_link l; struct acct_subject p;
    int fd=retained_logon(&l,&p);
    uint32_t locked=7, lg=0, ag=0; CHECK(!k->role_generations(k->ctx,&lg,&ag));
    CHECK(!k->enrollment_authorized(k->ctx,&p) && !k->active_uid(k->ctx,1001));
    CHECK(!k->auth_binding(k->ctx,&p,1,0,1,1,lg,ag,&locked) && locked==CIUKI_UID_INVALID);
    CHECK(k->auth_binding(k->ctx,&p,1,0,1,1,lg+1,ag,&locked)==-EAGAIN && k->auth_binding(k->ctx,&p,1,0,1,0,lg,ag,&locked)==-EAGAIN);
    /* A session without a process cannot be attributed: busy for every uid. */
    struct ciuki_credential c=standard(1001); struct ciuki_security_profile profile=profile_for(1001); uint32_t id=0;
    g_current=&controller; CHECK(!security_session_create(&c,&profile,&id) && id);
    CHECK(k->active_uid(k->ctx,1001)==1 && k->active_uid(k->ctx,1002)==1 && k->enrollment_authorized(k->ctx,&p)==-EACCES);
    CHECK(!k->auth_binding(k->ctx,&p,1,id,1,1,lg,ag,&locked) && locked==CIUKI_UID_INVALID);
    struct process *owner; (void)session_process(logon->process,id,&owner);
    CHECK(k->active_uid(k->ctx,1001)==1 && k->active_uid(k->ctx,1002)==0 && k->active_uid(k->ctx,1000)==0);
    CHECK(!k->auth_binding(k->ctx,&p,1,id,1,1,lg,ag,&locked) && locked==1001);
    CHECK(k->auth_binding(k->ctx,&p,1,id+1,1,1,lg,ag,&locked)==-EAGAIN && locked==CIUKI_UID_INVALID);
    /* Only LOGON is attested; a UI subject is refused. */
    struct ciuki_security_link uil=peer_connect(ui,1); int ufd=-1; struct acct_subject us;
    CHECK(!acct_kernel_adapter_accept(&acct_srv.kernel,1,&ufd) && !k->retain_peer(k->ctx,ufd,&us));
    CHECK(k->auth_binding(k->ctx,&us,1,0,1,1,lg,ag,&locked)==-EAGAIN && k->enrollment_authorized(k->ctx,&us)==-EACCES);
    CHECK(k->session_lock(k->ctx,&us,id)==-EACCES);
    k->release_peer(k->ctx,&us);
    desktop_select(ui); CHECK(!desktop_close(ui->process,uil.endpoint_fd));
    /* A CLOSING session is not unlockable; DEAD sessions are not active. */
    uint32_t state; g_current=&controller; CHECK(!security_session_end(id,true,&state) && state==2);
    CHECK(k->auth_binding(k->ctx,&p,1,id,1,1,lg,ag,&locked)==-EAGAIN && k->active_uid(k->ctx,1001)==1);
    proc_collect(); CHECK(!security_session_end(id,false,&state) && state==3);
    CHECK(!k->active_uid(k->ctx,1001) && !k->enrollment_authorized(k->ctx,&p));
    k->release_peer(k->ctx,&p);
    desktop_select(logon); CHECK(!desktop_close(logon->process,l.endpoint_fd));
    CHECK(!acct_kernel_adapter_retire(&acct_srv.kernel,fd) && !acct_kernel_adapter_retire(&acct_srv.kernel,ufd));
    puts("sec-int-01 adapter: session attestation (active uid, conservative unattributed sessions, enrollment only without sessions, UNLOCK owner from the session's processes, CLOSING/DEAD, LOGON-only) PASS");
}
static uint64_t next_attempt=1;
static int challenge(const struct ciuki_security_link *l, uint64_t id, const struct acct_begin *begin,
                     uint32_t ui_generation, struct acct_challenge *issued)
{
    uint8_t request[128], reply[64]; uint32_t bytes=0;
    struct acct_challenge proposal={.action=begin->action,.attempt=next_attempt++,.session=begin->session,
        .console=begin->console ? begin->console : 1,.authentication_epoch=1,.ui_generation=ui_generation,
        .logon_generation=1,.account_generation=1};
    CHECK(!acct_begin_encode(begin,1,request,64) && !acct_challenge_encode(&proposal,1,request+64,64));
    int status=peer_exchange(logon,l,5,id,request,128,reply,&bytes,serve_once);
    if (!status) CHECK(bytes==64 && !acct_challenge_decode(issued,0,reply,64));
    return status;
}
static int verify(const struct ciuki_security_link *l, uint32_t op, uint64_t id, const struct acct_challenge *issued,
                  const char *name, const char *secret, uint8_t progress[16])
{
    uint8_t request[272]={0}; uint32_t bytes=0;
    CHECK(!acct_challenge_encode(issued,0,request,64));
    uint8_t *s=request+64;
    wr32(s,1); wr64(s+8,issued->attempt); wr32(s+16,issued->session); wr32(s+20,issued->console);
    wr32(s+24,(uint32_t)strlen(name)); wr32(s+28,(uint32_t)strlen(secret)); strcpy((char *)s+48,name); strcpy((char *)s+80,secret);
    wr32(s+36,issued->uid); wr64(s+40,issued->accounts_generation);
    frame_generation=issued->accounts_generation;
    int status=peer_exchange(logon,l,op,id,request,272,progress,&bytes,serve_once);
    frame_generation=0;
    if (!status) CHECK(bytes==16);
    return status;
}
static void test_challenge_and_outcome(void)
{
    struct ciuki_security_link l=peer_connect(logon,1);
    acct_server_accept(&acct_srv);
    struct ciuki_security_link uil=peer_connect(ui,5); /* LOGON_UI->LOGON, whose generation LOGON attests */
    struct acct_begin begin={.action=2,.selector=2}; strcpy(begin.name,"CiukiUser");
    struct acct_challenge issued; uint8_t progress[16];
    CHECK(!challenge(&l,1,&begin,uil.endpoint_generation,&issued) && issued.uid==1000 && issued.attempt==1);
    /* Verification: blank CiukiUser; the outcome goes to LOGON on the private link. */
    logon_responder=logon_outcome_responder;
    CHECK(!verify(&l,2,2,&issued,"CiukiUser","",progress));
    logon_responder=NULL;
    CHECK(outcomes==1 && responder_calls==1 && rd32(progress)==1 && rd32(progress+4)==1 && rd32(progress+8)==0);
    CHECK(rd32(last_outcome+64)==0 && rd32(last_outcome+68)==1 && rd32(last_outcome+104)==1000 && !strcmp((const char *)last_outcome+272,"CiukiUser"));
    /* A stale LOGON generation is refused before the KDF. */
    uint8_t request[128], reply[64]; uint32_t bytes=0;
    struct acct_challenge proposal={.action=2,.attempt=next_attempt++,.console=1,.authentication_epoch=1,
        .ui_generation=uil.endpoint_generation,.logon_generation=2,.account_generation=1};
    CHECK(!acct_challenge_encode(&proposal,1,request+64,64) && !acct_begin_encode(&begin,1,request,64));
    CHECK(peer_exchange(logon,&l,5,3,request,128,reply,&bytes,serve_once)==-EAGAIN);
    /* UNLOCK: the locked owner comes from the kernel session; only its owner
     * can be selected and authenticated. */
    struct ciuki_credential c=standard(1000); struct ciuki_security_profile profile=profile_for(1000); uint32_t id=0;
    g_current=&controller; CHECK(!security_session_create(&c,&profile,&id));
    struct process *owner; (void)session_process(logon->process,id,&owner);
    struct acct_begin unlock={.action=3,.session=id,.console=1,.selector=1,.uid=1000};
    CHECK(!challenge(&l,4,&unlock,uil.endpoint_generation,&issued) && issued.uid==1000 && issued.session==id);
    logon_responder=logon_outcome_responder;
    CHECK(!verify(&l,3,5,&issued,"","",progress));
    logon_responder=NULL;
    CHECK(outcomes==2 && rd32(progress+4)==1 && rd32(last_outcome+68)==1 && rd32(last_outcome+104)==1000 &&
          rd32(last_outcome+96+52)==id);
    session_retire(id);
    c=standard(1001); profile=profile_for(1001);
    g_current=&controller; CHECK(!security_session_create(&c,&profile,&id));
    (void)session_process(logon->process,id,&owner);
    unlock.session=id;
    CHECK(challenge(&l,6,&unlock,uil.endpoint_generation,&issued)==-EACCES);
    session_retire(id);
    desktop_select(ui); CHECK(!desktop_close(ui->process,uil.endpoint_fd));
    desktop_select(logon); CHECK(!desktop_close(logon->process,l.endpoint_fd));
    serve_once(); acct_server_sweep(&acct_srv);
    puts("sec-int-01 adapter: CHALLENGE through auth_binding/role_generations, blank-password AUTH, AUTH_OUTCOME over the ACCOUNT->LOGON link with LOGON acknowledgment, stale generation refused, UNLOCK bound to the kernel session owner PASS");
}
static void test_cancellation_and_death(void)
{
    struct acct_kernel *k=&acct_srv.service.store.kernel;
    uint8_t body[300]={0}; struct ciuki_message m;
    /* The kernel delivers only complete requests: CANCEL before the server
     * copied anything proves removal (1); the loop retires the link. */
    struct ciuki_security_link client=peer_connect(ui,1);
    int fd=-1; CHECK(!acct_kernel_adapter_accept(&acct_srv.kernel,1,&fd));
    CHECK(!peer_send(ui,&client,1,5,1,body,300,0)); /* first fragment only */
    acct_server_serve(&acct_srv,fd);
    for (unsigned i=0;i<8;i++) CHECK(!acct_srv.transport.exchanges[i].occupied);
    struct ciuki_security_link cancel={.version=1,.size=64,.service=1,.endpoint_fd=client.endpoint_fd,
        .endpoint_generation=client.endpoint_generation,.request_id_lo=5};
    CHECK(peer_call(ui,14,&cancel,NULL)==1);
    acct_server_serve(&acct_srv,fd);
    CHECK(acct_kernel_adapter_link_live(&acct_srv.kernel,fd)==-EBADF);
    desktop_select(ui); CHECK(!desktop_close(ui->process,client.endpoint_fd));
    /* Admitted: ACCOUNT holds the first fragment of a complete request, so
     * CANCEL is irrevocable-or-unknown (0); the partial exchange is dropped
     * when the loop sees the revoked link. */
    client=peer_connect(ui,1);
    CHECK(!acct_kernel_adapter_accept(&acct_srv.kernel,1,&fd));
    CHECK(!peer_send(ui,&client,1,5,1,body,300,0) && !peer_send(ui,&client,1,5,1,body,300,192));
    CHECK(acct_sys_channel_recv(fd,&m,0)==1);
    CHECK(acct_frame(&acct_srv.transport,&acct_srv.service,fd,m.data,m.length,&acct_srv.response)==1);
    unsigned partial=0; for (unsigned i=0;i<8;i++) partial+=acct_srv.transport.exchanges[i].occupied;
    CHECK(partial==1);
    cancel.endpoint_fd=client.endpoint_fd; cancel.endpoint_generation=client.endpoint_generation;
    CHECK(peer_call(ui,14,&cancel,NULL)==0);
    /* CANCEL is a client operation; the kernel refuses it on the server end. */
    struct acct_security_link server_cancel={.version=1,.size=64,.service=1,.endpoint_fd=fd,
        .endpoint_generation=client.endpoint_generation,.request_id_lo=5};
    CHECK(k->service_link(k->ctx,ACCT_SERVICE_CANCEL,&server_cancel,NULL)==-EACCES);
    acct_server_serve(&acct_srv,fd); /* ESRCH: exchange dropped, link retired */
    for (unsigned i=0;i<8;i++) CHECK(!acct_srv.transport.exchanges[i].occupied);
    CHECK(acct_kernel_adapter_link_live(&acct_srv.kernel,fd)==-EBADF);
    desktop_select(ui); CHECK(!desktop_close(ui->process,client.endpoint_fd));
    /* Peer death revokes the retained subject. */
    struct process *dying; struct proc_thread *dt=make_process(logon->process,&dying);
    struct ciuki_security_link dc=peer_connect(dt,2);
    int dfd=-1; CHECK(!acct_kernel_adapter_accept(&acct_srv.kernel,2,&dfd));
    struct acct_subject s={0};
    CHECK(!k->retain_peer(k->ctx,dfd,&s) && s.uid==0 && dc.service==2);
    stop_collect(dying);
    CHECK(k->peer_alive(k->ctx,&s)==0 && k->same_link(k->ctx,&s,&s)==-ESRCH && acct_kernel_adapter_link_live(&acct_srv.kernel,dfd)==0);
    acct_server_sweep(&acct_srv); /* deferred while s is held */
    CHECK(acct_kernel_adapter_link_live(&acct_srv.kernel,dfd)==0);
    k->release_peer(k->ctx,&s);
    CHECK(acct_kernel_adapter_link_live(&acct_srv.kernel,dfd)==-EBADF);
    puts("sec-int-01 adapter: pre-admission CANCEL proven (1) and admitted CANCEL irrevocable (0) with the partial exchange dropped and the link retired, server-side CANCEL refused by the kernel, peer death revokes retained subjects PASS");
}
static void test_step_and_timeout(void)
{
    /* The image's loop pass: accept, serve, then work on the next pass. */
    struct ciuki_security_link client=peer_connect(ui,1);
    CHECK(!peer_send(ui,&client,1,1,1,NULL,0,0));
    acct_server_step(&acct_srv,250); acct_server_step(&acct_srv,250);
    uint8_t reply[16+144*ACCT_MAX]; uint32_t bytes=0;
    CHECK(!peer_reply(ui,client.endpoint_fd,reply,&bytes) && bytes==160);
    /* LOGON never acknowledges: ACCOUNT gives up after the service timeout,
     * drops its LOGON link and keeps serving new links. */
    struct ciuki_security_link l=peer_connect(logon,1);
    struct ciuki_security_link uil=peer_connect(ui,5);
    acct_server_accept(&acct_srv);
    struct acct_begin begin={.action=2,.selector=2}; strcpy(begin.name,"CiukiUser");
    struct acct_challenge issued; uint8_t progress[16];
    g_ticks+=1000; /* the refused UNLOCK above started the one-second backoff */
    CHECK(!challenge(&l,1,&begin,uil.endpoint_generation,&issued));
    unsigned waits=idle_waits; uint64_t start=g_ticks;
    CHECK(verify(&l,2,2,&issued,"CiukiUser","",progress)<0);
    CHECK(idle_waits>waits && g_ticks-start>=CIUKI_SECURITY_SERVICE_TIMEOUT_MS && !acct_srv.kernel.logon.occupied);
    CHECK(!acct_srv.service.faulted);
    desktop_select(ui); CHECK(!desktop_close(ui->process,uil.endpoint_fd) && !desktop_close(ui->process,client.endpoint_fd));
    desktop_select(logon); (void)desktop_close(logon->process,l.endpoint_fd);
    if (logon_have) { (void)desktop_close(logon->process,logon_kept.endpoint_fd); logon_have=0; }
    struct ciuki_security_link fresh=peer_connect(ui,1);
    acct_server_accept(&acct_srv);
    CHECK(peer_exchange(ui,&fresh,1,1,NULL,0,reply,&bytes,serve_once)==0 && bytes==160);
    desktop_select(ui); CHECK(!desktop_close(ui->process,fresh.endpoint_fd));
    serve_once(); acct_server_sweep(&acct_srv);
    puts("sec-int-01 adapter: production loop pass (accept/serve/work), unacknowledged AUTH_OUTCOME times out without faulting the service PASS");
}
int main(int argc, char **argv)
{
    setvbuf(stdout,0,_IONBF,0);
    CHECK(argc==3); storage_dir=argv[1]; production_dir=argv[2];
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram); controller.state=T_RUNNING;
    proc_init(); g_current=&controller;
    fake_init(&fake);
    struct process *lp,*up,*ap;
    logon=role_fixture(CIUKI_SECURITY_ROLE_LOGON,&lp); ui=role_fixture(CIUKI_SECURITY_ROLE_LOGON_UI,&up);
    account_thread=role_fixture(CIUKI_SECURITY_ROLE_ACCOUNT,&ap);
    for (struct process **p=(struct process *[]){lp,up,ap,0};*p;p++) (*p)->actions[SIGCHLD].handler=SIG_IGN;
    test_storage_posix();
    test_production_tables();
    test_lease_and_gate();
    test_retained_subjects();
    test_list_exchange();
    test_sessions();
    test_challenge_and_outcome();
    test_cancellation_and_death();
    test_step_and_timeout();
    acct_server_close(&acct_srv);
    CHECK(!acct_srv.kernel.leased && !acct_srv.kernel.logon.occupied && acct_srv.kernel.allocations==0);
    CHECK(prod_srv.kernel.allocations==0);
    for (unsigned i=0;i<ACCT_KERNEL_LEASES;i++) CHECK(!acct_srv.kernel.leases[i].occupied);
    for (unsigned i=0;i<ACCT_KERNEL_LINKS;i++) CHECK(!acct_srv.kernel.links[i].occupied);
    stop_collect(ap); stop_collect(up); stop_collect(lp);
    CHECK(!desktop_objects.channels && !desktop_objects.descriptions && !desktop_objects.messages);
    CHECK(!pages_used && !g_user_mappings && heap_blocks==1);
    kfree(supervisor_task); supervisor_task=0; CHECK(!heap_blocks);
    free(ram); ram=0; fake_destroy(&fake);
    printf("sec-int-01 adapter integration: PASS checks=%u; zero pages/heap/fds/subjects/channels\n",checks);
    return 0;
}
