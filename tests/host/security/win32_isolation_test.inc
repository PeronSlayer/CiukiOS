/* Uses actual credentials/sessions, object tables, APC and message entry
 * points from kernel_core_test.c; no duplicate security policy model. */
static struct proc_thread *security_fixture_user(struct process **out)
{
    CHECK(!proc_prepare(proc_supervisor(),out));CHECK(!security_bootstrap_desktop(*out));
    struct ciuki_file f=fixture_file();struct elf_image image;
    CHECK(!elf_validate(&f,&image)&&!elf_load(&f,&image,(*out)->memory));
    struct proc_thread *t;CHECK(!proc_thread_prepare(*out,image.entry,0,0,CIUKI_THREAD_STACK_DEFAULT,0,0,true,&t));
    proc_publish(*out,0);return t;
}
static struct process *token_exit_target;
static void token_exit_during_allocation(void){proc_stop(token_exit_target,0,0);}
#ifdef CIUKI_POLICY_COVERAGE
/* ---- sec-k08 H13 (tests/host/security/test_policy_coverage.py): the Win32
 * personality. A Standard user (uid 1007) and Guest, each in its own
 * session, try another user's (Bob's, uid 1001, another session) process,
 * token, threads (messages, APCs, suspension) and named objects, and a
 * SYSTEM process; every attempt fails with ERROR_ACCESS_DENIED (or the
 * object stays in the caller's own session namespace) and three more
 * attempts change no Win32 object/handle/message/APC/endpoint account, page
 * or heap block. ---- */
static void h13_credential(struct process *p,uint32_t uid,uint32_t gid,uint32_t session)
{
    struct ciuki_credential *c=&p->subject->credential;
    c->uid=uid;c->primary_gid=gid;c->group_count=1;c->groups[0]=gid;c->session_id=session;c->flags=CIUKI_CRED_INTERACTIVE;
}
static unsigned h13_cases;
struct h13_state { struct w32_account a; unsigned pages,heap; };
static void h13_snapshot(struct h13_state *s){s->a=w32_accounts;s->pages=pages_used;s->heap=heap_blocks;}
static void h13_same(const char *what,const struct h13_state *b)
{
    struct h13_state e;h13_snapshot(&e);
    if(memcmp(b,&e,sizeof(e)))printf("H13 win32 leak in %s: objects %u/%u handles %u/%u messages %u/%u apcs %u/%u pages %u/%u heap %u/%u\n",what,
        b->a.objects,e.a.objects,b->a.handles,e.a.handles,b->a.messages,e.a.messages,b->a.apcs,e.a.apcs,b->pages,e.pages,b->heap,e.heap);
    CHECK(!memcmp(b,&e,sizeof(e)));h13_cases++;
}
static uint32_t h13_open(struct proc_thread *t,uint32_t pid,uint32_t access)
{
    struct ciuki_w32_object_args open={.action=W32_OBJ_OPEN,.type=W32_OBJ_PROCESS,.value=pid,.access=access};
    CHECK(!w32_object_call(t,&open));if(!open.error&&open.result)CHECK(!w32_handle_close(t->process,open.result));return open.error;
}
static uint32_t h13_message(struct proc_thread *t,uint32_t action,uint32_t tid,uint32_t buffer)
{
    struct ciuki_w32_message_args m={.action=action,.tid=tid,.message={.message=action==W32_MSG_SEND?0xc:0x400},.buffer=buffer,.bytes=buffer?16:0};
    CHECK(!w32_message_call(t,&m));return m.error;
}
static uint32_t h13_thread_action(struct proc_thread *t,struct proc_thread *target,bool apc)
{
    uint32_t th;CHECK(!w32_handle_install(t->process,target->w32->identity,UINT32_MAX,false,&th));uint32_t error;
    if(apc){struct ciuki_w32_apc_args a={.action=W32_APC_QUEUE,.thread=th,.routine=CIUKI_W32_LOW};bool replaced=false;
        CHECK(!w32_apc_call(t,&a,NULL,&replaced));error=a.error;CHECK(!target->w32->head);}
    else{struct ciuki_w32_context_args c={.action=W32_CTX_SUSPEND,.thread=th};CHECK(!w32_context_call(t,&c));error=c.error;CHECK(!target->w32->suspend);}
    CHECK(!w32_handle_close(t->process,th));return error;
}
#define H13_W32(what,expr) do{CHECK((expr)==5);struct h13_state h13_b;h13_snapshot(&h13_b);\
    for(unsigned h13_i=0;h13_i<3;h13_i++)CHECK((expr)==5);h13_same(what,&h13_b);}while(0)
