/* H01 uses the existing physical-memory/scheduler/device boundary fakes.
 * All security decisions, copying, channels and lifecycle are production C.
 * SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_DESKTOP_HOST_MAIN security_unused_desktop_main
#include "../proc/desktop_test.c"

static bool owner_available;
static unsigned owner_calls, image_opens;
static uint32_t expected_uid, close_end_session;
static bool close_stop_parent, close_signal_parent;
static const char *committed_timezone;
static int committed_timezone_error=-ENOENT;
int security_profile_timezone(const struct security_subject *subject, const struct ciuki_security_profile *profile,
                              char *out, uint32_t bytes)
{
    CHECK(security_subject_live(subject));
    CHECK(security_subject_credential(subject)->uid==profile->uid);
    if (committed_timezone_error) return committed_timezone_error;
    CHECK(committed_timezone && strlen(committed_timezone)<bytes);
    strcpy(out,committed_timezone); return 0;
}
static void image_close_race(void *cookie)
{
    (void)cookie;
    if (close_end_session) { uint32_t state;CHECK(!security_session_end(close_end_session,true,&state)); }
    if (close_stop_parent) proc_stop(proc_current(),0,0);
    if (close_signal_parent) { struct proc_thread *t=proc_thread_for(g_current);t->pending|=UINT64_C(1)<<SIGUSR1;t->interrupted=true; }
}
static int security_image_open(void *, const char *, struct ciuki_file *);
int security_exec_open(struct process *p, const char *path, struct ciuki_file *out)
{
    CHECK(!p->published && p->state==PROC_PREPARING);
    CHECK(security_credential(p)->uid==expected_uid);
    CHECK(!strcmp(path,"/Programs/test") || !strcmp(path,"/Programs/desktop") || !strcmp(path,"/System/Win32/w32srv"));
    if (!strcmp(path,"/Programs/test")) return security_image_open(p->cwd,path,out);
    *out=fixture_file();return 0;
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
    if (close_end_session || close_stop_parent || close_signal_parent) out->close=image_close_race;
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
    a=security_args(15);CHECK(invoke(system,&a)==-EINVAL);
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
    proc_publish(child,0);CHECK(channel_peer_live(&old));
    struct security_peer peer={0};CHECK(!channel_peer_get(system->process,fds[0],&peer));
    CHECK(peer.subject==system->process->subject && peer.generation==old.generation);
    stop_collect(child);CHECK(channel_peer_live(&peer));
    CHECK(!desktop_close(system->process,fds[1]));CHECK(!channel_peer_live(&peer));
    channel_peer_put(&old);channel_peer_put(&peer);CHECK(!desktop_close(system->process,fds[0]));
    CHECK(!channel_pair(system->process,fds));
    CHECK(!proc_prepare(system->process,&child));
    int32_t handed=-1;
    CHECK(!channel_endpoint_handoff(system->process,fds[1],child,&handed,true));
    CHECK(child->fds[handed].object && !child->fds[handed].object->inheritable);
    proc_publish(child,0);CHECK(!channel_peer_get(system->process,fds[0],&peer));
    CHECK(peer.subject==child->subject);
    stop_collect(child);CHECK(!channel_peer_live(&peer));
    channel_peer_put(&peer);CHECK(!desktop_close(system->process,fds[0]));
    puts("H01 review (b): ordinary inheritance preserves established peer/generation; explicit initial handoff binds child PASS");
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
    close_signal_parent=true;
    a=security_args(5);a.session_id=id;a.spawn_address=SEC_SPAWN;
    CHECK(invoke(system,&a)==-EACCES); close_signal_parent=false;
    system->interrupted=false; system->pending&=~CIUKI_SIGBIT(SIGUSR1);
    proc_snapshot(&after); CHECK(!memcmp(&baseline,&after,sizeof(after)));
    /* An adapter close is allowed to sleep and revoke the provisional child. */
    close_end_session=id;
    a=security_args(5);a.session_id=id;a.spawn_address=SEC_SPAWN;
    CHECK(invoke(system,&a)==-EACCES);close_end_session=0;
    CHECK(security_session_generation(id));
    puts("H01 review (c): close callback ends session; final liveness guard prevents child publication PASS");
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
    (void)cwd;CHECK(!strcmp(path,"/System/Services/accounts"));*out=fixture_file();return 0;
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
    CHECK(supervisor_spawn_account_service(&service)>0 && security_system(service));
    uint32_t generation=security_role_generation(service); stop_collect(service);
    supervisor_poll(); service=security_service_server(2,0);
    CHECK(service && security_system(service) && security_role_generation(service)>generation);
    stop_collect(service); trusted_roles[CIUKI_SECURITY_ROLE_ACCOUNT].enabled=false;
    struct ciuki_credential c=standard(1300);struct ciuki_security_profile profile=profile_for(c.uid);uint32_t id;
    CHECK(!security_session_create(&c,&profile,&id));
    struct process *user;struct proc_thread *ut=session_process(system->process,id,&user);desktop_select(ut);
    uint32_t path=SEC_ARGS+0x600,vector=SEC_ARGS+0x700,entries[2]={path,0};
    const char name[]="/System/Services/accounts";
    CHECK(!ua_write(user->memory,path,name,sizeof(name)));
    CHECK(!ua_write(user->memory,vector,entries,sizeof(entries)));
    struct ciuki_spawn_args args={.size=32,.path=path,.argv=vector};
    CHECK(!ua_write(user->memory,SEC_SPAWN,&args,32));
    int pid=proc_spawn(SEC_SPAWN);CHECK(pid>0);struct process *child=proc_find((uint32_t)pid);
    CHECK(child && security_credential(child)->uid==1300 && !security_system(child) && !security_role(child));
    stop_collect(child);retire(id);
    proc_set_file_ops(saved_files);supervisor_set_io_ops(saved_io);
    puts("H01 protected service: fixed boot image, SYSTEM/session 0, singleton, failed-start retry, public same-path spawn stays Standard PASS");
}
/* Host-only backend substitutes devices, never authorization/state decisions. */
struct safe_console { struct proc_object object; unsigned refs; };
static bool user_visible, user_input;
static unsigned fences, activations;
static int fence_error;
static void safe_retain(struct proc_object *o) { ((struct safe_console *)o)->refs++; }
static void safe_release(struct proc_object *o) { if (!--((struct safe_console *)o)->refs) kfree(o); }
static int safe_install(struct process *p)
{
    struct safe_console *o=kzalloc(sizeof(*o));
    if (!o) return -ENOMEM;
    o->object=(struct proc_object){safe_retain,safe_release,false}; o->refs=1;
    int fd=desktop_fd_slot(p,0);
    if (fd<0) { safe_release(&o->object); return fd; }
    p->fds[fd]=(struct proc_fd){&o->object,FD_CLOEXEC};
    return fd;
}
static int fake_grant_prepare(struct process *p, uint32_t purpose, struct ciuki_security_launch *l)
{
    CHECK(!p->published && p->state==PROC_PREPARING);
    int fd=safe_install(p); if (fd<0) return fd; CHECK(fd==0);
    for (unsigned i=1;i<4;i++) { safe_retain(p->fds[0].object); p->fds[i]=p->fds[0]; }
    l->child_console_fd=3;
    if (purpose==1) {
        fd=safe_install(p); if (fd<0) return fd; l->child_display_fd=fd;
        fd=safe_install(p); if (fd<0) return fd; l->child_input_fd=fd;
    } else { fd=safe_install(p); if (fd<0) return fd; l->child_service_fd=fd; }
    CHECK(!user_visible && !user_input);
    return 0;
}
static int fake_conceal(uint32_t id, uint32_t generation)
{ (void)id;CHECK(generation);fences++;user_visible=user_input=false;return fence_error; }
static int fake_activate(uint32_t id, uint32_t generation)
{ CHECK(id && generation);activations++;user_visible=user_input=true;return 0; }
static void fake_grant_release(uint32_t id) { CHECK(id);user_visible=user_input=false; }
static int fake_trusted(struct process *p) { CHECK(security_role(p)==2); return 0; }
static const struct security_grant_ops fake_grants={fake_grant_prepare,fake_conceal,fake_activate,fake_grant_release,fake_trusted};
static struct proc_thread *role_fixture(uint32_t role, struct process **out)
{
    CHECK(!proc_prepare(proc_supervisor(),out));
    CHECK(!security_role_prepare(proc_supervisor(),*out,role,security_role_image(role)));
    struct ciuki_file f=fixture_file(); struct proc_strings *s=proc_strings_new();
    CHECK(s && !proc_strings_add(s,"role",5,false)); CHECK(!prepare_image(*out,&f,s,0));proc_strings_free(s);
    proc_publish(*out,0);
    for (unsigned i=0;i<CIUKI_THREAD_MAX;i++) if (proc_thread_slot(i) && proc_thread_slot(i)->process==*out) return proc_thread_slot(i);
    CHECK(false);return 0;
}
static int extended_call(struct proc_thread *t, uint32_t op, uint32_t session, const void *in, void *out, uint32_t size)
{
    CHECK(!ua_write(t->process->memory,SEC_SPAWN,in,size));
    struct ciuki_security_args a=security_args(op);
    a.session_id=session; a.spawn_address=SEC_SPAWN;
    if (op!=14) { a.output_address=SEC_OUT; a.output_bytes=size; }
    int err=invoke(t,&a);
    if (!err && out) CHECK(!ua_read(t->process->memory,out,SEC_OUT,size));
    return err ? err : op==14 ? (int)a.result : 0;
}
static struct ciuki_security_launch launch_input(uint32_t id, uint32_t purpose)
{ return (struct ciuki_security_launch){.version=1,.size=96,.session_id=id,
    .session_generation=security_session_generation(id),.purpose=purpose}; }
