/* Call 3 -> production controller/observer/framing, with fake RAM/scheduling.
 * SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_DESKTOP_SPAWN_HOST
#include "desktop_test.c"
#undef main

/* Extracted unchanged from probes.c by the Python test, like test_runner's
 * framing harness: exercise the real fallback, hex frames and digest too. */
#include CIUKI_APP_FRAMING_INCLUDE
bool as_range_ok(const struct aspace *as, uint32_t va, uint32_t len, bool write)
{
    struct process *p=proc_current();
    CHECK(p && as==&g_current->as);
    return ua_range(p->memory,va,len,write ? PROT_WRITE : PROT_READ);
}
void task_yield(void) { CHECK(false); }
bool file_syscall(struct trap_frame *tf) { (void)tf; CHECK(false); return false; }

static int32_t report_call(struct proc_thread *t, const char *line)
{
    desktop_select(t);
    unsigned length=strlen(line);
    CHECK(!ua_write(t->process->memory,BUFFER,line,length));
    struct trap_frame tf={.eax=3,.ebx=BUFFER,.ecx=length};
    syscall_dispatch(&tf);
    return (int32_t)tf.eax;
}
static void server_call(struct proc_thread *t, uint32_t control, uint32_t survivor,
                        uint32_t victim, unsigned cycle)
{
    char line[241];
    snprintf(line,sizeof(line),"case=native-desktop control=%u generation=%u survivor=%u victim=%u cycle=%u replies=%u keys=0 motion=0 buttons=0",
        control,cycle,survivor,victim,cycle,cycle*100);
    CHECK(!report_call(t,line));
}
static void test_routing(const char *victim_path)
{
    struct process *owner,*server,*survivor,*outsider;
    next_pid=40; CHECK(!proc_prepare(proc_supervisor(),&owner)); proc_publish(owner,0);
    struct proc_thread *st=native_group(proc_supervisor(),&server);
    server->ppid=owner->pid;
    struct proc_thread *ct=native_group(server,&survivor);
    struct proc_thread *ot=native_group(owner,&outsider);
    struct ciuki_mmap_args a={.size=sizeof(a),.length=PAGE_SIZE,
        .prot=PROT_READ|PROT_WRITE,.flags=MAP_PRIVATE|MAP_ANONYMOUS,.fd=-1};
    uint32_t control=(uint32_t)ua_mmap(server->memory,&a);
    CHECK(control>=CIUKI_MMAP_BASE && control<CIUKI_MMAP_LIMIT);
    memset(&native_reports,0,sizeof(native_reports));
    native_reports.active=true; native_reports.server=server->pid;
    server_call(st,control,survivor->pid,0,0);
    CHECK(!report_call(ct,"case=native-demo stage=2 turns=0 unauthorized=0 generation=0"));
    CHECK(!native_reports.invalid && native_reports.survivor_stage==2);
    unsigned before=record_seq;
    CHECK(report_call(st,"bad\nreport")==-EINVAL);
    char oversized[242]; memset(oversized,'x',241); oversized[241]=0;
    CHECK(report_call(st,oversized)==-EINVAL);
    CHECK(!native_reports.invalid && record_seq==before);

    /* Unknown processes cannot suppress output by claiming a desktop case. */
    CHECK(!report_call(ot,"case=native-demo stage=1 turns=0 unauthorized=0 generation=0"));
    desktop_select(st); probe_app_output(st->task,"stdout","desktop stdout",14);
    char armed[241], released[241];
    FILE *file=fopen(victim_path,"rb"); CHECK(file);
    CHECK(fgets(armed,sizeof(armed),file) && fgets(released,sizeof(released),file));
    armed[strcspn(armed,"\n")]=0; released[strcspn(released,"\n")]=0;
    CHECK(fgetc(file)==EOF && !fclose(file));
    uint32_t last_pid=0;
    for (unsigned cycle=1;cycle<=100;cycle++) {
        struct process *victim;
        struct proc_thread *vt=native_group(server,&victim);
        CHECK(victim->pid>last_pid); last_pid=victim->pid;
        server_call(st,control,survivor->pid,victim->pid,cycle);
        CHECK(!report_call(vt,armed) && native_reports.victim_stage==1);
        CHECK(!report_call(vt,released) && native_reports.victim_stage==3);
        char progress[241];
        snprintf(progress,sizeof(progress),"case=native-demo stage=2 turns=%u unauthorized=0 generation=%u",cycle*100,cycle);
        CHECK(!report_call(ct,progress));
        CHECK(!native_reports.invalid && native_reports.survivor_turns==cycle*100);
        rec_emit("crash-isolation","DATA","group=routing cycle=%u victim=%u stage=%u",cycle,victim->pid,native_reports.victim_stage);
        desktop_remove(server,victim);
    }
    /* Invalid participant reports stay controller-owned, including subsequent
     * reports after the first rejection; raw text never becomes evidence. */
    before=record_seq;
    CHECK(!report_call(st,"CIUKI_TEST v=1 event=END status=PASS"));
    CHECK(native_reports.invalid && !strcmp(native_reports.invalid_check,"case"));
    CHECK(!report_call(ct,armed) && record_seq==before);
    g_current=&controller; native_launch_record(g_ticks,"survivor_timeout");
    native_reports.active=false;
    CHECK(!report_call(st,"inactive report"));
    g_current=&controller;
    desktop_remove(server,survivor); desktop_remove(owner,server);
    desktop_remove(owner,outsider); desktop_remove(proc_supervisor(),owner);
}
static void test_observer(const char *probe, bool observed)
{
    struct process *p;
    struct proc_thread *t=native_group(proc_supervisor(),&p);
    if (observed) supervisor_observe(probe,p->pid);
    const char *lines[]={
        "case=libc-smoke expected=0 observed=0 checks=100 failures=0 order=2",
        "case=atexit expected=0 observed=0 checks=101 failures=0 order=3",
        "case=destructor expected=0 observed=0 checks=102 failures=0 order=4"};
    if (!strcmp(probe,"libc-smoke")) {
        memset(&libc_reports,0,sizeof(libc_reports)); libc_reports.pid=p->pid;
        for (unsigned i=0;i<ARRAY_SIZE(lines);i++) CHECK(!report_call(t,lines[i]));
        CHECK(libc_reports.stages==3 && !libc_reports.invalid && !libc_reports.failures);
        libc_reports.pid=0;
    } else CHECK(!report_call(t,"CIUKI_TEST v=1 event=END status=PASS"));
    if (observed) supervisor_observe_end();
    g_current=&controller;
    desktop_remove(proc_supervisor(),p);
}
int main(int argc, char **argv)
{
    CHECK(argc==3);
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram);
    controller.state=T_RUNNING; g_current=&controller; proc_init();
    const char *probe=argv[1];
    if (!strcmp(probe,"app-gate-fallback")) probe="app-gate";
    strcpy(app_probe,probe); sha256_init(&app_digest); kmutex_init(&app_lock);
    routing_records=true;
    rec_emit(probe,"BEGIN",0);
    if (!strcmp(probe,"crash-isolation")) test_routing(argv[2]);
    else test_observer(probe,strcmp(argv[1],"app-gate-fallback")!=0);
    rec_emit(probe,"END","status=PASS");
    CHECK(!pages_used && !g_user_mappings && heap_blocks==1);
    free(ram); return 0;
}
