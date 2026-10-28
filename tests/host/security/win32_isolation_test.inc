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
    uint32_t sid=c.session_id,state;CHECK(!security_session_end(sid,true,&state));
    CHECK(!w32_lookup(a,handle,W32_OBJ_TOKEN));
    g_current=&controller;proc_discard(alice);proc_discard(owner);proc_collect();CHECK(!security_session_end(sid,false,&state)&&state==CIUKI_SECURITY_SESSION_DEAD);
    /* Nontransferability is enforced at admission as well as lookup. */
    CHECK(w32_handle_install(system,retained,8,false,&th)==5);w32_release(retained);
    CHECK(!w32_handle_close(system,sh));proc_discard(system);(void)native;
    puts("H07 kernel: real token/query/rights/revocation, session names, protected process, APC/message denial and unchanged queues PASS");
}