static int launch_call(struct proc_thread *t,uint32_t id,uint32_t purpose,struct ciuki_security_launch *out)
{ struct ciuki_security_launch in=launch_input(id,purpose); return extended_call(t,8,id,&in,out,96); }
static int poll_call(struct proc_thread *t,struct ciuki_security_launch *l)
{
    struct ciuki_security_launch in=launch_input(l->session_id,l->purpose);
    in.launch_id=l->launch_id; in.launch_generation=l->launch_generation;
    return extended_call(t,9,l->session_id,&in,l,96);
}
static struct proc_thread *thread_of(struct process *p)
{
    for (unsigned i=0;i<CIUKI_THREAD_MAX;i++) if (proc_thread_slot(i) && proc_thread_slot(i)->process==p) return proc_thread_slot(i);
    CHECK(false);return 0;
}
static void ready_child(const struct ciuki_security_launch *l, int status)
{
    struct process *p=proc_find(l->pid);CHECK(p && security_process_generation(p)==l->process_generation);
    desktop_select(thread_of(p));
    struct ciuki_message m={.length=16};uint32_t words[]={1,l->launch_id,l->launch_generation,(uint32_t)status};
    memcpy(m.data,words,16);CHECK(!ua_write(p->memory,SEC_OUT,&m,sizeof(m)));
    CHECK(!channel_send(p,l->child_ready_fd,SEC_OUT,DONTWAIT));
}
static struct ciuki_security_console enter_gate(struct proc_thread *t,uint32_t id,uint32_t reason)
{
    struct ciuki_security_console in={.version=1,.size=64,.session_id=id,
        .session_generation=security_session_generation(id),.reason=reason},out;
    CHECK(!extended_call(t,10,id,&in,&out,64));return out;
}
static int leave_gate(struct proc_thread *t,struct ciuki_security_console *in)
{
    in->state=0;
    struct ciuki_security_console out;
    return extended_call(t,11,in->session_id,in,&out,64);
}
static void test_amendment_environment(struct proc_thread *logon)
{
    struct ciuki_credential c=standard(1007); struct ciuki_security_profile profile=profile_for(c.uid);
    strcpy(profile.path,"/Windows/Profiles/u00000003"); profile.path_bytes=(uint32_t)strlen(profile.path)+1;
    uint32_t id; CHECK(!security_session_create(&c,&profile,&id));
    struct process *child; CHECK(!proc_prepare(logon->process,&child)); CHECK(!security_session_attach(child,id));
    struct credential_copy *copy=kzalloc(sizeof(*copy)); CHECK(copy);
    struct proc_strings *strings=proc_strings_new(); CHECK(strings);
    committed_timezone_error=0; committed_timezone="CET-1CEST,M3.5.0,M10.5.0/3";
    CHECK(!credential_environment(&strings,copy,&profile,child));
    bool locale=false,tz=false,settings=false,home=false;
    for (unsigned i=0;i<strings->envc;i++) {
        const char *value=(const char *)P2V(strings->pages[strings->envp[i]/PAGE_SIZE])+strings->envp[i]%PAGE_SIZE;
        locale|=!strcmp(value,"LC_ALL=C"); tz|=!strcmp(value,"TZ=CET-1CEST,M3.5.0,M10.5.0/3");
        settings|=!strcmp(value,"CIUKI_PROFILE_SETTINGS=/Windows/Profiles/u00000003/Settings/desktop.cfg");
        home|=!strcmp(value,"HOME=/Windows/Profiles/u00000003/Documents");
    }
    CHECK(locale && tz && settings && home);
    proc_strings_free(strings);
    const char *bad[]={"LC_ALL=en_US","TZ=UTC0","CIUKI_PROFILE_SETTINGS=/System/private","CIUKI_LAUNCH_ID=99"};
    for (unsigned i=0;i<ARRAY_SIZE(bad);i++) {
        strings=proc_strings_new(); CHECK(strings && !proc_strings_add(strings,bad[i],(uint32_t)strlen(bad[i])+1,true));
        CHECK(credential_environment(&strings,copy,&profile,child)==-EINVAL); proc_strings_free(strings);
    }
    strings=proc_strings_new(); CHECK(strings);
    committed_timezone="bad timezone"; CHECK(credential_environment(&strings,copy,&profile,child)==-EINVAL);
    committed_timezone_error=-EIO; CHECK(credential_environment(&strings,copy,&profile,child)==-EIO);
    committed_timezone_error=-ENOENT; CHECK(!credential_environment(&strings,copy,&profile,child));
    proc_strings_free(strings); kfree(copy); proc_discard(child); retire(id);
    puts("H01 environment: retained target subject, mapped G32 root, LC_ALL/settings/validated TZ, override and corruption refusal PASS");
}
static void test_amendment_launch(struct proc_thread *logon)
{
    expected_uid=1400;struct ciuki_credential c=standard(expected_uid);struct ciuki_security_profile profile=profile_for(c.uid);
    security_set_grant_ops(&fake_grants);
    struct ciuki_security_console boot=enter_gate(logon,0,4);CHECK(boot.console_epoch && !boot.authentication_required);
    CHECK(leave_gate(logon,&boot)==-EACCES);
    uint32_t id;CHECK(!security_session_create(&c,&profile,&id));
    struct ciuki_security_launch desk,server;
    struct ciuki_security_launch input=launch_input(id,2),sentinel;
    memset(&sentinel,0xa5,sizeof(sentinel));
    for (int fail=0;fail<2;fail++) {
        CHECK(!ua_write(logon->process->memory,SEC_SPAWN,&input,96));
        CHECK(!ua_write(logon->process->memory,SEC_OUT,&sentinel,96));
        struct ciuki_security_args args=security_args(8); args.session_id=id; args.spawn_address=SEC_SPAWN;
        args.output_address=SEC_OUT; args.output_bytes=96;
        CHECK(!ua_write(logon->process->memory,SEC_ARGS,&args,64)); desktop_select(logon);
        struct trap_frame tf={.eax=101,.ebx=SEC_ARGS,.ecx=64}; copy_fail_after=fail;
        CHECK(proc_syscall(&tf)==-EFAULT); copy_fail_after=-1;
        struct ciuki_security_launch unchanged;
        CHECK(!ua_read(logon->process->memory,&unchanged,SEC_OUT,96) && !memcmp(&unchanged,&sentinel,96));
        CHECK(security_session_generation(id)==1 && !session_find(id)->launches[1].state);
    }
    input.reserved[0]=1;
    CHECK(extended_call(logon,8,id,&input,&server,96)==-EINVAL);
    input=launch_input(id,2); input.version=2;
    CHECK(extended_call(logon,8,id,&input,&server,96)==-EOPNOTSUPP);
    struct proc_ledger baseline,after;proc_snapshot(&baseline);unsigned heap=heap_blocks,pages=pages_used,failures=0;
    bool ok=false;
    for (int fail=0;fail<160;fail++) {
        struct ciuki_security_launch in=launch_input(id,2);
        CHECK(!ua_write(logon->process->memory,SEC_SPAWN,&in,96));
        struct ciuki_security_args a=security_args(8);a.session_id=id;a.spawn_address=SEC_SPAWN;a.output_address=SEC_OUT;a.output_bytes=96;
        CHECK(!ua_write(logon->process->memory,SEC_ARGS,&a,64));desktop_select(logon);
        struct trap_frame tf={.eax=101,.ebx=SEC_ARGS,.ecx=64};alloc_fail=fail;
        int err=proc_syscall(&tf);alloc_fail=-1;
        if (!err) { CHECK(!ua_read(logon->process->memory,&server,SEC_OUT,96));ok=true;break; }
        CHECK(err==-ENOMEM);failures++;
        proc_snapshot(&after);CHECK(!memcmp(&baseline,&after,sizeof(after)));
        CHECK(heap_blocks==heap && pages_used==pages && security_session_generation(id)==1);
    }
    CHECK(ok && failures>20);
    CHECK(!launch_call(logon,id,1,&desk));CHECK(launch_call(logon,id,1,&desk)==-EBUSY);
    CHECK(security_role(proc_find(server.pid))==7 && security_role(proc_find(desk.pid))==6);
    CHECK(desk.child_service_fd==-1 && server.child_display_fd==-1 && server.child_input_fd==-1);
    CHECK(!user_visible && !user_input);
    struct ciuki_security_link route={.version=1,.size=64,.service=5},client,accepted;
    struct proc_thread *desktop_thread=thread_of(proc_find(desk.pid));
    CHECK(!extended_call(desktop_thread,12,0,&route,&client,64));
    CHECK(!extended_call(logon,13,0,&route,&accepted,64));
    struct security_subject *desktop_subject=0;
    CHECK(!security_subject_acquire_endpoint(logon->process,accepted.endpoint_fd,accepted.endpoint_generation,&desktop_subject));
    CHECK(!security_subject_check(desktop_subject,(5u<<16)|5,id));
    CHECK(security_subject_check(desktop_subject,(5u<<16)|4,id)==-EPERM);
    CHECK(security_subject_check(desktop_subject,(5u<<16)|5,id+1)==-EACCES);
    CHECK(!desktop_close(desktop_thread->process,client.endpoint_fd));
    CHECK(security_subject_check(desktop_subject,(5u<<16)|5,id)==-ESRCH);
    security_subject_put(desktop_subject); CHECK(!desktop_close(logon->process,accepted.endpoint_fd));
    CHECK(extended_call(thread_of(proc_find(server.pid)),12,0,&route,&client,64)==-EPERM);
    CHECK(!poll_call(logon,&server) && server.state==1);
    struct ciuki_security_console locked=enter_gate(logon,id,1);
    CHECK(locked.authentication_required && !user_visible);
    ready_child(&server,0);CHECK(!poll_call(logon,&server) && server.state==2);
    ready_child(&desk,0);CHECK(!poll_call(logon,&desk) && desk.state==2 && !user_visible);
    struct process *dp=proc_find(desk.pid);const struct ciuki_credential before=*security_credential(dp);
    dp->cpu_ticks+=17;CHECK(dp->state==PROC_LIVE && dp->cpu_ticks==17);
    CHECK(!security_console_attention(4));CHECK(leave_gate(logon,&locked)==-EAGAIN && !user_visible);
    struct ciuki_security_console attention=enter_gate(logon,id,4);
    CHECK(attention.authentication_required && attention.authentication_epoch>locked.authentication_epoch);
    /* Registered LOGON represents the consumed correlated AUTH success. */
    CHECK(!leave_gate(logon,&attention) && user_visible && user_input);
    CHECK(leave_gate(logon,&attention)==-EAGAIN);
    CHECK(!memcmp(&before,security_credential(dp),64) && dp->cpu_ticks==17);
    /* Snapshots must never report a recycled child fd as the grant/console. */
    int old_console=desk.child_console_fd;
    CHECK(!desktop_close(dp,old_console)); CHECK(safe_install(dp)==old_console);
    CHECK(!poll_call(logon,&desk) && desk.child_console_fd==-1);
    /* Same-session children retain identity, never role/control/grant fds,
     * even after the parent clears CLOEXEC on every description. */
    for (unsigned fd=0;fd<CIUKI_OPEN_MAX;fd++) dp->fds[fd].flags=0;
    struct process *descendant; CHECK(!proc_prepare(dp,&descendant));
    CHECK(!proc_inherit(descendant,dp,0,0) && !security_role(descendant));
    for (unsigned fd=0;fd<CIUKI_OPEN_MAX;fd++) CHECK(!descendant->fds[fd].object);
    proc_discard(descendant);
    for (uint32_t role=2;role<=3;role++) {
        struct process *trusted;role_fixture(role,&trusted);
        struct ciuki_security_console current=enter_gate(logon,id,1);
        uint32_t epoch=current.authentication_epoch;
        stop_collect(trusted);
        CHECK(!user_visible && session_find(id)->console.authentication_required &&
            session_find(id)->console.authentication_epoch>epoch && leave_gate(logon,&current)==-EAGAIN);
        role_fixture(role,&trusted);
        current=enter_gate(logon,id,4);CHECK(current.authentication_required);
        CHECK(!leave_gate(logon,&current) && user_visible);
        stop_collect(trusted);CHECK(!user_visible);
    }
    struct ciuki_security_console repeated=enter_gate(logon,id,4),again=enter_gate(logon,id,4);
    CHECK(!memcmp(&repeated,&again,64));
    fence_error=-EBUSY;struct ciuki_security_console in={.version=1,.size=64,.session_id=id,
        .session_generation=security_session_generation(id),.reason=2};
    CHECK(extended_call(logon,10,id,&in,&again,64)==-EBUSY && !user_visible);fence_error=0;
    retire(id);
    CHECK(!security_session_create(&c,&profile,&id));CHECK(!launch_call(logon,id,2,&server));
    g_ticks+=5000;g_current=&controller;security_poll();CHECK(session_find(id)->launches[1].state==4);
    retire(id);
    CHECK(!security_session_create(&c,&profile,&id));CHECK(!launch_call(logon,id,2,&server));
    ready_child(&server,-EIO);CHECK(!poll_call(logon,&server) && server.state==4 && server.error==-EIO);
    retire(id);
    CHECK(fences>=8 && activations>=3);
    security_set_grant_ops(0);
    printf("H01 amendment launch/console: %u allocation failures; readiness, concealed CPU progress, epochs, physical attention, cookies, UI/ACCOUNT restart, fence failure, negative ready and timeout PASS\n",failures);
}
static struct ciuki_security_link connect_security(struct proc_thread *client,uint32_t service)
{
    struct ciuki_security_link in={.version=1,.size=64,.service=service},out;
    CHECK(!extended_call(client,12,0,&in,&out,64));return out;
}
static struct ciuki_security_link accept_security(struct proc_thread *server,uint32_t service)
{
    struct ciuki_security_link in={.version=1,.size=64,.service=service},out;
    CHECK(!extended_call(server,13,0,&in,&out,64));return out;
}
static int frame_send(struct proc_thread *t,const struct ciuki_security_link *l,uint64_t id,uint32_t flags,
                      uint32_t total,uint32_t offset,uint32_t bytes)
{
    struct ciuki_message m={.length=64+bytes};
    uint32_t h[16]={0x31435343,1,64,l->service,1,flags,(uint32_t)id,(uint32_t)(id>>32),
        l->endpoint_generation,bytes,0,0,0,total,offset,0};
    memcpy(m.data,h,64);memset(m.data+64,0x42,bytes);
    desktop_select(t);CHECK(!ua_write(t->process->memory,SEC_OUT,&m,sizeof(m)));
    return channel_send(t->process,l->endpoint_fd,SEC_OUT,DONTWAIT);
}
static int cancel_security(struct proc_thread *t,const struct ciuki_security_link *l,uint64_t id)
{
    struct ciuki_security_link in={.version=1,.size=64,.service=l->service,.endpoint_fd=l->endpoint_fd,
        .endpoint_generation=l->endpoint_generation,.request_id_lo=(uint32_t)id,.request_id_hi=(uint32_t)(id>>32)};
    return extended_call(t,14,0,&in,0,64);
}
static void test_amendment_service(struct proc_thread *logon)
{
    struct process *account;struct proc_thread *at=role_fixture(3,&account);
    struct process *ui;struct proc_thread *uit=role_fixture(2,&ui);
    for (uint32_t operation=4;operation<=11;operation++) if (operation!=7) {
        struct ciuki_security_args denied=security_args(operation);
        CHECK(invoke(at,&denied)==-EPERM);
    }
    struct ciuki_security_link al=connect_security(logon,1),ar=accept_security(at,1);
    CHECK(al.peer_role==3 && ar.peer_role==1 && al.endpoint_generation==ar.endpoint_generation);
    struct security_subject *subject=0;
    CHECK(!security_subject_acquire_endpoint(account,ar.endpoint_fd,ar.endpoint_generation,&subject));
    CHECK(!security_subject_check(subject,(1u<<16)|5,0) && security_subject_role(subject)==1);
    CHECK(!security_subject_get(subject));security_subject_put(subject);
    logon->process->fds[al.endpoint_fd].flags=0;
    struct process *child; CHECK(!proc_prepare(logon->process,&child)); CHECK(!proc_inherit(child,logon->process,0,0));
    for (unsigned i=0;i<CIUKI_OPEN_MAX;i++) CHECK(child->fds[i].object!=logon->process->fds[al.endpoint_fd].object);
    proc_discard(child);
    unsigned heap_before=heap_blocks,pages_before=pages_used;
    for (int fail=0;fail<4;fail++) {
        alloc_fail=fail; int error=frame_send(logon,&al,1,1,8192,0,1); alloc_fail=-1;
        CHECK(error==-ENOMEM && !service_exchanges && heap_blocks==heap_before && pages_used==pages_before);
    }
    CHECK(frame_send(logon,&al,1,1,8193,0,192)==-EINVAL);
    CHECK(!frame_send(logon,&al,1,1,200,0,192));
    desktop_select(at);CHECK(channel_recv(account,ar.endpoint_fd,SEC_OUT,DONTWAIT)==-EAGAIN);
    CHECK(frame_send(logon,&al,1,1,200,193,7)==-EINVAL);
    CHECK(cancel_security(logon,&al,1)==1);
    CHECK(security_subject_check(subject,0,0)==-ESRCH);security_subject_put(subject);
    CHECK(!desktop_close(logon->process,al.endpoint_fd));CHECK(!desktop_close(account,ar.endpoint_fd));
    struct ciuki_security_link next=connect_security(logon,1);ar=accept_security(at,1);
    CHECK(next.endpoint_fd==al.endpoint_fd && next.endpoint_generation!=al.endpoint_generation);
    CHECK(!frame_send(logon,&next,1,1,200,0,192));CHECK(!frame_send(logon,&next,1,1,200,192,8));
    desktop_select(at);CHECK(channel_recv(account,ar.endpoint_fd,SEC_OUT,DONTWAIT)==1);
    CHECK(!security_subject_acquire_endpoint(account,ar.endpoint_fd,ar.endpoint_generation,&subject));
    CHECK(cancel_security(logon,&next,1)==0 && !security_subject_live(subject));
    CHECK(frame_send(at,&ar,1,2,0,0,0)==-ESRCH);security_subject_put(subject);
    CHECK(!desktop_close(logon->process,next.endpoint_fd));CHECK(!desktop_close(account,ar.endpoint_fd));
    next=connect_security(uit,1);ar=accept_security(at,1);
    CHECK(!security_subject_acquire_endpoint(account,ar.endpoint_fd,ar.endpoint_generation,&subject));
    CHECK(!security_subject_check(subject,(1u<<16)|1,0));
    CHECK(security_subject_check(subject,(1u<<16)|5,0)==-EPERM);
    CHECK(!desktop_close(ui,next.endpoint_fd));CHECK(!security_subject_live(subject));
    security_subject_put(subject);CHECK(!desktop_close(account,ar.endpoint_fd));
    next=connect_security(logon,1);ar=accept_security(at,1);
    unsigned body_pages=pages_used,queued=desktop_objects.messages;
    for (unsigned off=0;off<8192;off++) CHECK(!frame_send(logon,&next,2,1,8192,off,1));
    CHECK(pages_used==body_pages+2 && desktop_objects.messages==queued);
    for (unsigned i=0;i<43;i++) {desktop_select(at);CHECK(channel_recv(account,ar.endpoint_fd,SEC_OUT,DONTWAIT)==1);}
    CHECK(!frame_send(at,&ar,2,2,0,0,0));desktop_select(logon);
    CHECK(channel_recv(logon->process,next.endpoint_fd,SEC_OUT,DONTWAIT)==1 && !service_exchanges);
    CHECK(frame_send(logon,&next,2,1,0,0,0)==-EINVAL);
    CHECK(!frame_send(logon,&next,3,1,2,0,1));g_ticks+=5000;
    CHECK(cancel_security(logon,&next,3)==0);
    CHECK(!desktop_close(logon->process,next.endpoint_fd));CHECK(!desktop_close(account,ar.endpoint_fd));
    next=connect_security(logon,1); ar=accept_security(at,1);
    CHECK(!frame_send(logon,&next,1,1,0,0,0)); desktop_select(at);
    CHECK(channel_recv(account,ar.endpoint_fd,SEC_OUT,DONTWAIT)==1);
    CHECK(!frame_send(at,&ar,1,2,200,0,1)); g_ticks+=5000; security_poll();
    CHECK(!service_exchanges && cancel_security(logon,&next,1)==0);
    CHECK(!desktop_close(logon->process,next.endpoint_fd)); CHECK(!desktop_close(account,ar.endpoint_fd));
    struct ciuki_security_link clients[9],servers[9];
    for (unsigned i=0;i<9;i++) { clients[i]=connect_security(logon,1); servers[i]=accept_security(at,1); }
    for (unsigned i=0;i<8;i++) CHECK(!frame_send(logon,&clients[i],1,1,0,0,0));
    CHECK(service_exchanges==8 && frame_send(logon,&clients[8],1,1,0,0,0)==-EAGAIN);
    for (unsigned i=0;i<9;i++) {
        CHECK(cancel_security(logon,&clients[i],1)==(i<8 ? 1 : 0));
        CHECK(!desktop_close(logon->process,clients[i].endpoint_fd)); CHECK(!desktop_close(account,servers[i].endpoint_fd));
    }
    CHECK(!service_exchanges);
    stop_collect(ui);stop_collect(account);
    puts("H01 amendment services: role-bound CONNECT/ACCEPT, retained checks, 8192-byte framing, gaps/replays, cancellation before/after dequeue, late reply, fd generations and timeout PASS");
}
static void test_amendment_restart_matrix(struct proc_thread **current)
{
    security_set_grant_ops(&fake_grants); expected_uid=1450;
    struct ciuki_credential c=standard(expected_uid); struct ciuki_security_profile profile=profile_for(c.uid);
    for (unsigned phase=0;phase<2;phase++) for (uint32_t role=1;role<=3;role++) {
        struct proc_thread *logon=*current;
        struct process *service=logon->process;
        if (role!=1) role_fixture(role,&service);
        uint32_t generation=security_role_generation(service),id;
        CHECK(!security_session_create(&c,&profile,&id));
        struct ciuki_security_launch server,desk;
        CHECK(!launch_call(logon,id,2,&server) && !launch_call(logon,id,1,&desk));
        struct ciuki_security_console old=enter_gate(logon,id,1);
        if (phase) {
            ready_child(&server,0); CHECK(!poll_call(logon,&server));
            ready_child(&desk,0); CHECK(!poll_call(logon,&desk));
        }
        stop_collect(service);
        CHECK(!user_visible && session_find(id)->console.authentication_required);
        struct proc_thread *replacement=role_fixture(role,&service);
        CHECK(security_role_generation(service)>generation);
        if (role==1) { *current=logon=replacement; service->actions[SIGCHLD].handler=SIG_IGN; }
        CHECK(leave_gate(logon,&old)==-EAGAIN);
        if (!phase) { ready_child(&server,0); ready_child(&desk,0); }
        CHECK(!poll_call(logon,&server) && server.state==2);
        CHECK(!poll_call(logon,&desk) && desk.state==2 && !user_visible);
        struct ciuki_security_console fresh=enter_gate(logon,id,4);
        CHECK(fresh.authentication_required && fresh.authentication_epoch>old.authentication_epoch);
        CHECK(!leave_gate(logon,&fresh) && user_visible);
        CHECK(leave_gate(logon,&fresh)==-EAGAIN);
        retire(id); if (role!=1) stop_collect(service);
    }
    /* An uncontained device cannot be given to the next session. */
    uint32_t id,state; CHECK(!security_session_create(&c,&profile,&id));
    struct ciuki_security_launch server; CHECK(!launch_call(*current,id,2,&server));
    fence_error=-EBUSY;
    CHECK(!security_session_end(id,true,&state) && state==2); proc_collect();
    CHECK(!security_session_end(id,false,&state) && state==2);
    uint32_t another; CHECK(security_session_create(&c,&profile,&another)==-EBUSY);
    fence_error=0; security_poll(); retire(id);
    CHECK(!security_session_create(&c,&profile,&id));
    struct proc_thread *logon=*current;
    uint32_t path=SEC_ARGS+0x600,argument=SEC_ARGS+0x650,vector=SEC_ARGS+0x700,argv[]={argument,0};
    CHECK(!ua_write(logon->process->memory,path,"/Programs/test",15));
    CHECK(!ua_write(logon->process->memory,argument,"test",5));
    CHECK(!ua_write(logon->process->memory,vector,argv,sizeof(argv)));
    struct ciuki_spawn_args spawn={.size=32,.path=path,.argv=vector};
    CHECK(!ua_write(logon->process->memory,SEC_SPAWN,&spawn,32));
    struct ciuki_security_args args=security_args(5);args.session_id=id;args.spawn_address=SEC_SPAWN;
    desktop_select(logon); CHECK(!ua_write(logon->process->memory,SEC_ARGS,&args,64));
    struct trap_frame frame={.eax=101,.ebx=SEC_ARGS,.ecx=64};
    close_stop_parent=true; int close_error=proc_syscall(&frame);close_stop_parent=false;
    CHECK(close_error==-EACCES);
    CHECK(logon->process->state==PROC_STOPPING && !session_find(id)->refs);
    /* Direct host dispatch has no architecture return-to-user epilogue. */
    task_kill(logon->task,0);
    stop_collect(logon->process);
    struct process *replacement; *current=role_fixture(1,&replacement);replacement->actions[SIGCHLD].handler=SIG_IGN;
    retire(id);
    security_set_grant_ops(0);
    puts("H01 restart matrix: LOGON/UI/ACCOUNT before readiness and before LEAVE, fresh control binding, no POLL unlock; failed-fence containment PASS");
}