static void h13_win32(void)
{
    struct process *bob,*system,*actors[2];
    struct proc_thread *bt=object_process(proc_supervisor(),&bob),*st=object_process(proc_supervisor(),&system),*at[2];
    (void)st;h13_credential(bob,1001,545,301);
    at[0]=object_process(proc_supervisor(),&actors[0]);h13_credential(actors[0],1007,545,300);
    at[1]=object_process(proc_supervisor(),&actors[1]);h13_credential(actors[1],501,546,302);
    uint16_t name[]={'h','1','3','-','s','h','a','r','e',0};struct w32_object *bo;uint32_t bh;bool existing;
    CHECK(!w32_named_create(bob,W32_OBJ_EVENT,name,3,false,&bo,&bh,&existing)&&!existing);
    for(unsigned w=0;w<2;w++){
        struct proc_thread *t=at[w];g_current=t->task;g_current->state=T_RUNNING;
        struct ciuki_w32_vm scratch=request(0,PAGE_SIZE,W32_MEM_RESERVE|W32_MEM_COMMIT,W32_PAGE_READWRITE);
        CHECK(!w32_vm_alloc(t->process->memory,&scratch));
        static const uint32_t rights[]={0x1000,0x400,0x0010,0x0020,0x0001,0x1fffff};   /* query, read/write memory, terminate, all */
        for(unsigned i=0;i<sizeof(rights)/sizeof(*rights);i++){
            H13_W32("Bob's process",h13_open(t,bob->pid,rights[i]));
            H13_W32("SYSTEM process",h13_open(t,system->pid,rights[i]));
        }
        H13_W32("post to Bob's thread",h13_message(t,W32_MSG_POST,bt->tid,0));
        H13_W32("send to Bob's thread",h13_message(t,W32_MSG_SEND,bt->tid,scratch.address));
        H13_W32("APC to Bob's thread",h13_thread_action(t,bt,true));
        H13_W32("suspend Bob's thread",h13_thread_action(t,bt,false));
        H13_W32("APC to a SYSTEM thread",h13_thread_action(t,st,true));
        /* Bob's named event: the caller gets its own session's object. */
        struct w32_object *mine;uint32_t mh;
        CHECK(!w32_named_create(t->process,W32_OBJ_EVENT,name,3,false,&mine,&mh,&existing)&&!existing&&mine!=bo);
        bo->signalled=true;CHECK(w32_wait_try(t,&mh,1,false,0)==W32_WAIT_TIMEOUT||!mine->signalled);bo->signalled=false;
        CHECK(!w32_handle_close(t->process,mh));h13_cases++;
    }
    CHECK(!w32_handle_close(bob,bh));
    g_current=&controller;for(unsigned w=0;w<2;w++)proc_discard(actors[w]);proc_discard(bob);proc_discard(system);proc_collect();
    printf("H13 win32: %u refusals (Standard user and Guest: another session's process/token rights, messages, APCs, suspension and named objects, SYSTEM process and thread) without object/handle/message/APC/page/heap leaks PASS\n",h13_cases);
}
/* ---- end sec-k08 ---- */
#endif
static void test_security_isolation(void)
{
    proc_init();struct process *owner,*alice,*system;struct proc_thread *native=security_fixture_user(&owner);
    struct proc_thread *a=object_process(owner,&alice),*s=object_process(proc_supervisor(),&system);
    g_current=a->task;g_current->state=T_RUNNING;
    struct ciuki_w32_vm scratch=request(0,PAGE_SIZE,W32_MEM_RESERVE|W32_MEM_COMMIT,W32_PAGE_READWRITE);
    CHECK(!w32_vm_alloc(alice->memory,&scratch));
    struct ciuki_w32_object_args token={.action=W32_OBJ_CREATE,.type=W32_OBJ_TOKEN,.source=W32_CURRENT_PROCESS,.access=8};
    CHECK(!w32_object_call(a,&token)&&!token.error);uint32_t handle=token.result;
    token=(struct ciuki_w32_object_args){.action=W32_OBJ_QUERY,.handle=handle,.name=scratch.address,.maximum=64};
    CHECK(!w32_object_call(a,&token)&&!token.error&&token.result==64&&token.value==7);
    struct ciuki_credential c;CHECK(!ua_read(alice->memory,&c,scratch.address,64)&&c.uid==1000&&c.session_id);
    token=(struct ciuki_w32_object_args){.action=W32_OBJ_CREATE,.type=W32_OBJ_TOKEN,.source=W32_CURRENT_PROCESS,.access=0x28};
    CHECK(!w32_object_call(a,&token)&&token.error==5);
    token=(struct ciuki_w32_object_args){.action=W32_OBJ_SET_INHERIT,.handle=handle,.value=1};
    CHECK(!w32_object_call(a,&token)&&token.error==5);
    struct process *race;object_process(owner,&race);
    struct ciuki_w32_object_args race_open={.action=W32_OBJ_OPEN,.type=W32_OBJ_PROCESS,.value=race->pid,.access=0x1000};
    CHECK(!w32_object_call(a,&race_open)&&!race_open.error);
    token_exit_target=race;w32_allocation_hook=token_exit_during_allocation;
    struct ciuki_w32_object_args race_token={.action=W32_OBJ_CREATE,.type=W32_OBJ_TOKEN,.source=race_open.result,.access=8};
    CHECK(!w32_object_call(a,&race_token)&&race_token.error==6&&!race_token.result);
    CHECK(!w32_handle_close(alice,race_open.result));proc_discard(race);token_exit_target=NULL;
    struct ciuki_w32_object_args wrong_kind={.action=W32_OBJ_QUERY,.type=W32_OBJ_TOKEN,.handle=W32_CURRENT_PROCESS,.name=scratch.address,.maximum=64};
    CHECK(!w32_object_call(a,&wrong_kind)&&wrong_kind.error==6);
    struct ciuki_w32_object_args query_process={.action=W32_OBJ_OPEN,.type=W32_OBJ_PROCESS,.value=alice->pid,.access=0x1000};
    CHECK(!w32_object_call(a,&query_process)&&!query_process.error);
    struct ciuki_w32_system_args priority={.action=W32_S_QUERY_PRIORITY,.handle=query_process.result};
    CHECK(!w32_system_call(a,&priority)&&!priority.error);
    uint32_t prior_priority=alice->w32->priority_class;
    priority=(struct ciuki_w32_system_args){.action=W32_S_PROCESS_PRIORITY,.handle=query_process.result,.value=0x40};
    CHECK(!w32_system_call(a,&priority)&&priority.error==5&&alice->w32->priority_class==prior_priority);
    CHECK(!w32_handle_close(alice,query_process.result));
    struct ciuki_w32_object_args open={.action=W32_OBJ_OPEN,.type=W32_OBJ_PROCESS,.value=system->pid,.access=0x1fffff};
    CHECK(!w32_object_call(a,&open)&&open.error==5);
    struct ciuki_w32_message_args message={.action=W32_MSG_POST,.tid=s->tid,.message={.message=0x400}};
    uint32_t messages=w32_accounts.messages;CHECK(!w32_message_call(a,&message)&&message.error==5&&messages==w32_accounts.messages);
    message=(struct ciuki_w32_message_args){.action=W32_MSG_SEND,.tid=s->tid,.message={.message=0xc},.buffer=scratch.address,.bytes=16};
    CHECK(!w32_message_call(a,&message)&&message.error==5&&messages==w32_accounts.messages);
    /* Even a retained pre-security thread reference is rechecked at APC queue. */
    uint32_t th;CHECK(!w32_handle_install(alice,s->w32->identity,UINT32_MAX,false,&th));
    struct ciuki_w32_apc_args apc={.action=W32_APC_QUEUE,.thread=th,.routine=CIUKI_W32_LOW};bool replaced=false;
    CHECK(!w32_apc_call(a,&apc,NULL,&replaced)&&apc.error==5&&!s->w32->head);
    struct ciuki_w32_context_args suspend={.action=W32_CTX_SUSPEND,.thread=th};
    CHECK(!w32_context_call(a,&suspend)&&suspend.error==5&&!s->w32->suspend);
    CHECK(!w32_handle_close(alice,th));
    CHECK(!w32_handle_install(alice,a->w32->identity,0x40,false,&th));
    suspend=(struct ciuki_w32_context_args){.action=W32_CTX_SUSPEND,.thread=th};
    CHECK(!w32_context_call(a,&suspend)&&suspend.error==5&&!a->w32->suspend);
    priority=(struct ciuki_w32_system_args){.action=W32_S_QUERY_PRIORITY,.handle=th,.value=1};
    CHECK(!w32_system_call(a,&priority)&&!priority.error);
    int32_t prior_relative=a->w32->relative_priority;
    priority=(struct ciuki_w32_system_args){.action=W32_S_THREAD_PRIORITY,.handle=th,.value=1};
    CHECK(!w32_system_call(a,&priority)&&priority.error==5&&a->w32->relative_priority==prior_relative);
    CHECK(!w32_handle_close(alice,th));
    uint16_t name[]={'s','e','c','-','s','h','a','r','e',0};struct w32_object *ao,*so;uint32_t ah,sh;bool existing;
    CHECK(!w32_named_create(alice,W32_OBJ_EVENT,name,3,false,&ao,&ah,&existing)&&!existing);
    CHECK(!w32_named_create(system,W32_OBJ_EVENT,name,3,false,&so,&sh,&existing)&&!existing&&ao!=so);
    ao->signalled=true;
    CHECK(w32_wait_try(a,&ah,1,false,0)==W32_WAIT_FAILED&&ao->signalled);
    struct ciuki_w32_object_args duplicate_args={.action=W32_OBJ_DUPLICATE,.source=W32_CURRENT_PROCESS,
        .target=W32_CURRENT_PROCESS,.handle=ah,.access=0x100003};
    CHECK(!w32_object_call(a,&duplicate_args)&&duplicate_args.error==5);
    struct w32_object *retained=w32_lookup(a,handle,W32_OBJ_TOKEN);CHECK(retained);w32_retain(retained);
    /* --- sec-k07 (begin): Win32 part of logoff teardown (production
     * diagnostics.c mailbox and objects list, called by PID 1 while CLOSING). */
    w32_refusal_queue(alice,4,"k07-user.dll",2); w32_refusal_queue(system,5,"k07-system.dll",3);
    unsigned k07_mine=0,k07_total=w32_fault_count;
    for(unsigned i=0;i<w32_fault_count;i++) k07_mine+=w32_fault_session[(w32_fault_head+i)%16]==c.session_id;
    CHECK(k07_mine>=1 && k07_total>k07_mine);
    unsigned k07_bound=0;
    for(struct w32_object *o=w32_objects;o;o=o->next)
        k07_bound+=o->security_session==c.session_id||(o->type==W32_OBJ_TOKEN&&o->u.token.session_id==c.session_id);
    CHECK(k07_bound>=2 && w32_session_revoke(c.session_id)==k07_bound);
    CHECK(w32_fault_count==k07_total-k07_mine);
    for(unsigned i=0;i<w32_fault_count;i++) CHECK(w32_fault_session[(w32_fault_head+i)%16]!=c.session_id);
    CHECK(!w32_session_revoke(0));
    /* --- sec-k07 (end) --- */
    uint32_t sid=c.session_id,state;CHECK(!security_session_end(sid,true,&state));
    CHECK(!w32_lookup(a,handle,W32_OBJ_TOKEN));
    g_current=&controller;proc_discard(alice);proc_discard(owner);proc_collect();CHECK(!security_session_end(sid,false,&state)&&state==CIUKI_SECURITY_SESSION_DEAD);
    /* Nontransferability is enforced at admission as well as lookup. */
    CHECK(w32_handle_install(system,retained,8,false,&th)==5);w32_release(retained);
    CHECK(!w32_session_revoke(sid)); /* sec-k07: nothing of the drained session remains */
    CHECK(!w32_handle_close(system,sh));proc_discard(system);(void)native;
#ifdef CIUKI_POLICY_COVERAGE
    h13_win32();   /* sec-k08 */
#endif
    puts("H07 kernel: real token/query/rights/revocation, session names, protected process, APC/message denial and unchanged queues PASS");
    puts("H09 Win32 teardown (sec-k07): session reports purged, bound named objects/tokens counted until released PASS");
}
