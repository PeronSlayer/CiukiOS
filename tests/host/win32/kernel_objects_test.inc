/* Deterministic production object/section interleavings on the RAM fixture. */
static struct proc_thread *object_process(struct process *parent, struct process **out)
{
    make_pe(pe_data, false);
    make_pe(loader_data, true);
    loader_missing = false;
    static const struct ciuki_file_ops ops = { .open = loader_open };
    proc_set_file_ops(&ops);
    struct proc_strings *s = proc_strings_new();
    CHECK(s);
    CHECK(!proc_strings_add(s, "/fixture.exe", 13, false));
    struct ciuki_file image = { pe_data, sizeof(pe_data), pe_read, 0 };
    CHECK(proc_spawn_file(parent, &image, s, 0, 0, 0, 0, out) > 0);
    proc_strings_free(s);
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (t && t->process == *out)
            return t;
    }
    CHECK(false);
    return 0;
}
static uint32_t object_create(struct proc_thread *t, uint32_t type, uint32_t value, uint32_t maximum)
{
    struct ciuki_w32_object_args a
        = { .action = W32_OBJ_CREATE, .type = type, .value = value, .maximum = maximum };
    CHECK(!w32_object_call(t, &a) && !a.error && a.result && !(a.result & 3));
    return a.result;
}
static struct proc_thread *interleave_thread;
static struct w32_object *interleave_first, *interleave_second;
static unsigned interleave_step;
static void wait_interleave(void)
{
    interleave_step++;
    if (interleave_step == 1) {
        interleave_first->signalled = true;
        w32_changed_wake();
        CHECK(interleave_first->signalled);
    }
    if (interleave_step == 2) {
        interleave_second->signalled = true;
        w32_changed_wake();
    }
}
static void terminate_wait_interleave(void)
{
    struct ciuki_w32_system_args a={.action=W32_S_TERMINATE_THREAD,.handle=W32_CURRENT_THREAD,.value=0x12345678};
    CHECK(!w32_system_call(interleave_thread,&a)&&!a.error&&a.result==interleave_thread->tid);
    on_schedule=0;
}
static void pulse_interleave(void)
{
    struct ciuki_w32_object_args a = { .action = W32_OBJ_PULSE, .handle = interleave_step };
    CHECK(!w32_object_call(interleave_thread, &a) && !a.error);
}
/* The other thread's ResumeThread while the suspended thread is parked at
 * its user return after a timer IRQ: the park is a plain blocked task (no
 * kwait with IF off) and the resume makes it runnable at once. */
static unsigned preempted_resumes;
static void suspend_preempted_interleave(void)
{
    struct proc_thread *t = interleave_thread;
    CHECK(!(host_flags & 0x200) && t->w32->safe && t->w32->suspend == 1);
    CHECK(t->task->state == T_BLOCKED && t->task->wake_tick == g_ticks); /* armed at now+1, the stub ticked once */
    struct ciuki_w32_context_args a = { .action = W32_CTX_RESUME, .thread = W32_CURRENT_THREAD };
    CHECK(!w32_context_call(t, &a) && !a.error && a.result == 1 && !t->w32->suspend);
    CHECK(t->task->state == T_READY && !t->task->wake_tick);
    preempted_resumes++;
}
/* The timer fired (timer_task_wake made the task ready) and only then did
 * another thread clear the suspend count: no direct unpark, loop re-check. */
static unsigned timer_parks;
static void suspend_timer_interleave(void)
{
    struct proc_thread *t = interleave_thread;
    CHECK(t->task->state == T_BLOCKED && t->w32->safe);
    if (++timer_parks == 1) { t->task->state = T_READY; t->task->wake_tick = 0; return; } /* timer wake, still suspended: parks again */
    t->w32->suspend = 0;
    t->task->state = T_READY; t->task->wake_tick = 0;
}
/* Codex review of wt/q3crash, P1: the retirement record may sleep on serial
 * backpressure; these hooks run during it. */