int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    ram=calloc(HOST_PAGES,PAGE_SIZE);CHECK(ram);controller.state=T_RUNNING;
    proc_init();g_current=&controller;
    struct process *system_process;
    CHECK(!proc_prepare(proc_supervisor(),&system_process));
    CHECK(!security_role_prepare(proc_supervisor(),system_process,1,security_role_image(1)));
    struct ciuki_file sf=fixture_file(); struct proc_strings *ss=proc_strings_new();
    CHECK(ss && !proc_strings_add(ss,"test",5,false));
    CHECK(!prepare_image(system_process,&sf,ss,0)); proc_strings_free(ss);
    proc_publish(system_process,0);
    struct proc_thread *system=0;
    for (unsigned i=0;i<CIUKI_THREAD_MAX;i++) if (proc_thread_slot(i) && proc_thread_slot(i)->process==system_process) system=proc_thread_slot(i);
    CHECK(system);
    system_process->actions[SIGCHLD].handler=SIG_IGN;
    test_security_records(system);test_security_templates(system);test_security_inheritance(system);
    test_security_channels(system);test_security_failure(system);test_security_handoff(system);
    test_security_spawn(system);test_security_retention(system);
    test_security_boot_service(system);
    test_amendment_environment(system); test_amendment_launch(system); test_amendment_service(system);
    test_amendment_restart_matrix(&system); system_process=system->process;
    stop_collect(system_process);
    CHECK(!pages_used && !g_user_mappings && heap_blocks==1 && owner_calls);
    CHECK(!desktop_objects.channels && !desktop_objects.descriptions && !desktop_objects.messages);
    /* The fixture's PID 1 task is normally process-lifetime reachable. */
    kfree(supervisor_task);supervisor_task=0;CHECK(!heap_blocks);
    free(ram);ram=0;
    printf("H01 credentials: PASS checks=%u; zero pages/heap/fds/subjects/channels\n",checks);
    return 0;
}
