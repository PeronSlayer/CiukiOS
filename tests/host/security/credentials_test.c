/* H01 uses the existing physical-memory/scheduler/device boundary fakes.
 * All security decisions, copying, channels and lifecycle are production C.
 * SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_DESKTOP_HOST_MAIN security_unused_desktop_main
#include "../proc/desktop_test.c"

static bool owner_available;
static unsigned owner_calls, image_opens;
static uint32_t expected_uid;
static int security_image_open(void *, const char *, struct ciuki_file *);
int security_exec_open(struct process *p, const char *path, struct ciuki_file *out)
{
    CHECK(!p->published && p->state==PROC_PREPARING);
    CHECK(security_credential(p)->uid==expected_uid && !strcmp(path,"/Programs/test"));
    return security_image_open(p->cwd,path,out);
}
int security_owner_verify(const struct ciuki_security_profile *p)
{
    owner_calls++;
    if (!owner_available) return -EIO;
    if (p->owner_generation_lo != 17) return -EAGAIN;
    return p->owner_key_lo == 23 && p->volume_id_lo == 31 ? 0 : -EACCES;
}
int px_security_cwd(struct process *p, const char *path)
{
    CHECK(p->state == PROC_PREPARING && !p->published);
    CHECK(strstr(path, "/Documents"));
    CHECK(!p->cwd && !p->fds[0].object && !p->fds[1].object && !p->fds[2].object);
    return 0;
}
static int security_image_open(void *cwd, const char *path, struct ciuki_file *out)
{
    (void)cwd;
    CHECK(!strcmp(path, "/Programs/test"));
    struct proc_thread *t = proc_thread_for(g_current);
    CHECK(t && t->launch_loading && !t->launch_loading->published);
    CHECK(security_credential(t->launch_loading)->uid == expected_uid);
    image_opens++;
    *out = fixture_file();
    return 0;
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
#define SEC_ARGS (CIUKI_IMAGE_BASE + PAGE_SIZE + 0x100)
#define SEC_OUT (SEC_ARGS + 0x100)
#define SEC_CRED (SEC_ARGS + 0x200)
#define SEC_PROFILE (SEC_ARGS + 0x300)
#define SEC_SPAWN (SEC_ARGS + 0x400)
static struct ciuki_security_args security_args(uint32_t op)
{
    struct ciuki_security_args a = { .version=1, .size=64, .operation=op };
    if (op <= 3) { a.output_address=SEC_OUT; a.output_bytes=64; }
    return a;
}
static int invoke(struct proc_thread *t, struct ciuki_security_args *a)
{
    desktop_select(t);
    CHECK(!ua_write(t->process->memory, SEC_ARGS, a, 64));
    struct trap_frame tf={.eax=101,.ebx=SEC_ARGS,.ecx=64};
    int err=proc_syscall(&tf);
    struct ciuki_security_args after;
    CHECK(!ua_read(t->process->memory,&after,SEC_ARGS,64));
    if (err) CHECK(!memcmp(a,&after,64));
    else *a=after;
    return err;
}
static void retire(uint32_t id)
{
    g_current=&controller;
    uint32_t state;
    CHECK(!security_session_end(id,true,&state));
    proc_collect();
    CHECK(!security_session_end(id,false,&state) && state==3);
    CHECK(!security_session_end(id,true,&state) && state==3);
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
static void test_security_records(struct proc_thread *system)
{
    struct ciuki_security_args a=security_args(1);
    CHECK(!invoke(system,&a));
    struct ciuki_credential c;
    CHECK(!ua_read(system->process->memory,&c,SEC_OUT,64));
    CHECK(c.uid==0 && c.primary_gid==0 && c.group_count==1 && !c.groups[0] && !c.session_id && !c.flags);
    uint8_t before[64];memset(before,0xa7,64);
    CHECK(!ua_write(system->process->memory,SEC_OUT,before,64));
    a=security_args(1);a.version=2;CHECK(invoke(system,&a)==-EOPNOTSUPP);
    a=security_args(1);a.size=63;CHECK(invoke(system,&a)==-EINVAL);
    a=security_args(8);CHECK(invoke(system,&a)==-EINVAL);
    a=security_args(1);a.flags=1;CHECK(invoke(system,&a)==-EINVAL);
    a=security_args(1);a.output_address++;CHECK(invoke(system,&a)==-EINVAL);
    a=security_args(1);a.output_address=SEC_ARGS;CHECK(invoke(system,&a)==-EINVAL);
    a=security_args(1);a.output_address=0xfffffffc;CHECK(invoke(system,&a)==-EFAULT);
    a=security_args(1);a.output_address=CIUKI_MMAP_BASE+16*PAGE_SIZE;CHECK(invoke(system,&a)==-EFAULT);
    CHECK(!ua_read(system->process->memory,&c,SEC_OUT,64));CHECK(!memcmp(before,&c,64));
    struct trap_frame tf={.eax=101,.ebx=SEC_ARGS,.ecx=63};CHECK(proc_syscall(&tf)==-EINVAL);
    tf.ecx=64;tf.edx=1;CHECK(proc_syscall(&tf)==-EINVAL);
    puts("H01 records: layouts, selectors, spans, overlap, unchanged failure output PASS");
}
static void test_security_templates(struct proc_thread *system)
{
    struct ciuki_credential c=standard(1001);
    struct ciuki_security_profile profile=profile_for(c.uid);
    uint32_t id=0;
    owner_available=false;CHECK(security_session_create(&c,&profile,&id)==-EIO && !id);
    owner_available=true;
    struct ciuki_credential bad=c;
    bad.uid=0;CHECK(security_session_create(&bad,&profile,&id)==-EINVAL);
    bad=c;bad.uid=999;CHECK(security_session_create(&bad,&profile,&id)==-EINVAL);
    bad=c;bad.uid=0xffffffff;CHECK(security_session_create(&bad,&profile,&id)==-EINVAL);
    bad=c;bad.group_count=9;CHECK(security_session_create(&bad,&profile,&id)==-EINVAL);
    bad=c;bad.groups[7]=1;CHECK(security_session_create(&bad,&profile,&id)==-EINVAL);
    bad=c;bad.group_count=2;bad.groups[1]=544;CHECK(security_session_create(&bad,&profile,&id)==-EINVAL);
    bad=c;bad.flags=3;CHECK(security_session_create(&bad,&profile,&id)==-EINVAL);
    profile.owner_generation_lo=18;CHECK(security_session_create(&c,&profile,&id)==-EAGAIN);
    profile=profile_for(c.uid);strcpy(profile.path,"/Windows/Profiles/../secret");profile.path_bytes=(uint32_t)strlen(profile.path)+1;
    CHECK(security_session_create(&c,&profile,&id)==-EINVAL);
    profile=profile_for(c.uid);
    CHECK(!ua_write(system->process->memory,SEC_CRED,&c,64));
    CHECK(!ua_write(system->process->memory,SEC_PROFILE,&profile,112));
    struct ciuki_security_args a=security_args(4);a.credential_address=SEC_CRED;a.profile_address=SEC_PROFILE;
    uint32_t high=session_high_water;unsigned owner_before=owner_calls;
    for (int fail=0;fail<3;fail++) {
        CHECK(!ua_write(system->process->memory,SEC_ARGS,&a,64));
        struct trap_frame tf={.eax=101,.ebx=SEC_ARGS,.ecx=64};copy_fail_after=fail;
        int err=proc_syscall(&tf);copy_fail_after=-1;
        CHECK(err==-EFAULT && session_high_water==high && owner_calls==owner_before);
        struct ciuki_security_args unchanged;CHECK(!ua_read(system->process->memory,&unchanged,SEC_ARGS,64));
        CHECK(!memcmp(&a,&unchanged,64));
    }
    CHECK(!ua_write(system->process->memory,SEC_ARGS,&a,64));
    CHECK(!ua_mprotect(system->process->memory,CIUKI_IMAGE_BASE+PAGE_SIZE,PAGE_SIZE,PROT_READ));
    struct trap_frame readonly={.eax=101,.ebx=SEC_ARGS,.ecx=64};
    CHECK(proc_syscall(&readonly)==-EFAULT && session_high_water==high && owner_calls==owner_before);
    CHECK(!ua_mprotect(system->process->memory,CIUKI_IMAGE_BASE+PAGE_SIZE,PAGE_SIZE,PROT_READ|PROT_WRITE));
    CHECK(!invoke(system,&a));id=a.result;CHECK(id==1);
    uint32_t another=0;CHECK(security_session_create(&c,&profile,&another)==-EBUSY && !another);
    a=security_args(7);a.session_id=id;CHECK(!invoke(system,&a) && a.state==1);
    a=security_args(6);a.session_id=id;a.flags=3;CHECK(invoke(system,&a)==-EINVAL);
    retire(id);
    c.uid=500;c.group_count=2;c.groups[0]=544;c.groups[1]=545;profile=profile_for(500);
    CHECK(!security_session_create(&c,&profile,&id) && id==2);retire(id);
    c=standard(501);c.primary_gid=546;c.groups[0]=546;profile=profile_for(501);
    CHECK(!security_session_create(&c,&profile,&id) && id==3);retire(id);
    c=standard(1001);profile=profile_for(c.uid);
    uint32_t saved=session_high_water;session_high_water=UINT32_MAX;
    CHECK(security_session_create(&c,&profile,&id)==-EAGAIN);session_high_water=saved;
    puts("H01 sessions: exact templates, gid 0, reserved ids, owner gate, one console, monotonic exhaustion PASS");
}
static void test_security_inheritance(struct proc_thread *system)
{
    struct ciuki_credential c=standard(1001);struct ciuki_security_profile profile=profile_for(c.uid);uint32_t id;
    CHECK(!security_session_create(&c,&profile,&id));
    struct process *user,*child;struct proc_thread *ut=session_process(system->process,id,&user);
    struct proc_thread *ct=make_process(user,&child);
    CHECK(!memcmp(security_credential(user),security_credential(child),64));
    struct proc_thread *extra;CHECK(!proc_thread_prepare(user,CIUKI_IMAGE_BASE,0,CIUKI_IMAGE_BASE,0,0,0,false,&extra));
    CHECK(extra->process->subject==ut->process->subject && ct->process->subject!=ut->process->subject);
    struct ciuki_security_args a=security_args(4);CHECK(invoke(ut,&a)==-EPERM);
    a=security_args(5);CHECK(invoke(ut,&a)==-EPERM);
    a=security_args(6);CHECK(invoke(ut,&a)==-EPERM);
    a=security_args(2);a.target_pid=system->process->pid;CHECK(invoke(ut,&a)==-EACCES);
    a=security_args(2);a.target_pid=child->pid;CHECK(!invoke(ut,&a));
    a=security_args(2);a.target_pid=0x7fffffff;CHECK(invoke(ut,&a)==-ESRCH);
    struct process *prepared;CHECK(!proc_prepare(system->process,&prepared));
    CHECK(!security_session_attach(prepared,id));
    struct ciuki_spawn_fd fd={0,0};CHECK(proc_inherit(prepared,system->process,&fd,1)==-EPERM);
    CHECK(!proc_inherit(prepared,system->process,0,0));
    for (unsigned i=0;i<CIUKI_OPEN_MAX;i++) CHECK(!prepared->fds[i].object);
    proc_discard(prepared);
    g_current=&controller;
    uint32_t state;CHECK(!security_session_end(id,true,&state) && state==2);
    CHECK(proc_prepare(user,&prepared)==-EACCES);
    CHECK(proc_thread_prepare(user,CIUKI_IMAGE_BASE,0,CIUKI_IMAGE_BASE,0,0,0,false,&extra)==-EACCES);
    g_current=&controller;proc_collect();CHECK(!security_session_end(id,false,&state) && state==3);
    puts("H01 inheritance: native/threads/orphans immutable, public elevation denied, all SYSTEM fds dropped, closing fenced PASS");
}
static void test_security_channels(struct proc_thread *system)
{
    struct ciuki_credential c=standard(1002);struct ciuki_security_profile profile=profile_for(c.uid);uint32_t id;
    CHECK(!security_session_create(&c,&profile,&id));
    struct process *user;struct proc_thread *ut=session_process(system->process,id,&user);
    int32_t fds[2];CHECK(!channel_connect(user,system->process,fds));
    struct security_peer peer={0};CHECK(!channel_peer_get(system->process,fds[1],&peer));
    CHECK(security_subject_credential(peer.subject)->uid==1002 && channel_peer_live(&peer));
    struct ciuki_security_args a=security_args(3);a.endpoint_fd=fds[1];CHECK(!invoke(system,&a));
    struct ciuki_credential out;CHECK(!ua_read(system->process->memory,&out,SEC_OUT,64) && out.uid==1002);
    a=security_args(3);a.endpoint_fd=fds[0];CHECK(!invoke(ut,&a));
    CHECK(!ua_read(user->memory,&out,SEC_OUT,64) && !out.uid);
    struct ciuki_message message={.sender_pid=1};desktop_select(ut);
    CHECK(!ua_write(user->memory,SEC_OUT,&message,sizeof(message)));
    CHECK(channel_send(user,fds[0],SEC_OUT,DONTWAIT)==-EINVAL);
    message.sender_pid=0;message.length=4;memset(message.data,0,4); /* forged uid=SYSTEM is just data */
    CHECK(!ua_write(user->memory,SEC_OUT,&message,sizeof(message)));
    CHECK(!channel_send(user,fds[0],SEC_OUT,DONTWAIT));desktop_select(system);
    CHECK(channel_recv(system->process,fds[1],SEC_OUT,DONTWAIT)==1);
    CHECK(!ua_read(system->process->memory,&message,SEC_OUT,sizeof(message)) && message.sender_pid==user->pid);
    CHECK(security_subject_credential(peer.subject)->uid==1002);
    CHECK(!desktop_close(user,fds[0]));CHECK(!channel_peer_live(&peer));
    int32_t reused[2];CHECK(!channel_pair(user,reused) && reused[0]==fds[0]);CHECK(!channel_peer_live(&peer));
    channel_peer_put(&peer);CHECK(!desktop_close(system->process,fds[1]));
    CHECK(!desktop_close(user,reused[0]));CHECK(!desktop_close(user,reused[1]));
    CHECK(!channel_connect(user,system->process,fds));CHECK(!channel_peer_get(system->process,fds[1],&peer));
    uint32_t state;CHECK(!security_session_end(id,true,&state));CHECK(!channel_peer_live(&peer));
    g_current=&controller;proc_collect();CHECK(!security_session_end(id,false,&state) && state==3);
    CHECK(!desktop_close(system->process,fds[1]));channel_peer_put(&peer);
    CHECK(!security_session_end(id,false,&state) && state==3);
    puts("H01 IPC: retained authenticated peer, forged uid/sender, close/reuse, dead-session revocation and final release PASS");
}
static void test_security_failure(struct proc_thread *system)
{
    desktop_select(system);
    struct proc_ledger baseline,after;proc_snapshot(&baseline);
    unsigned baseline_heap=heap_blocks,baseline_pages=pages_used;
    unsigned failures=0;
    for (int fail=0;fail<80;fail++) {
        struct process *child=0;alloc_fail=fail;
        int err=proc_prepare(system->process,&child);alloc_fail=-1;
        if (!err) { proc_discard(child); break; }
        CHECK(err==-ENOMEM);failures++;
        proc_snapshot(&after);CHECK(!memcmp(&baseline,&after,sizeof(after)));
        CHECK(heap_blocks==baseline_heap && pages_used==baseline_pages);
    }
    CHECK(failures>=5);
    puts("H01 failures: every proc_prepare allocation rolls back without published pid, pages, handles or subject leaks PASS");
}
static void test_security_handoff(struct proc_thread *system)
{
    int32_t fds[2];CHECK(!channel_pair(system->process,fds));
    system->process->fds[fds[1]].flags=0;
    struct security_peer old={0};CHECK(!channel_peer_get(system->process,fds[0],&old));
    struct process *child;struct ciuki_spawn_fd map={fds[1],3};
    CHECK(!proc_prepare(system->process,&child));CHECK(!proc_inherit(child,system->process,&map,1));
    CHECK(channel_peer_live(&old));proc_discard(child);CHECK(channel_peer_live(&old));
    CHECK(!proc_prepare(system->process,&child));CHECK(!proc_inherit(child,system->process,&map,1));
    proc_publish(child,0);CHECK(!channel_peer_live(&old));
    struct security_peer peer={0};CHECK(!channel_peer_get(system->process,fds[0],&peer));
    CHECK(peer.subject==child->subject && peer.subject!=system->process->subject);
    CHECK(!desktop_close(system->process,fds[1]) && channel_peer_live(&peer));
    uint32_t refs=peer.subject->refs;peer.subject->refs=UINT32_MAX;
    struct security_peer refused={0};CHECK(channel_peer_get(system->process,fds[0],&refused)==-EAGAIN && !refused.endpoint);
    peer.subject->refs=refs;
    stop_collect(child);CHECK(!channel_peer_live(&peer));
    channel_peer_put(&old);channel_peer_put(&peer);CHECK(!desktop_close(system->process,fds[0]));
    puts("H01 handoff: unpublished abort preserves peer, publication binds actual child, parent close cannot revoke child, refcount exhaustion PASS");
}
static void test_security_spawn(struct proc_thread *system)
{
    struct ciuki_credential c=standard(1003);struct ciuki_security_profile profile=profile_for(c.uid);uint32_t id;
    CHECK(!security_session_create(&c,&profile,&id));
    desktop_select(system);expected_uid=c.uid;
    const struct ciuki_file_ops ops={.open=security_image_open};proc_set_file_ops(&ops);
    const uint32_t path=SEC_ARGS+0x500, vector=SEC_ARGS+0x600, argument=SEC_ARGS+0x700;
    const uint32_t envp=SEC_ARGS+0x800, env=SEC_ARGS+0x900;
    uint32_t argv[2]={argument,0}, envv[2]={env,0};
    struct ciuki_spawn_args spawn={.size=32,.path=path,.argv=vector,.envp=envp};
    CHECK(!ua_write(system->process->memory,path,"/Programs/test",15));
    CHECK(!ua_write(system->process->memory,argument,"test",5));
    CHECK(!ua_write(system->process->memory,vector,argv,sizeof(argv)));
    CHECK(!ua_write(system->process->memory,envp,envv,sizeof(envv)));
    CHECK(!ua_write(system->process->memory,env,"USER=Alice",11));
    struct test_object stdio={.object={obj_retain,obj_release,true},.id=0};object_refs[0]=1;
    system->process->fds[0].object=&stdio.object;
    CHECK(!ua_write(system->process->memory,SEC_SPAWN,&spawn,32));
    struct ciuki_security_args a=security_args(5);a.session_id=id;a.spawn_address=SEC_SPAWN;
    CHECK(!invoke(system,&a));struct process *child=proc_find(a.result);CHECK(child);
    CHECK(security_credential(child)->uid==1003 && !security_system(child));
    for (unsigned i=0;i<CIUKI_OPEN_MAX;i++) CHECK(!child->fds[i].object);
    CHECK(object_refs[0]==1);stop_collect(child);
    desktop_select(system);unsigned opened=image_opens;
    CHECK(!ua_write(system->process->memory,env,"SECRET=bad",11));
    a=security_args(5);a.session_id=id;a.spawn_address=SEC_SPAWN;CHECK(invoke(system,&a)==-EINVAL && image_opens==opened);
    CHECK(!ua_write(system->process->memory,env,"HOME=/System",13));
    CHECK(invoke(system,&a)==-EINVAL && image_opens==opened);
    CHECK(!ua_write(system->process->memory,env,"USER=Alice",11));
    spawn.fd_count=1;CHECK(!ua_write(system->process->memory,SEC_SPAWN,&spawn,32));
    CHECK(invoke(system,&a)==-EINVAL);spawn.fd_count=0;
    spawn.path=SEC_ARGS;CHECK(!ua_write(system->process->memory,SEC_SPAWN,&spawn,32));
    CHECK(invoke(system,&a)==-EINVAL);spawn.path=path;
    CHECK(!ua_write(system->process->memory,SEC_SPAWN,&spawn,32));
    struct proc_ledger baseline,after;proc_snapshot(&baseline);unsigned heap=heap_blocks,pages=pages_used;
    unsigned failures=0;bool succeeded=false;
    for (int fail=0;fail<256;fail++) {
        a=security_args(5);a.session_id=id;a.spawn_address=SEC_SPAWN;
        /* Warm both records before fail injection; output pinning still runs. */
        CHECK(!ua_write(system->process->memory,SEC_ARGS,&a,64));
        struct trap_frame tf={.eax=101,.ebx=SEC_ARGS,.ecx=64};alloc_fail=fail;
        int err=proc_syscall(&tf);alloc_fail=-1;
        if (!err) {
            CHECK(!ua_read(system->process->memory,&a,SEC_ARGS,64));child=proc_find(a.result);CHECK(child);
            stop_collect(child);desktop_select(system);succeeded=true;break;
        }
        CHECK(err==-ENOMEM);failures++;
        proc_snapshot(&after);CHECK(!memcmp(&baseline,&after,sizeof(after)));
        CHECK(heap_blocks==heap && pages_used==pages);
        struct ciuki_security_args unchanged;CHECK(!ua_read(system->process->memory,&unchanged,SEC_ARGS,64));
        CHECK(!memcmp(&unchanged,&a,64));
    }
    CHECK(succeeded && failures>10 && object_refs[0]==1);
    unsigned copy_failures=0;succeeded=false;
    for (int fail=0;fail<512;fail++) {
        a=security_args(5);a.session_id=id;a.spawn_address=SEC_SPAWN;
        CHECK(!ua_write(system->process->memory,SEC_ARGS,&a,64));
        struct trap_frame tf={.eax=101,.ebx=SEC_ARGS,.ecx=64};copy_fail_after=fail;
        int err=proc_syscall(&tf);copy_fail_after=-1;
        if (!err) {
            CHECK(!ua_read(system->process->memory,&a,SEC_ARGS,64));child=proc_find(a.result);CHECK(child);
            stop_collect(child);desktop_select(system);succeeded=true;break;
        }
        CHECK(err==-EFAULT);copy_failures++;
        proc_snapshot(&after);CHECK(!memcmp(&baseline,&after,sizeof(after)));
        CHECK(heap_blocks==heap && pages_used==pages);
        struct ciuki_security_args unchanged;CHECK(!ua_read(system->process->memory,&unchanged,SEC_ARGS,64));
        CHECK(!memcmp(&unchanged,&a,64));
    }
    CHECK(succeeded && copy_failures>30);
    system->process->fds[0].object=0;obj_release(&stdio.object);retire(id);
    proc_set_file_ops(0);
    printf("H01 spawn: allowlist/profile cwd, zero SYSTEM fds, overlap/env denied, %u allocation + %u copy failures atomic PASS\n",failures,copy_failures);
}
static void test_security_retention(struct proc_thread *system)
{
    struct security_subject *held[4];uint32_t ids[4];
    for (unsigned i=0;i<4;i++) {
        struct ciuki_credential c=standard(1100+i);struct ciuki_security_profile profile=profile_for(c.uid);
        CHECK(!security_session_create(&c,&profile,&ids[i]));
        struct process *user;session_process(system->process,ids[i],&user);
        held[i]=user->subject;CHECK(!security_subject_get(held[i]));
        retire(ids[i]);CHECK(!security_subject_live(held[i]));
    }
    struct ciuki_credential c=standard(1200);struct ciuki_security_profile profile=profile_for(c.uid);uint32_t id;
    CHECK(security_session_create(&c,&profile,&id)==-EAGAIN);
    for (unsigned i=0;i<4;i++) security_subject_put(held[i]);
    CHECK(!security_session_create(&c,&profile,&id) && id>ids[3]);
    struct process *reserved;CHECK(!proc_prepare(system->process,&reserved));CHECK(!security_session_attach(reserved,id));
    uint32_t state;CHECK(!security_session_end(id,true,&state) && state==2);
    CHECK(security_session_create(&c,&profile,&ids[0])==-EBUSY);
    proc_discard(reserved);CHECK(!security_session_end(id,false,&state) && state==3);
    c=standard(500);c.groups[0]=544;c.groups[1]=545;c.group_count=2;c.flags=3;profile=profile_for(c.uid);
    uint32_t flags=g_boot.flags;g_boot.flags=0;CHECK(security_session_create(&c,&profile,&id)==-EINVAL);
    g_boot.flags=CBI_F_SAFE_MODE;CHECK(!security_session_create(&c,&profile,&id));g_boot.flags=flags;
    struct process *admin;session_process(system->process,id,&admin);CHECK(security_admin(admin) && !security_system(admin));
    struct ciuki_credential out;CHECK(security_query_process(admin,system->process->pid,&out)==-EACCES);retire(id);
    puts("H01 retention: four DEAD slots bounded, final-reference reclamation, provisional-child fence, Safe Mode kernel flag PASS");
}
static int account_image_open(void *cwd, const char *path, struct ciuki_file *out)
{
    (void)cwd;CHECK(!strcmp(path,"/System/Security/accounts"));*out=fixture_file();return 0;
}
static int account_stdio(struct process *p)
{ CHECK(!p->published && security_system(p) && !security_credential(p)->session_id);return 0; }
static void test_security_boot_service(struct proc_thread *system)
{
    const struct ciuki_file_ops *saved_files=proc_get_file_ops();
    const struct supervisor_io_ops *saved_io=supervisor_io;
    const struct ciuki_file_ops files={.open=account_image_open};
    const struct supervisor_io_ops io={.stdio=account_stdio};
    proc_set_file_ops(&files);supervisor_set_io_ops(&io);g_current=&controller;
    struct process *service,*refused=0;
    CHECK(supervisor_spawn_account_service(&service)>0 && security_system(service));
    CHECK(supervisor_spawn_account_service(&refused)==-EBUSY && !refused);stop_collect(service);
    alloc_fail=0;int error=supervisor_spawn_account_service(&refused);alloc_fail=-1;
    CHECK(error==-ENOMEM && !refused);
    CHECK(supervisor_spawn_account_service(&service)>0 && security_system(service));stop_collect(service);
    struct ciuki_credential c=standard(1300);struct ciuki_security_profile profile=profile_for(c.uid);uint32_t id;
    CHECK(!security_session_create(&c,&profile,&id));
    struct process *user;struct proc_thread *ut=session_process(system->process,id,&user);desktop_select(ut);
    uint32_t path=SEC_ARGS+0x600,vector=SEC_ARGS+0x700,entries[2]={path,0};
    const char name[]="/System/Security/accounts";
    CHECK(!ua_write(user->memory,path,name,sizeof(name)));
    CHECK(!ua_write(user->memory,vector,entries,sizeof(entries)));
    struct ciuki_spawn_args args={.size=32,.path=path,.argv=vector};
    CHECK(!ua_write(user->memory,SEC_SPAWN,&args,32));
    int pid=proc_spawn(SEC_SPAWN);CHECK(pid>0);struct process *child=proc_find((uint32_t)pid);
    CHECK(child && security_credential(child)->uid==1300 && !security_system(child));
    stop_collect(child);retire(id);
    proc_set_file_ops(saved_files);supervisor_set_io_ops(saved_io);
    puts("H01 protected service: fixed boot image, SYSTEM/session 0, singleton, failed-start retry, public same-path spawn stays Standard PASS");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    ram=calloc(HOST_PAGES,PAGE_SIZE);CHECK(ram);controller.state=T_RUNNING;
    proc_init();g_current=&controller;
    struct process *system_process;struct proc_thread *system=make_process(proc_supervisor(),&system_process);
    system_process->actions[SIGCHLD].handler=SIG_IGN;
    test_security_records(system);test_security_templates(system);test_security_inheritance(system);
    test_security_channels(system);test_security_failure(system);test_security_handoff(system);
    test_security_spawn(system);test_security_retention(system);
    test_security_boot_service(system);
    stop_collect(system_process);
    CHECK(!pages_used && !g_user_mappings && heap_blocks==1 && owner_calls);
    CHECK(!desktop_objects.channels && !desktop_objects.descriptions && !desktop_objects.messages);
    /* The fixture's PID 1 task is normally process-lifetime reachable. */
    kfree(supervisor_task);supervisor_task=0;CHECK(!heap_blocks);
    free(ram);ram=0;
    printf("H01 credentials: PASS checks=%u; zero pages/heap/fds/subjects/channels\n",checks);
    return 0;
}