static struct proc_thread *rival_registrar; static uint32_t rival_error = 1234; static int hook_busy;
static void rival_during_log(const char *fmt)
{
    if (hook_busy || !strstr(fmt, "service server pid=")) return;  /* the retirement record; its reason is an argument */
    hook_busy = 1;
    struct ciuki_w32_service_args a = { .action = W32_SVC_REGISTER, .service = 2 };
    CHECK(!w32_service_call(rival_registrar, &a));
    rival_error = a.error;
    hook_busy = 0;
}
static struct process *reap_target; static unsigned reaped;
static void reap_during_log(const char *fmt)
{
    if (hook_busy || !strstr(fmt, "service server pid=")) return;
    hook_busy = 1;
    struct task *saved = g_current; g_current = &controller;
    proc_stop(reap_target, 0, 0); proc_collect(); reaped++;
    g_current = saved;
    hook_busy = 0;
}
static unsigned count_fds(const struct process *p)
{
    unsigned n = 0;
    for (unsigned i = 0; p->fds && i < CIUKI_OPEN_MAX; i++) n += !!p->fds[i].object;
    return n;
}
static void suspend_interleave(void)
{
    CHECK(interleave_thread->w32->safe);
    struct ciuki_w32_context_args a = { .action = W32_CTX_GET,
        .thread = W32_CURRENT_THREAD,
        .flags = 0x1003f,
        .context = interleave_thread->teb + 1024 };
    CHECK(!w32_context_call(interleave_thread, &a) && !a.error);
    struct ciuki_w32_context c;
    CHECK(!ua_read(interleave_thread->process->memory, &c, a.context, sizeof(c)));
    CHECK(c.eax == 42);
    c.eax = 77;
    c.debug[0] = 0x1234;
    memset(c.extended + 160, 0x51, 128);
    CHECK(!ua_write(interleave_thread->process->memory, a.context, &c, sizeof(c)));
    a.action = W32_CTX_SET;
    CHECK(!w32_context_call(interleave_thread, &a) && !a.error);
    a.action = W32_CTX_RESUME;
    CHECK(!w32_context_call(interleave_thread, &a) && a.result == 1);
}
static struct proc_thread *relay_peer;
static uint32_t relay_bytes = 6, relay_buffer;
static void relay_interleave(void)
{
    struct task *saved = g_current;
    g_current = relay_peer->task;
    struct ciuki_w32_message_args get = { .action = W32_MSG_GET,
        .buffer = relay_buffer ? relay_buffer : relay_peer->teb + 512,
        .bytes = relay_bytes };
    CHECK(!w32_message_call(relay_peer, &get) && get.result && get.token && get.message.lparam == get.buffer
        && get.bytes == relay_bytes);
    char copied[6];
    CHECK(!ua_read(relay_peer->process->memory, copied, get.buffer, 6) && !memcmp(copied, "hello", 6));
    get.action = W32_MSG_REPLY;
    get.message.lparam = 77;
    CHECK(!w32_message_call(relay_peer, &get) && !get.error);
    g_current = saved;
    on_schedule = 0;
}
static void test_services(struct proc_thread *client)
{
    struct process *desktop, *server;
    struct proc_thread *dt = make_process(proc_supervisor(), &desktop), *st = make_process(desktop, &server);
    CHECK(dt && st);
    w32_service_delegate(proc_supervisor(), desktop, "/Programs/Desktop");
    CHECK(!desktop->w32);
    w32_service_delegate(proc_supervisor(), desktop, "/Programs/desktop");
    CHECK(desktop->w32 && desktop->w32->service == 1);
    fake_desktop_pid = desktop->pid;
    w32_service_delegate(desktop, server, "/System/Win32/w32srv");
    CHECK(server->w32 && server->w32->service == 2);
    struct ciuki_w32_service_args service = { .action = W32_SVC_REGISTER, .service = 2 };
    CHECK(!w32_service_call(client, &service) && service.error == 5);
    service.error = 0;
    CHECK(!w32_service_call(st, &service) && !service.error);
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_REGISTER, .service = 1 };
    CHECK(!w32_service_call(dt, &service) && !service.error);
    CHECK(!w32_service_desktop_client(desktop,client->process->pid));
    service=(struct ciuki_w32_service_args){.action=W32_SVC_CONNECT,.service=1};
    CHECK(!w32_service_call(client,&service)&&!service.error);
    uint32_t desktop_endpoint=service.endpoint;
    CHECK(!w32_service_desktop_client(desktop,client->process->pid));
    service=(struct ciuki_w32_service_args){.action=W32_SVC_ACCEPT,.service=1};
    CHECK(!w32_service_call(dt,&service)&&!service.error&&service.result);
    CHECK(w32_service_desktop_client(desktop,client->process->pid));
    CHECK(!w32_service_desktop_client(server,client->process->pid));
    struct proc_object *connected_end=client->process->fds[desktop_endpoint].object;
    client->process->fds[desktop_endpoint].object=NULL;
    CHECK(!w32_service_desktop_client(desktop,client->process->pid));
    client->process->fds[desktop_endpoint].object=connected_end;
    CHECK(w32_service_desktop_client(desktop,client->process->pid));
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_CONNECT, .service = 2 };
    CHECK(!w32_service_call(client, &service) && !service.error);
    uint32_t endpoint = service.endpoint;
    CHECK(client->process->fds[endpoint].object && !client->process->fds[endpoint].object->inheritable);
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_ACCEPT, .service = 2 };
    CHECK(!w32_service_call(st, &service) && !service.error && service.result
        && service.pid == client->process->pid);
    struct proc_object *peer_end = server->fds[service.endpoint].object;
    CHECK(peer_end && !peer_end->inheritable);
    /* Native CONNECT is restricted to the current display owner and its
     * exact Control Panel child; a basename and an inherited role cannot do it. */
    struct process *control,*random;
    struct proc_thread *ct=make_process(desktop,&control),*rt=make_process(desktop,&random);
    service=(struct ciuki_w32_service_args){.action=W32_SVC_CONNECT,.service=2};
    CHECK(!w32_service_call(dt,&service) && !service.error && service.result);
    service=(struct ciuki_w32_service_args){.action=W32_SVC_CONNECT,.service=1};
    CHECK(!w32_service_call(dt,&service) && service.error==5);
    w32_service_delegate(desktop,random,"/User/control");
    service=(struct ciuki_w32_service_args){.action=W32_SVC_CONNECT,.service=2};
    CHECK(!w32_service_call(rt,&service) && service.error==5);
    w32_service_delegate(desktop,control,"/Programs/control");
    CHECK(control->w32 && !random->w32);
    service=(struct ciuki_w32_service_args){.action=W32_SVC_CONNECT,.service=2};
    CHECK(!w32_service_call(ct,&service) && !service.error && service.result);
    registry_grant=false;
    service=(struct ciuki_w32_service_args){.action=W32_SVC_CONNECT,.service=2};
    CHECK(!w32_service_call(ct,&service) && service.error==5);
    registry_grant=true;
    service=(struct ciuki_w32_service_args){.action=W32_SVC_CONNECT,.service=1};
    CHECK(!w32_service_call(ct,&service) && service.error==5);
    proc_stop(control,0,0);proc_stop(random,0,0);proc_collect();
    /* Native service operations are admitted without granting core authority. */
    struct task *saved = g_current;
    g_current = st->task;
    service = (struct ciuki_w32_service_args) {
        .header = { 1, sizeof(service), W32_SERVICE, 0 }, .action = W32_SVC_ACCEPT, .service = 2
    };
    CHECK(!ua_write(server->memory, st->tls + 64, &service, sizeof(service)));
    struct trap_frame tf = { .ebx = st->tls + 64, .ecx = sizeof(service) };
    CHECK(!w32_syscall(&tf));
    struct ciuki_w32_object_args denied
        = { .header = { 1, sizeof(denied), W32_OBJECT, 0 }, .action = W32_OBJ_CREATE, .type = W32_OBJ_EVENT };
    CHECK(!ua_write(server->memory, st->tls + 64, &denied, sizeof(denied)));
    tf = (struct trap_frame) { .ebx = st->tls + 64, .ecx = sizeof(denied) };
    CHECK(w32_syscall(&tf) == -EPERM);
    g_current = saved;
    struct process *spawned;
    struct proc_thread *nt = object_process(proc_supervisor(), &spawned);
    uint32_t params;
    struct ciuki_w32_parameters_v2 handoff;
    CHECK(!ua_read(spawned->memory, &params, spawned->w32_peb + 16, 4)
        && !ua_read(spawned->memory, &handoff, params, sizeof(handoff)) && handoff.core.version == 2
        && handoff.core.size == 32 && handoff.desktop_endpoint != UINT32_MAX
        && handoff.w32srv_endpoint != UINT32_MAX);
    CHECK(nt);
    proc_stop(spawned, 0, 0);
    proc_collect();
    proc_stop(server, 0, 0);
    proc_collect();
    CHECK(client->process->state == PROC_LIVE && !client->process->fds[endpoint].object);
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_CONNECT, .service = 2 };
    CHECK(!w32_service_call(client, &service) && service.error == 1062);
    st = make_process(desktop, &server);
    w32_service_delegate(desktop, server, "/System/Win32/w32srv");
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_REGISTER, .service = 2 };
    CHECK(!w32_service_call(st, &service) && !service.error);
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_CONNECT, .service = 2 };
    CHECK(!w32_service_call(client, &service) && !service.error);
    /* q3crash 2026-10-25 (1): a Win32 client's death must not close the
     * desktop's end of its service link from the kernel: the desktop polls
     * that descriptor (POLLNVAL ended it with status 1, or the number was
     * reused). The server keeps its descriptor, reads the hang-up and closes. */
    struct process *dying; struct proc_thread *dying_thread = object_process(desktop, &dying);
    CHECK(dying_thread && dying->w32 && dying->w32->endpoint[1]); /* handoff connected it to the window server */
    /* Links are prepended: the newest pending connection is accepted first. */
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_ACCEPT, .service = 2 };
    CHECK(!w32_service_call(st, &service) && !service.error && service.result && service.pid == dying->pid);
    int channel_poll_events(struct desktop_description *);
    int32_t kept_fd = (int32_t)service.endpoint;
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_ACCEPT, .service = 2 };
    CHECK(!w32_service_call(st, &service) && !service.error && service.result && service.pid == client->process->pid);
    struct proc_object *kept_end = server->fds[kept_fd].object;
    CHECK(kept_end);
    struct task *before_client_death = g_current;
    g_current = &controller;                     /* the dying client's task is collectable */
    proc_stop(dying, 0, 0);
    proc_collect();
    CHECK(server->fds[kept_fd].object == kept_end);
    CHECK(channel_poll_events(desktop_fd(server, kept_fd)) & POLLHUP);
    CHECK(!desktop_close(server, kept_fd) && !server->fds[kept_fd].object);
    /* q3crash 2026-10-25 (2): the desktop died; its window server is an
     * orphan of PID 1 and still registered. The supervisor's hook retires it;
     * independently, a successor's registration retires a stale holder
     * instead of failing with error 5 forever (323 restarts). */
    struct process *next_desktop, *next_server;
    struct proc_thread *ndt = make_process(proc_supervisor(), &next_desktop);
    CHECK(ndt);
    w32_service_delegate(proc_supervisor(), next_desktop, "/Programs/desktop");
    proc_stop(desktop, 0, 0);
    proc_collect();
    CHECK(server->state == PROC_LIVE && server->ppid == proc_supervisor()->pid);
    fake_desktop_pid = next_desktop->pid;
    struct proc_thread *nst = make_process(next_desktop, &next_server);
    CHECK(nst);
    w32_service_delegate(next_desktop, next_server, "/System/Win32/w32srv");
    CHECK(next_server->w32 && next_server->w32->service == 2);
    struct process *rival; struct proc_thread *rt2 = make_process(next_desktop, &rival);
    CHECK(rt2);
    w32_service_delegate(next_desktop, rival, "/System/Win32/w32srv");
    /* Codex review P1: while the retirement is recorded on the console, a
     * rival server of the same desktop registers. Lookup, stale check, stop
     * and publication are one interval, so the rival meets a live current
     * holder and is refused; the first registrar stays registered. */
    rival_registrar = rt2; rival_error = 1234; on_klog = rival_during_log;
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_REGISTER, .service = 2 };
    CHECK(!w32_service_call(nst, &service) && !service.error && service.result);
    on_klog = 0;
    CHECK(rival_error == 5);
    CHECK(server->state != PROC_LIVE);           /* the stale holder was retired */
    proc_collect();
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_REGISTER, .service = 1 };
    CHECK(!w32_service_call(ndt, &service) && !service.error);
    /* A live holder whose desktop is current still refuses a second server. */
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_REGISTER, .service = 2 };
    CHECK(!w32_service_call(rt2, &service) && service.error == 5 && next_server->state == PROC_LIVE);
    proc_stop(rival, 0, 0); proc_collect();
    /* Codex review P1: during the record the retired server exits and is
     * reaped (the old code stopped it after the record: use after free). */
    struct process *third_desktop, *third_server;
    struct proc_thread *tdt = make_process(proc_supervisor(), &third_desktop);
    CHECK(tdt);
    w32_service_delegate(proc_supervisor(), third_desktop, "/Programs/desktop");
    uint32_t next_server_pid = next_server->pid;
    proc_stop(next_desktop, 0, 0);
    proc_collect();
    CHECK(next_server->state == PROC_LIVE && next_server->ppid == proc_supervisor()->pid);
    fake_desktop_pid = third_desktop->pid;
    struct proc_thread *tst = make_process(third_desktop, &third_server);
    CHECK(tst);
    w32_service_delegate(third_desktop, third_server, "/System/Win32/w32srv");
    reap_target = next_server; reaped = 0; on_klog = reap_during_log;
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_REGISTER, .service = 2 };
    CHECK(!w32_service_call(tst, &service) && !service.error && service.result);
    on_klog = 0;
    CHECK(reaped == 1 && !proc_find(next_server_pid));
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_REGISTER, .service = 1 };
    CHECK(!w32_service_call(tdt, &service) && !service.error);
    service = (struct ciuki_w32_service_args) { .action = W32_SVC_ACCEPT, .service = 2 };
    CHECK(!w32_service_call(tst, &service) && !service.error);   /* the third server is the holder */
    /* Codex review P2: a Win32 program that dies before either server
     * accepted its connection leaves no descriptor behind in the servers
     * (channel_connect installed one each; ACCEPT would never report it). */
    unsigned desktop_fds = count_fds(third_desktop), server_fds = count_fds(third_server);
    for (unsigned k = 0; k < 5; k++) {
        struct process *brief; struct proc_thread *bt = object_process(third_desktop, &brief);
        CHECK(bt && brief->w32 && brief->w32->endpoint[0] && brief->w32->endpoint[1]);
        CHECK(count_fds(third_desktop) == desktop_fds + 1 && count_fds(third_server) == server_fds + 1);
        g_current = &controller;
        proc_stop(brief, 0, 0);
        proc_collect();
        CHECK(count_fds(third_desktop) == desktop_fds && count_fds(third_server) == server_fds);
    }
    /* The hook: the desktop's death retires its registered window server at once. */
    w32_services_desktop_died(third_desktop->pid);
    CHECK(third_server->state != PROC_LIVE);
    proc_collect();
    proc_stop(third_desktop, 0, 0);
    proc_collect();
    g_current = before_client_death;
    fake_desktop_pid = 0;
    CHECK(!w32_accounts.endpoints && !desktop_objects.channels && !desktop_objects.descriptions);
}
static uint32_t interleave_file_base, interleave_file_section;
static unsigned writer_refusals;
static void writer_interleave(void)
{
    on_preempt_point = 0;
    struct ciuki_w32_view_args v = { .action = W32_VIEW_MAP,
        .section = interleave_file_section,
        .bytes = 4096,
        .access = W32_FILE_MAP_WRITE };
    CHECK(!w32_view_call(relay_peer, &v) && v.error == 5);
    writer_refusals++;
}
static void view_io_interleave(void)
{
    model_io_interleave = 0;
    CHECK(interleave_thread->in_syscall);
    struct ciuki_w32_view_args v = { .action = W32_VIEW_UNMAP, .address = interleave_file_base };
    CHECK(!w32_view_call(interleave_thread, &v) && v.error == 170);
    struct ciuki_w32_vm a = request(interleave_file_base, 4096, 0, W32_PAGE_READONLY);
    CHECK(w32_vm_protect(interleave_thread->process->memory, &a) == W32_STATUS_DEVICE_BUSY);
}
static void cow_protect_interleave(void)
{
    uint32_t *page = pte(interleave_thread->process->memory, 0x70010000, false);
    if (page && (*page & PTE_SHARED))
        CHECK(!(*page & PTE_W));
}
static void test_objects_sections(void)
{
    const uint32_t operations[]
        = { W32_OBJECT, W32_WAIT, W32_APC, W32_CONTEXT, W32_MESSAGE, W32_SECTION, W32_VIEW, W32_SERVICE };
    for (unsigned i = 0; i < sizeof(operations) / sizeof(operations[0]); i++) {
        uint32_t wire[60] = { 0 };
        struct ciuki_w32_header *h = (void *)wire;
        *h = (struct ciuki_w32_header) { 1, w32_record_size(operations[i]), operations[i], 0 };
        if (operations[i] != W32_WAIT)
            wire[4] = 1;
        CHECK(!w32_record_validate(wire, h->size, true));
        CHECK(w32_record_validate(wire, h->size - 4, true) == -EINVAL);
        CHECK(w32_record_validate(wire, h->size, false) == (operations[i] == W32_SERVICE ? 0 : -EPERM));
        h->reserved = 1;
        CHECK(w32_record_validate(wire, h->size, true) == -EINVAL);
        h->reserved = 0;
        h->version = 2;
        CHECK(w32_record_validate(wire, h->size, true) == -EINVAL);
    }
    struct w32_account empty;
    w32_snapshot(&empty);
    CHECK(!empty.objects && !empty.metadata_bytes);
    unsigned initial_pages = pages_used, initial_heap = heap_blocks;
    int32_t initial_promises[MM_OWNERS][2];
    memcpy(initial_promises, promises, sizeof(promises));
    struct process *p, *other;
    struct proc_thread *t = object_process(proc_supervisor(), &p),
                       *peer = object_process(proc_supervisor(), &other);
    g_current = t->task;
    t->task->state = T_RUNNING;
    uint8_t fp[512] __attribute__((aligned(16)));
    memset(fp, 0, sizeof(fp));
    t->task->fpu_area = fp;
    t->task->win32 = true;
    g_cpu_fxsr = true;
    g_cpu_sse = true;
    fpu_init();
    uint32_t event = object_create(t, W32_OBJ_EVENT, 0, 0), sem = object_create(t, W32_OBJ_SEMAPHORE, 0, 3);
    struct w32_object *event_object = w32_lookup(t, event, W32_OBJ_EVENT),
                      *sem_object = w32_lookup(t, sem, W32_OBJ_SEMAPHORE);
    event_object->signalled = true;
    uint32_t hs[2] = { event, sem };
    CHECK(w32_wait_try(t, hs, 2, true, 0) == W32_WAIT_PENDING && event_object->signalled
        && !sem_object->u.semaphore.count);
    sem_object->u.semaphore.count = 1;
    CHECK(w32_wait_try(t, hs, 2, true, 0) == 0 && !event_object->signalled && !sem_object->u.semaphore.count);
    event_object->signalled = true;
    sem_object->u.semaphore.count = 1;
    CHECK(w32_wait_try(t, hs, 2, false, 0) == 0 && !event_object->signalled
        && sem_object->u.semaphore.count == 1);
    CHECK(w32_wait_try(t, hs, 2, false, 0) == 1 && !sem_object->u.semaphore.count);
    uint32_t event2 = object_create(t, W32_OBJ_EVENT, 0, 0);
    hs[1] = event2;
    CHECK(!ua_write(p->memory, t->teb + 128, hs, 8));
    interleave_thread = t;
    interleave_first = event_object;
    interleave_second = w32_lookup(t, event2, 0);
    interleave_step = 0;
    on_schedule = wait_interleave;
    struct ciuki_w32_wait_args w = { .handles = t->teb + 128, .count = 2, .all = 1, .timeout = 10 };
    CHECK(!w32_wait_call(t, &w) && w.result == 0 && interleave_step == 2 && !event_object->signalled
        && !interleave_second->signalled);
    on_schedule = 0;
    w = (struct ciuki_w32_wait_args) { .handles = t->teb + 128, .count = 1, .timeout = 5 };
    uint64_t start = g_ticks;
    CHECK(!w32_wait_call(t, &w) && w.result == 258 && g_ticks == start + 5);
    interleave_step = event;
    on_schedule = pulse_interleave;
    w = (struct ciuki_w32_wait_args) { .handles = t->teb + 128, .count = 1, .timeout = 5 };
    CHECK(!w32_wait_call(t, &w) && w.result == 0 && !event_object->signalled);
    on_schedule = 0;
    CHECK(w32_wait_try(t, &event, 1, false, 0) == W32_WAIT_PENDING);
    uint32_t mutex = object_create(t, W32_OBJ_MUTEX, 0, 0), mh;
    struct w32_object *mo = w32_lookup(t, mutex, 0);
    CHECK(!w32_handle_install(other, mo, 123, false, &mh));
    CHECK(w32_wait_try(t, &mutex, 1, false, 0) == 0 && w32_wait_try(t, &mutex, 1, false, 0) == 0
        && mo->u.mutex.recursion == 2);
    struct ciuki_w32_object_args a = { .action = W32_OBJ_RELEASE, .handle = mh };
    CHECK(!w32_object_call(peer, &a) && a.error == 288);
    /* Before the abandonment simulation marks this identity as exited. */
    interleave_thread=t;
    on_schedule=terminate_wait_interleave;
    struct ciuki_w32_wait_args termination_sleep={.timeout=W32_INFINITE};
    CHECK(!w32_wait_call(t,&termination_sleep)&&!termination_sleep.error&&termination_sleep.result==W32_WAIT_FAILED);
    CHECK(t->w32->terminate_requested&&t->w32->terminate_code==0x12345678);
    t->w32->terminate_requested=false;t->interrupted=false;
    w32_thread_died(t);
    CHECK(w32_wait_try(peer, &mh, 1, false, 0) == 128 && mo->u.mutex.owner == peer->tid);
    t->w32->ending = false;
    a = (struct ciuki_w32_object_args) { .action = W32_OBJ_RELEASE, .handle = mh };
    CHECK(!w32_object_call(peer, &a) && !a.error);
    a = (struct ciuki_w32_object_args) { .action = W32_OBJ_RELEASE, .handle = sem, .value = 4 };
    CHECK(!w32_object_call(t, &a) && a.error == 298);
    uint32_t process_handle;
    CHECK(!w32_handle_install(p, other->w32->identity, 77, false, &process_handle));
    a = (struct ciuki_w32_object_args) { .action = W32_OBJ_DUPLICATE,
        .source = W32_CURRENT_PROCESS,
        .target = process_handle,
        .handle = event,
        .flags = W32_DUP_SAME_ACCESS,
        .value = 1 };
    CHECK(!w32_object_call(t, &a) && !a.error && w32_lookup(peer, a.result, 0) == event_object);
    uint32_t duplicate = a.result;
    CHECK(!w32_handle_close(other, duplicate) && w32_handle_close(other, duplicate) == 6);
    uint32_t old = event2;
    CHECK(!w32_handle_close(p, event2) && !w32_lookup(t, old, 0) && w32_handle_close(p, old) == 6);
    event2 = object_create(t, W32_OBJ_EVENT, 0, 0);
    CHECK(w32_lookup(t, event2, 0) && event2 <= 0x4000);
    /* q3exec: handle values are (slot + 1) * 4 and a freed value is reused
     * by the next handle of that slot, as on Windows; Win16-derived code
     * (the VISE installer stub) keeps HFILEs in sign-extended WORDs, so no
     * value may reach 0x8000 however often slots are reused. */
    {
        uint32_t first = object_create(t, W32_OBJ_EVENT, 0, 0), second = object_create(t, W32_OBJ_EVENT, 0, 0);
        for (unsigned i = 0; i < 70000; i++) {
            CHECK(!w32_handle_close(p, first));
            uint32_t again = object_create(t, W32_OBJ_EVENT, 0, 0);
            CHECK(again == first && (uint32_t)(int32_t)(int16_t)again == again);
        }
        CHECK(!w32_handle_close(p, first) && !w32_handle_close(p, second));
        CHECK(!w32_lookup(t, first, 0) && !w32_lookup(t, 0x4004, 0) && !w32_lookup(t, 0x8000 | first, 0));
    }
    a = (struct ciuki_w32_object_args) { .action = W32_OBJ_SET_INHERIT, .handle = event, .value = 1 };
    CHECK(!w32_object_call(t, &a) && !a.error);
    struct process *child;
    CHECK(!proc_prepare(p, &child));
    child->personality = PROC_WIN32;
    child->memory->win32 = true;
    CHECK(!w32_handles_inherit(child, p));
    struct proc_thread inherited = { .process = child };
    CHECK(w32_lookup(&inherited, event, 0) == event_object && !w32_lookup(&inherited, sem, 0));
    proc_discard(child);
    const uint16_t name[] = { 'G', 'l', 'o', 'b', 'a', 'l', '\\', 'x', '\\', 'A', 0 },
                   flat[] = { 'x', '\\', 'A', 0 };
    struct w32_object *named, *again;
    uint32_t nh, nh2;
    bool exists;
    CHECK(!w32_named_create(p, W32_OBJ_EVENT, name, 1, false, &named, &nh, &exists) && !exists);
    CHECK(!w32_named_create(other, W32_OBJ_EVENT, flat, 2, false, &again, &nh2, &exists) && exists
        && named == again);
    CHECK(w32_named_create(other, W32_OBJ_MUTEX, flat, 0, false, &again, &nh2, &exists) == 6);
    uint32_t routine = 0x401000;
    t->process->w32->apc_dispatcher = 0x70011000;
    CHECK(!w32_apc_queue(t, routine, 11, 0, 0) && !w32_apc_queue(t, routine, 22, 0, 0));
    CHECK(t->w32->head->argument == 11 && t->w32->tail->argument == 22);
    uint32_t record = t->teb + 256;
    w = (struct ciuki_w32_wait_args) {
        .header = { 1, sizeof(w), W32_WAIT, 0 }, .alertable = 1, .timeout = 0
    };
    CHECK(!ua_write(p->memory, record, &w, sizeof(w)));
    struct trap_frame tf = { .eax = 96,
        .ebx = record,
        .ecx = sizeof(w),
        .gs = 0x23,
        .fs = 0x4b,
        .es = 0x23,
        .ds = 0x23,
        .cs = 0x1b,
        .user_ss = 0x23,
        .eip = routine,
        .user_esp = t->stack_base + t->stack_bytes - 128,
        .eflags = 0x202 };
    CHECK(!w32_syscall(&tf) && tf.eip == p->w32->apc_dispatcher);
    uint32_t apc_frame[5];
    CHECK(!ua_read(p->memory, apc_frame, tf.user_esp, sizeof(apc_frame)) && apc_frame[2] == 11);
    struct ciuki_w32_apc_args ap = { .action = W32_APC_RETURN };
    bool replaced = false;
    CHECK(!w32_apc_call(t, &ap, &tf, &replaced) && replaced);
    CHECK(!ua_read(p->memory, apc_frame, tf.user_esp, sizeof(apc_frame)) && apc_frame[2] == 22);
    CHECK(
        !w32_apc_call(t, &ap, &tf, &replaced) && tf.eip == routine && !t->w32->apc_context && !t->w32->head);
    CHECK(!w32_apc_queue(t, routine, 31, 0, 0));
    CHECK(w32_apc_deliver(t, &tf));
    struct ciuki_w32_context *outer = t->w32->apc_context;
    tf.eip = routine + 4;
    CHECK(!w32_apc_queue(t, routine, 32, 0, 0));
    CHECK(w32_apc_deliver(t, &tf) && t->w32->apc_context != outer);
    CHECK(!w32_apc_call(t, &ap, &tf, &replaced) && tf.eip == routine + 4 && t->w32->apc_context == outer);
    CHECK(!w32_apc_call(t, &ap, &tf, &replaced) && tf.eip == routine && !t->w32->apc_context);
    tf.eax = 42;
    interleave_thread = t;
    on_schedule = suspend_interleave;
    struct ciuki_w32_context_args cx = { .action = W32_CTX_SUSPEND, .thread = W32_CURRENT_THREAD };
    CHECK(!w32_context_call(t, &cx) && !cx.error);
    w32_user_return(t, &tf);
    CHECK(tf.eax == 77 && !t->w32->suspend);
    on_schedule = 0;
    for (unsigned i = 160; i < 288; i++)
        CHECK(t->task->fpu_area[i] == 0x51);
    /* q3crash 2026-10-25: SuspendThread of a running thread takes effect on
     * trap_return after the timer IRQ that preempted it, with IF cleared
     * (interrupt gate), and the syscall tail re-disables IF too; the park
     * must not be a kwait (sync: blocking operation outside thread context). */
    tf.eax = 42;
    interleave_thread = t;
    on_schedule = suspend_preempted_interleave;
    cx = (struct ciuki_w32_context_args) { .action = W32_CTX_SUSPEND, .thread = W32_CURRENT_THREAD };
    CHECK(!w32_context_call(t, &cx) && !cx.error && cx.result == 0);
    uint32_t trap_tail = irq_save();         /* IF off: the trap_return hook */
    w32_user_return(t, &tf);
    CHECK(!(host_flags & 0x200));
    irq_restore(trap_tail);
    CHECK(preempted_resumes == 1 && tf.eax == 42 && !t->w32->suspend && !t->w32->safe);
    CHECK(t->task->state == T_RUNNING && !t->task->wake_tick);
    on_schedule = 0;
    /* The resume may also come from the one-tick safety timer: the parked
     * task wakes without a direct w32_unpark and leaves on resume_ready. */
    cx = (struct ciuki_w32_context_args) { .action = W32_CTX_SUSPEND, .thread = W32_CURRENT_THREAD };
    CHECK(!w32_context_call(t, &cx) && !cx.error);
    on_schedule = suspend_timer_interleave;
    trap_tail = irq_save();
    w32_user_return(t, &tf);
    irq_restore(trap_tail);
    CHECK(timer_parks == 2 && !t->w32->suspend && !t->w32->safe && t->task->state == T_RUNNING);
    on_schedule = 0;
    /* w32_unpark is a no-op for a thread that is not parked. */
    w32_unpark(t);
    CHECK(t->task->state == T_RUNNING);
    cx = (struct ciuki_w32_context_args) { .action = W32_CTX_SUSPEND, .thread = W32_CURRENT_THREAD };
    for (unsigned i = 0; i < 127; i++) {
        CHECK(!w32_context_call(t, &cx) && !cx.error && cx.result == i);
    }
    CHECK(!w32_context_call(t, &cx) && cx.error == 87 && t->w32->suspend == 127);
    cx.action = W32_CTX_RESUME;
    cx.error = 0;
    for (unsigned i = 127; i; i--)
        CHECK(!w32_context_call(t, &cx) && !cx.error && cx.result == i);
    struct ciuki_w32_message_args m
        = { .action = W32_MSG_POST, .tid = peer->tid, .message = { .message = 0x400, .wparam = 7 } };
    for (unsigned i = 0; i < 10000; i++) {
        m.error = 0;
        CHECK(!w32_message_call(t, &m) && !m.error);
    }
    CHECK(!w32_message_call(t, &m) && m.error == 1816 && w32_accounts.messages == 10000);
    CHECK(w32_queue_bits(peer) == W32_QS_POSTMESSAGE && other->w32->private_bytes < 4 * 1024 * 1024);
    g_current = peer->task;
    peer->task->state = T_RUNNING;
    m = (struct ciuki_w32_message_args) { .action = W32_MSG_GET };
    CHECK(!w32_message_call(peer, &m) && m.result && m.message.wparam == 7);
    m = (struct ciuki_w32_message_args) {
        .action = W32_MSG_POST, .tid = peer->tid, .message = { .message = 0xc }
    };
    CHECK(!w32_message_call(peer, &m) && m.error == 87);
    g_current = t->task;
    relay_peer = peer;
    on_schedule = relay_interleave;
    CHECK(!ua_write(p->memory, t->teb + 512, "hello", 6));
    m = (struct ciuki_w32_message_args) { .action = W32_MSG_SEND,
        .tid = peer->tid,
        .message = { .message = 0xc },
        .buffer = t->teb + 512,
        .bytes = 6 };
    CHECK(!w32_message_call(t, &m) && !m.error && m.result == 77 && !sends);
    struct ciuki_w32_vm sendbuf = request(0, 65536, W32_MEM_RESERVE | W32_MEM_COMMIT, 4),
                        receivebuf = sendbuf;
    CHECK(!w32_vm_alloc(p->memory, &sendbuf) && !w32_vm_alloc(other->memory, &receivebuf));
    relay_bytes = 65536;
    relay_buffer = receivebuf.address;
    CHECK(!ua_write(p->memory, sendbuf.address, "hello", 6));
    on_schedule = relay_interleave;
    m = (struct ciuki_w32_message_args) { .action = W32_MSG_SEND,
        .tid = peer->tid,
        .message = { .message = 0x4a },
        .buffer = sendbuf.address,
        .bytes = 65536 };
    CHECK(!w32_message_call(t, &m) && !m.error && m.result == 77 && !sends);
    sendbuf.flags = W32_MEM_RELEASE;
    sendbuf.bytes = 0;
    receivebuf.flags = W32_MEM_RELEASE;
    receivebuf.bytes = 0;
    CHECK(!w32_vm_free(p->memory, &sendbuf) && !w32_vm_free(other->memory, &receivebuf));
    relay_bytes = 6;
    relay_buffer = 0;
    struct ciuki_w32_wait_args sleep = { .timeout = 3 };
    start = g_ticks;
    CHECK(!w32_wait_call(t, &sleep) && !sleep.error && sleep.result == 258 && g_ticks == start + 3);
    test_services(t);
    uint32_t timer = object_create(t, W32_OBJ_TIMER, 0, 0);
    int64_t relative = -20000;
    uint64_t due = (uint64_t)relative;
    a = (struct ciuki_w32_object_args) { .action = W32_OBJ_TIMER_SET,
        .handle = timer,
        .value = (uint32_t)due,
        .maximum = (uint32_t)(due >> 32),
        .extra = 3 };
    CHECK(!w32_object_call(t, &a) && !a.error);
    CHECK(!ua_write(p->memory, t->teb + 128, &timer, 4));
    w = (struct ciuki_w32_wait_args) { .handles = t->teb + 128, .count = 1, .timeout = 10 };
    start = g_ticks;
    CHECK(!w32_wait_call(t, &w) && !w.error && w.result == 0 && g_ticks == start + 2);
    CHECK(w32_wait_try(t, &timer, 1, false, 0) == W32_WAIT_PENDING);
    g_ticks += 3;
    w32_timer_poll();
    CHECK(w32_wait_try(t, &timer, 1, false, 0) == 0);
    a = (struct ciuki_w32_object_args) { .action = W32_OBJ_TIMER_CANCEL, .handle = timer };
    CHECK(!w32_object_call(t, &a) && !a.error);
    uint64_t absolute = w32_system_time() + 10000;
    a = (struct ciuki_w32_object_args) { .action = W32_OBJ_TIMER_SET,
        .handle = timer,
        .value = (uint32_t)absolute,
        .maximum = (uint32_t)(absolute >> 32) };
    CHECK(!w32_object_call(t, &a) && !a.error);
    g_ticks++;
    w32_timer_poll();
    CHECK(w32_wait_try(t, &timer, 1, false, 0) == 0);
    struct ciuki_w32_section_args sec
        = { .kind = W32_SEC_PAGEFILE, .bytes = 131072, .protect = W32_PAGE_READWRITE };
    CHECK(!w32_section_call(t, &sec) && !sec.error);
    uint32_t section = sec.result, peer_section;
    struct w32_object *so = w32_lookup(t, section, 0);
    CHECK(!w32_handle_install(other, so, 0, false, &peer_section));
    struct ciuki_w32_view_args va = { .action = W32_VIEW_MAP,
        .section = section,
        .bytes = 4096,
        .offset_lo = 65536,
        .access = W32_FILE_MAP_WRITE };
    CHECK(!w32_view_call(t, &va) && !va.error && !(va.address & 65535));
    uint32_t base = va.address;
    va = (struct ciuki_w32_view_args) { .action = W32_VIEW_MAP,
        .section = peer_section,
        .bytes = 4096,
        .offset_lo = 65536,
        .access = W32_FILE_MAP_WRITE };
    CHECK(!w32_view_call(peer, &va) && !va.error);
    uint32_t peer_base = va.address;
    uint32_t value = 123456, read = 99;
    CHECK(!ua_read(p->memory, &read, base, 4) && !read);
    CHECK(!ua_write(p->memory, base, &value, 4) && !ua_read(other->memory, &read, peer_base, 4)
        && read == value);
    {
        /* fx-hlrun: OpenFileMapping opens a section by the name CreateFileMapping
         * gave it (one global namespace); missing names, unnamed requests and
         * other object types fail as on Windows. Names live in user memory. */
        static const uint16_t shared_name[] = { 'H', 'L', 'S', 'h', 'a', 'r', 'e', 0 },
                              absent_name[] = { 'H', 'L', 'X', 0 }, event_name[] = { 'H', 'L', 'E', 0 };
        uint32_t names = base + 1024;
        CHECK(!ua_write(p->memory, names, shared_name, sizeof(shared_name))
            && !ua_write(p->memory, names + 64, absent_name, sizeof(absent_name))
            && !ua_write(p->memory, names + 128, event_name, sizeof(event_name)));
        struct ciuki_w32_section_args named_sec
            = { .kind = W32_SEC_PAGEFILE, .bytes = 65536, .protect = W32_PAGE_READWRITE, .name = names };
        CHECK(!w32_section_call(t, &named_sec) && !named_sec.error);
        struct ciuki_w32_object_args open
            = { .action = W32_OBJ_OPEN, .type = W32_OBJ_SECTION, .name = names, .access = UINT32_MAX };
        CHECK(!w32_object_call(t, &open) && !open.error && open.result != named_sec.result
            && w32_lookup(t, open.result, 0) == w32_lookup(t, named_sec.result, 0));
        struct ciuki_w32_view_args nv = { .action = W32_VIEW_MAP, .section = named_sec.result, .bytes = 4096,
            .access = W32_FILE_MAP_WRITE };
        CHECK(!w32_view_call(t, &nv) && !nv.error);
        uint32_t creator_view = nv.address;
        nv = (struct ciuki_w32_view_args) { .action = W32_VIEW_MAP, .section = open.result, .bytes = 4096,
            .access = W32_FILE_MAP_WRITE };
        CHECK(!w32_view_call(t, &nv) && !nv.error && nv.address != creator_view);
        uint32_t opened_view = nv.address, mark = 0x484c4f4d, seen = 0;
        CHECK(!ua_write(p->memory, creator_view + 8, &mark, 4) && !ua_read(p->memory, &seen, opened_view + 8, 4)
            && seen == mark);
        struct ciuki_w32_object_args miss
            = { .action = W32_OBJ_OPEN, .type = W32_OBJ_SECTION, .name = names + 64, .access = UINT32_MAX };
        CHECK(!w32_object_call(t, &miss) && miss.error == 2);
        miss = (struct ciuki_w32_object_args) { .action = W32_OBJ_OPEN, .type = W32_OBJ_SECTION, .access = UINT32_MAX };
        CHECK(!w32_object_call(t, &miss) && miss.error == 87);
        struct ciuki_w32_object_args ev
            = { .action = W32_OBJ_CREATE, .type = W32_OBJ_EVENT, .name = names + 128, .access = UINT32_MAX };
        CHECK(!w32_object_call(t, &ev) && !ev.error);
        miss = (struct ciuki_w32_object_args) { .action = W32_OBJ_OPEN, .type = W32_OBJ_SECTION, .name = names + 128,
            .access = UINT32_MAX };
        CHECK(!w32_object_call(t, &miss) && miss.error == 6);
        for (uint32_t view_base = creator_view, k = 0; k < 2; view_base = opened_view, k++) {
            nv = (struct ciuki_w32_view_args) { .action = W32_VIEW_UNMAP, .address = view_base };
            CHECK(!w32_view_call(t, &nv) && !nv.error);
        }
        CHECK(!w32_handle_close(p, ev.result) && !w32_handle_close(p, open.result)
            && !w32_handle_close(p, named_sec.result));
    }
    va = (struct ciuki_w32_view_args) {
        .action = W32_VIEW_MAP, .section = section, .bytes = 4096, .offset_lo = 1, .access = W32_FILE_MAP_READ
    };
    CHECK(!w32_view_call(t, &va) && va.error == 87);
    struct ciuki_w32_vm protect = request(base, 4096, 0, W32_PAGE_READONLY);
    CHECK(!w32_vm_protect(p->memory, &protect) && protect.old_protect == 4
        && w32_view_access(p->memory, base, true) == -EFAULT);
    protect = request(base, 4096, 0, W32_PAGE_READWRITE);
    CHECK(!w32_vm_protect(p->memory, &protect));
    /* Builtin pages share identity until the first write. Data copies survive
     * later reads and VirtualProtect changes without reverting to originals. */
    CHECK(
        !w32_view_access(p->memory, 0x70011000, false) && !w32_view_access(other->memory, 0x70011000, false));
    CHECK(
        !w32_view_access(p->memory, 0x70010000, false) && !w32_view_access(other->memory, 0x70010000, false));
    CHECK((*pte(p->memory, 0x70010000, false) & ~4095u) == (*pte(other->memory, 0x70010000, false) & ~4095u)
        && w32_view_access(p->memory, 0x70010000, true) == -EFAULT);
    uint32_t *pp = pte(p->memory, 0x70011000, false), *op = pte(other->memory, 0x70011000, false);
    CHECK((*pp & ~4095u) == (*op & ~4095u) && !(*pp & PTE_W));
    interleave_thread = t;
    on_preempt_point = cow_protect_interleave;
    protect = request(0x70010000, 3 * 4096, 0, W32_PAGE_EXECUTE_READWRITE);
    CHECK(!w32_vm_protect(p->memory, &protect));
    on_preempt_point = 0;
    CHECK(!(*pte(p->memory, 0x70010000, false) & PTE_W));
    uint32_t header_word = 0x12345678;
    CHECK(!ua_write(p->memory, 0x70010000, &header_word, 4) && !ua_read(other->memory, &read, 0x70010000, 4)
        && read != header_word);
    CHECK((*pte(p->memory, 0x70010000, false) & ~4095u) != (*pte(other->memory, 0x70010000, false) & ~4095u));
    value = 0x90;
    CHECK(!ua_write(p->memory, 0x70011000, &value, 4));
    CHECK((*pp & ~4095u) != (*op & ~4095u));
    CHECK(!ua_read(other->memory, &read, 0x70011000, 4) && read != value);
    CHECK(!ua_read(p->memory, &read, 0x70011000, 4) && read == value);
    memset(&model_file, 0, sizeof(model_file));
    memset(&model_node, 0, sizeof(model_node));
    memset(model_data, 0, sizeof(model_data));
    model_node.kind = PX_FILE;
    model_node.space = &model_space;
    model_space.vfs = &model_vfs;
    model_node.entry.size = sizeof(model_data);
    model_file.node = &model_node;
    model_file.flags = O_RDWR;
    model_file.object = (struct proc_object) { model_file_retain, model_file_release, false };
    model_file_refs = 1;
    p->fds[10].object = &model_file.object;
    a = (struct ciuki_w32_object_args) {
        .action = W32_OBJ_CREATE, .type = W32_OBJ_FILE, .source = 10, .access = 3
    };
    CHECK(!w32_object_call(t, &a) && !a.error);
    sec = (struct ciuki_w32_section_args) {
        .kind = W32_SEC_FILE, .file = a.result, .bytes = sizeof(model_data), .protect = W32_PAGE_READWRITE
    };
    CHECK(!w32_section_call(t, &sec) && !sec.error);
    uint32_t file_section = sec.result;
    so = w32_lookup(t, file_section, 0);
    CHECK(!w32_handle_install(other, so, 0, false, &peer_section));
    interleave_file_section = peer_section;
    on_preempt_point = writer_interleave;
    va = (struct ciuki_w32_view_args) {
        .action = W32_VIEW_MAP, .section = file_section, .bytes = 4096, .access = W32_FILE_MAP_WRITE
    };
    CHECK(!w32_view_call(t, &va) && !va.error);
    uint32_t file_base = va.address;
    CHECK(writer_refusals == 1 && !on_preempt_point);
    interleave_file_base = file_base;
    model_io_interleave = view_io_interleave;
    tf.vector = 14;
    tf.err = 2;
    CHECK(w32_fault(&tf, file_base) && !model_io_interleave && !t->in_syscall);
    va = (struct ciuki_w32_view_args) {
        .action = W32_VIEW_MAP, .section = peer_section, .bytes = 4096, .access = W32_FILE_MAP_WRITE
    };
    CHECK(!w32_view_call(peer, &va) && va.error == 5);
    va.access = W32_FILE_MAP_READ;
    CHECK(!w32_view_call(peer, &va) && !va.error);
    uint32_t file_peer = va.address;
    value = 0xabcdef;
    CHECK(!ua_write(p->memory, file_base, &value, 4) && !ua_read(other->memory, &read, file_peer, 4)
        && read == value);
    model_io_error = true;
    va = (struct ciuki_w32_view_args) { .action = W32_VIEW_UNMAP, .address = file_base };
    CHECK(!w32_view_call(t, &va) && va.error && w32_view_contains(p->memory, file_base));
    model_io_error = false;
    va.action = W32_VIEW_FLUSH;
    CHECK(!w32_view_call(t, &va) && !va.error);
    memcpy(&read, model_data, 4);
    CHECK(read == value);
    value = 5678;
    size_t done;
    CHECK(!file_io_locked(&model_file, &value, 4, true, true, 0, &done) && done == 4
        && !ua_read(other->memory, &read, file_peer, 4) && read == value);
    va = (struct ciuki_w32_view_args) {
        .action = W32_VIEW_MAP, .section = peer_section, .bytes = 4096, .access = W32_FILE_MAP_COPY
    };
    CHECK(!w32_view_call(peer, &va) && !va.error);
    uint32_t copybase = va.address;
    value = 0xbeef;
    CHECK(!ua_write(other->memory, copybase, &value, 4) && !ua_read(p->memory, &read, file_base, 4)
        && read != value);
    /* Independent sections of one file share bytes while preserving their
     * own names and protection; mapped stores and native writes stay coherent. */
    sec = (struct ciuki_w32_section_args) {
        .kind = W32_SEC_FILE, .file = a.result, .bytes = 4096, .protect = 2
    };
    uint32_t imported = 0;
    for (unsigned c = 0; c < 64; c++)
        if (p->w32->chunks[c])
            for (unsigned i = 0; i < 64; i++)
                if (p->w32->chunks[c][i].object && p->w32->chunks[c][i].object->type == W32_OBJ_FILE)
                    imported = (c * 64 + i + 1) << 2;
    sec.file = imported;
    CHECK(!w32_section_call(t, &sec) && !sec.error);
    struct w32_object *alias = w32_lookup(t, sec.result, 0);
    CHECK(alias != so && alias->u.section == so->u.section && alias->section_protect == 2);
    model_file.flags = O_RDONLY;
    sec = (struct ciuki_w32_section_args) {
        .kind = W32_SEC_FILE, .file = imported, .bytes = 4096, .protect = W32_PAGE_WRITECOPY
    };
    CHECK(!w32_section_call(t, &sec) && !sec.error);
    va = (struct ciuki_w32_view_args) {
        .action = W32_VIEW_MAP, .section = sec.result, .bytes = 4096, .access = W32_FILE_MAP_WRITE
    };
    CHECK(!w32_view_call(t, &va) && va.error == 5);
    va.access = W32_FILE_MAP_COPY;
    CHECK(!w32_view_call(t, &va) && !va.error);
    va.action = W32_VIEW_UNMAP;
    CHECK(!w32_view_call(t, &va) && !va.error);
    model_file.flags = O_RDWR;
    /* Sparse map and COW allocation failure rollback retains every counter. */
    struct w32_account stable;
    w32_snapshot(&stable);
    unsigned stable_pages = pages_used, stable_heap = heap_blocks;
    int32_t stable_promises[MM_OWNERS][2];
    memcpy(stable_promises, promises, sizeof(promises));
    for (int fail = 0; fail < 24; fail++) {
        va = (struct ciuki_w32_view_args) {
            .action = W32_VIEW_MAP, .section = section, .bytes = 65536, .access = W32_FILE_MAP_COPY
        };
        alloc_fail = fail;
        w32_view_call(t, &va);
        alloc_fail = -1;
        if (!va.error) {
            va.action = W32_VIEW_UNMAP;
            CHECK(!w32_view_call(t, &va) && !va.error);
        }
        struct w32_account now;
        w32_snapshot(&now);
        CHECK(!memcmp(&now, &stable, sizeof(now)) && pages_used == stable_pages && heap_blocks == stable_heap
            && !memcmp(promises, stable_promises, sizeof(promises)));
    }
    struct w32_object *preparing = w32_object_new(W32_OBJ_SECTION);
    CHECK(preparing);
    uint32_t preparing_handle;
    CHECK(!w32_handle_install(p, preparing, 0, false, &preparing_handle));
    va = (struct ciuki_w32_view_args) {
        .action = W32_VIEW_MAP, .section = preparing_handle, .bytes = 4096, .access = W32_FILE_MAP_READ
    };
    CHECK(!w32_view_call(t, &va) && va.error == 170);
    CHECK(!w32_handle_close(p, preparing_handle));
    w32_release(preparing);
    /* Full handle and global-object limits, without conflating references
     * with distinct objects. CLOSE_SOURCE still closes on target failure. */
    struct process *limited;
    CHECK(!proc_prepare(p, &limited));
    limited->personality = PROC_WIN32;
    limited->memory->win32 = true;
    CHECK(!w32_process_init(limited));
    uint32_t *limit_handles = calloc(4096, 4);
    CHECK(limit_handles);
    for (unsigned i = 0; i < 4096; i++)
        CHECK(!w32_handle_install(limited, event_object, 3, false, &limit_handles[i]));
    uint32_t excess;
    CHECK(w32_handle_install(limited, event_object, 0, false, &excess) == 4);
    struct proc_thread limit_thread = { .process = limited };
    CHECK(w32_lookup(&limit_thread, limit_handles[4095], 0) == event_object);
    proc_discard(limited);
    free(limit_handles);
    struct w32_object **pool = calloc(8192, sizeof(*pool));
    CHECK(pool);
    unsigned used = 0;
    while (w32_accounts.objects < 8192) {
        pool[used] = w32_object_new(W32_OBJ_WINSTATION);
        CHECK(pool[used]);
        used++;
    }
    CHECK(!w32_object_new(W32_OBJ_EVENT));
    while (used)
        w32_release(pool[--used]);
    free(pool);
    printf("w32 budget: object=%zu process=%zu thread=%zu metadata=%u queue_10000=%u shared_pages=%u "
           "COW_credits=%u\n",
        sizeof(struct w32_object), sizeof(struct w32_process), sizeof(struct w32_thread),
        w32_accounts.metadata_bytes, other->w32->private_bytes, w32_accounts.pages, w32_accounts.cow_credits);
    t->task->fpu_area = 0;
    fpu_task_release(t->task);
    g_current = &controller;
    proc_stop(other, (int32_t)0xdeadbeef, 0);
    proc_collect();
    a = (struct ciuki_w32_object_args) { .action = W32_OBJ_QUERY, .handle = process_handle };
    CHECK(!w32_object_call(t, &a) && !a.error && a.result == 0xdeadbeef);
    proc_stop(p, 0, 0);
    proc_collect();
    proc_set_file_ops(0);
    CHECK(!model_file_refs);
    struct w32_account after;
    w32_snapshot(&after);
    CHECK(
        !memcmp(&after, &empty, sizeof(after)) && pages_used == initial_pages && heap_blocks == initial_heap);
    CHECK(!memcmp(promises, initial_promises, sizeof(promises)) && !mapping_count && !shared_mapping_count);
    /* Exact production teardown, including sparse queues, named objects,
     * reservations, COW credits, shared sections and inherited handles. */
    for (unsigned cycle = 0; cycle < 100; cycle++) {
        struct process *live;
        struct proc_thread *lt = object_process(proc_supervisor(), &live);
        object_create(lt, W32_OBJ_EVENT, 1, 1);
        object_create(lt, W32_OBJ_MUTEX, 1, 0);
        CHECK(!w32_apc_queue(lt, 0x401000, cycle, 0, 0));
        struct ciuki_w32_message_args pending_message
            = { .action = W32_MSG_POST, .tid = lt->tid, .message = { .message = 0x400 } };
        CHECK(!w32_message_call(lt, &pending_message) && !pending_message.error);
        sec = (struct ciuki_w32_section_args) { .kind = W32_SEC_PAGEFILE, .bytes = 65536, .protect = 4 };
        CHECK(!w32_section_call(lt, &sec) && !sec.error);
        va = (struct ciuki_w32_view_args) {
            .action = W32_VIEW_MAP, .section = sec.result, .bytes = 4096, .access = 2
        };
        CHECK(!w32_view_call(lt, &va) && !va.error);
        CHECK(!ua_write(live->memory, va.address, &cycle, 4));
        proc_stop(live, 0, 0);
        proc_collect();
        w32_snapshot(&after);
        CHECK(!memcmp(&after, &empty, sizeof(after)) && pages_used == initial_pages
            && heap_blocks == initial_heap && !memcmp(promises, initial_promises, sizeof(promises))
            && !mapping_count && !shared_mapping_count);
    }
    puts("w32 objects: atomic waits/order/pulse/timeouts, abandonment, 16-bit handle values/reuse/duplicate/inherit/names, "
         "APC FIFO/user frames, suspend/FXSAVE, 10000 quota, COW/coherence/flush errors, 100 teardown cycles "
         "PASS");
}
