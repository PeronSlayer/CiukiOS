#ifdef APP_GATE_KHEAP_LEDGER_TEST
#include <stdio.h>
#include <stdlib.h>
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
/* Replace only privileged IRQ instructions and the physical direct map.
 * Allocation, free-list reuse and ledger updates are production kheap.c. */
#define CIUKI_CPU_H
static _Alignas(PAGE_SIZE) uint8_t heap_pages[33][PAGE_SIZE];
static unsigned allocated_pages, page_limit = 32, irq_depth;
#define P2V(p) ((void *)heap_pages[(p) / PAGE_SIZE - 1])
static uint32_t irq_save(void) { return irq_depth++; }
static void irq_restore(uint32_t f) { if (irq_depth != f+1) abort(); irq_depth=f; }
uint32_t pmm_alloc(void) { return allocated_pages == page_limit ? 0 : ++allocated_pages * PAGE_SIZE; }
void panic(const char *fmt, ...) { (void)fmt; abort(); }
#include "../../../src/kernel/core/kheap.c"
static void check_heap(unsigned pages, size_t bytes, size_t high)
{
    struct kheap_ledger first, second;
    kheap_snapshot(&first); kheap_snapshot(&second);
    if (first.pages != pages || first.in_use != bytes || first.peak != high ||
        second.pages != pages || second.in_use != bytes || second.peak != high ||
        kheap_in_use() != bytes || irq_depth || allocated_pages != pages) abort();
}
int main(void)
{
    kheap_init(); check_heap(0,0,0);
    void *blocks[64];
    for (unsigned i=0; i<64; i++) {
        blocks[i]=kmalloc(2040); if (!blocks[i]) abort();
        check_heap((i+2)/2,(i+1)*2048,(i+1)*2048);
    }
    if (kmalloc(2040) || kmalloc(2041)) abort();
    check_heap(32,131072,131072);
    for (unsigned i=0; i<64; i++) kfree(blocks[i]);
    kfree(0); check_heap(32,0,131072);
    void *reused=kmalloc(2040); if (!reused) abort();
    check_heap(32,2048,131072); kfree(reused);
    page_limit++;
    uint8_t *small=kzalloc(24); if (!small) abort();
    for (unsigned i=0; i<24; i++) if (small[i]) abort();
    check_heap(33,32,131072); kfree(small); check_heap(33,0,131072);
    puts("heap ledger: retained_pages=33 bytes_in_use=0 peak=131072 refill_failure=unchanged reuse=ok");
    return 0;
}
#elif defined(APP_GATE_NAMESPACE_TEARDOWN_TEST)
#include <stdio.h>
#include <stdlib.h>
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <ciuki/kernel.h>
#include <ciuki/files.h>
static unsigned allocations, freed_nodes;
void *kzalloc(size_t bytes) { void *p = calloc(1,bytes); if (p) allocations++; return p; }
void kfree(void *p) { if (p) { if (!allocations) abort(); allocations--; freed_nodes++; free(p); } }
void panic(const char *fmt, ...) { (void)fmt; abort(); }
bool clock_fat_utc(uint16_t date, uint16_t time, int64_t *seconds)
{ (void)date; (void)time; *seconds=0; return false; }
int fat_unmount(struct fat_volume *volume) { volume->mounted=false; return 0; }
#include "../../../src/kernel/proc/posixpath.c"
int main(void)
{
    struct vfs vfs; struct fat_volume volume = {.mounted=true,.type=32,.root=2};
    struct px_namespace *space;
    vfs_init(&vfs);
    if (vfs_attach(&vfs,2,&volume) || files_attach(&vfs,&space)) abort();
    struct file_ledger before, after, detached;
    files_snapshot(space,&before);
    /* Same node creation/reuse used by a file lookup in the gate; no open
     * references survive. Repeated lookups must reuse the named identity. */
    struct fat_entry entry = {.parent=2,.index=7};
    struct px_node *named = node_entry(space,space->root,&entry);
    if (!named || named->refs || node_entry(space,space->root,&entry) != named) abort();
    files_snapshot(space,&after);
    if (after.nodes != before.nodes+1 || after.pins || after.descriptions) abort();
    if (vfs_detach(&vfs,2)) abort();
    files_snapshot(space,&detached);
    /* Current volume detach expires nodes. Actual allocation release is
     * in files_detach via vfs_destroy, after every cwd pin is dropped. */
    if (named->linked || detached.nodes != after.nodes || !allocations) abort();
    unsigned retained=detached.nodes, released=freed_nodes;
    vfs_destroy(&vfs);
    if (spaces || allocations || freed_nodes-released != retained+1) abort();
    printf("namespace teardown: gate_nodes_before=%u gate_nodes_after=%u detached_nodes=%u freed_nodes=%u live_allocations=%u\n",
           before.nodes,after.nodes,retained,freed_nodes-released-1,allocations);
    return 0;
}
#else
/* Fake Lua scheduling; records/captures/parsing are the production C code.
 * This is host controller validation, never guest application evidence. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/supervisor.h>
#include <ciuki/storage.h>
#include <ciuki/probe.h>
#ifdef APP_GATE_KERNEL_STACK_TEST
#include <pthread.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <ucontext.h>
#endif

struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
static struct process ps[8];
static struct uaddr memory[8];
static struct ua_extent stacks[8];
static struct fb_device framebuffer;
static struct desktop_activity activity;
static struct proc_thread app_thread;
static struct task app_task;
static struct process *running, *server;
static unsigned seq, mode, run_number, run_step, next_pid=1;
static const char *directory;
static uint32_t server_ticks;
static struct storage test_storage;
static struct px_namespace test_space;
static struct px_node test_nodes[60];
static struct fat_volume test_volume;
static bool ledger_changed;
static struct file_description ledger_description;
static struct proc_thread ledger_thread;
static bool identity_growth(void) { return mode == 21 || mode == 22 || mode == 28 || mode == 35; }

void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    char line[256];
    int n = ksnprintf(line,sizeof(line),"CIUKI_TEST v=1 run=12345678 seq=%06u probe=%s event=%s",++seq,probe,event);
    if (fmt) {
        line[n++] = ' ';
        va_list ap; va_start(ap,fmt); n += kvsnprintf(line+n,sizeof(line)-(size_t)n,fmt,ap); va_end(ap);
    }
    if (n>240) { fprintf(stderr,"oversize record: %d %s\n",n,line); exit(2); }
    puts(line);
}
struct process *proc_supervisor(void) { return &ps[0]; }
struct process *proc_find(uint32_t pid)
{
    for (unsigned i=0;i<8;i++) if (ps[i].pid==pid) return &ps[i];
    return 0;
}
int proc_prepare(struct process *parent, struct process **out)
{
    for (unsigned i=1;i<8;i++) if (!ps[i].pid) {
        ps[i]=(struct process){.pid=++next_pid,.ppid=parent->pid,.state=PROC_PREPARING};
        *out=&ps[i]; return 0;
    }
    return -ENOMEM;
}
void proc_publish(struct process *p, uint32_t flags)
{
    p->published=true; p->state=PROC_LIVE; p->pgid=flags ? p->pid : p->ppid;
}
void proc_stop(struct process *p, int code, uint32_t signal)
{
    p->status=signal ? (int)signal : code<<8; p->state=PROC_ZOMBIE; p->memory=0;
}
int proc_reap(struct process *parent, struct process *p)
{
    if (p->ppid!=parent->pid || p->state!=PROC_ZOMBIE) abort();
    if (running==p) running=0;
    memset(p,0,sizeof(*p)); return 0;
}
void proc_collect(void)
{
    for (unsigned i=1;i<8;i++) if (ps[i].state==PROC_ZOMBIE && ps[i].ppid==1) proc_reap(&ps[0],&ps[i]);
}
void proc_snapshot(struct proc_ledger *l)
{
    memset(l,0,sizeof(*l));
    for (unsigned i=0;i<8;i++) if (ps[i].pid) { l->processes++; l->zombies+=ps[i].state==PROC_ZOMBIE; }
    if (ledger_changed && mode == 27) l->tables++;
}
void desktop_snapshot(struct desktop_ledger *l) { memset(l,0,sizeof(*l)); if (ledger_changed && mode == 29) l->grants++; }
void desktop_activity_snapshot(struct desktop_activity *a) { *a=activity; }
const struct fb_device *fbdev_get(void) { return &framebuffer; }
struct storage *storage_get(void) { return &test_storage; }
int storage_enable_write(struct storage *s, unsigned drive) { (void)s; return drive==2 && mode!=13 ? 0 : -EIO; }
struct proc_thread *proc_thread_for(const struct task *task) { return task==&app_task ? &app_thread : 0; }
int ua_read(const struct uaddr *u, void *dst, uint32_t va, uint32_t bytes)
{
    (void)u;
    if (va!=CIUKI_IMAGE_BASE+CIUKI_PAGE_SIZE+16 || bytes!=4) abort();
    memcpy(dst,&server_ticks,4); return 0;
}
static int file_read(void *cookie, uint32_t offset, void *dst, uint32_t bytes)
{
    FILE *file=cookie;
    return fseek(file,offset,SEEK_SET) || fread(dst,1,bytes,file)!=bytes ? -EIO : 0;
}
static void file_close_snapshot(void *cookie) { fclose(cookie); }
static int file_open_snapshot(void *cwd, const char *path, struct ciuki_file *out)
{
    (void)cwd;
    if (!strcmp(path,"/bin/desktop")) { *out=(struct ciuki_file){0}; return 0; }
    char host[1024]; snprintf(host,sizeof(host),"%s%s",directory,path);
    FILE *file=fopen(host,"rb"); if (!file) return -ENOENT;
    fseek(file,0,SEEK_END); long size=ftell(file);
    *out=(struct ciuki_file){.cookie=file,.bytes=(uint32_t)size,.read=file_read,.close=file_close_snapshot}; return 0;
}
const struct ciuki_file_ops *proc_get_file_ops(void)
{
    static const struct ciuki_file_ops ops={.open=file_open_snapshot}; return &ops;
}
uint32_t pmm_free_count(void) {
    if (ledger_changed && (mode == 28 || mode == 35)) return 968;
    if (ledger_changed && mode == 36) return 999;
    return mode==18 && run_number==2 ? 999 : 1000;
}
size_t kheap_in_use(void) {
    return 1024 + (ledger_changed && identity_growth() ? gate_identity_size() : 0) +
        (ledger_changed && mode == 22 ? 32 : 0);
}
void kheap_snapshot(struct kheap_ledger *l) {
    *l=(struct kheap_ledger){.pages=64,.in_use=kheap_in_use(),.peak=4096};
    if (ledger_changed && (mode == 28 || mode == 35)) {
        l->pages += mode == 28 ? 32 : 31;
        l->peak=131072;
    }
    if (ledger_changed && mode == 37) l->pages++;
}
uint32_t file_description_count(void) { return 2 + (ledger_changed && mode == 30); }
void files_snapshot(struct px_namespace *space, struct file_ledger *l) { (void)space; memset(l,0,sizeof(*l)); }
struct proc_thread *proc_thread_slot(unsigned slot) {
    if (!ledger_changed || slot || mode < 32 || mode > 34) return 0;
    ledger_thread.stopped = mode != 32; ledger_thread.retained = mode == 33;
    ledger_thread.word_wait.queued = mode == 34;
    return &ledger_thread;
}
int supervisor_spawn(const char *path, const char *const argv[], const char *cwd, bool desktop, struct process **out)
{
    if (mode==14 && !desktop) return -ENOENT;
    int err=proc_prepare(&ps[0],out); if (err) return err;
    struct process *p=*out;
    unsigned slot=(unsigned)(p-ps);
    stacks[slot]=(struct ua_extent){.base=CIUKI_MAIN_STACK_LIMIT-65536,.end=CIUKI_MAIN_STACK_LIMIT,.kind=UA_STACK};
    memory[slot]=(struct uaddr){.heap_base=0x100000,.brk=0x100000,.as={.pages=20},.backing=20,.head=&stacks[slot]};
    p->memory=&memory[slot]; proc_publish(p,CIUKI_SPAWN_NEW_GROUP);
    if (desktop) {
        if (strcmp(path,"/bin/desktop") || strcmp(argv[0],"desktop") || argv[1] || cwd) abort();
        server=p;
    } else {
        if (strcmp(path,"/bin/lua") || strcmp(cwd,"/system/tests/lua-5.4.8-tests") || strcmp(argv[0],"lua")) abort();
        run_number++;
        if (run_number==1) { if (strcmp(argv[1],"-e") || strcmp(argv[2],"_U=true") || strcmp(argv[3],"all.lua") || argv[4]) abort(); }
        else if (strcmp(argv[1],"ciuki-f2.lua") || argv[2]) abort();
        running=p; run_step=0;
        app_thread=(struct proc_thread){.tid=17+run_number,.process=p};
    }
    return (int)p->pid;
}
int supervisor_spawn_standin(struct process *owner, struct process **out)
{
    const char *argv[]={"desktop",0};
    int result=supervisor_spawn("/bin/desktop",argv,0,true,out); (*out)->ppid=owner->pid; return result;
}
static void output(unsigned stream, const char *text)
{
    /* Exercise tokens split across writes; the real observer binds the task. */
    while (*text) { unsigned n=(unsigned)strlen(text); if (n>7) n=7;
        if (!supervisor_output(&app_task,stream,text,n)) abort(); text+=n; }
}
void task_sleep_ms(uint32_t ms)
{
    g_ticks+=ms; server_ticks+=(uint32_t)ms;
    if (server && server->state==PROC_LIVE) activity.presents++;
    if (!running || running->state!=PROC_LIVE) return;
    if (mode==10 && run_number==1) { g_ticks+=900000; return; }
    run_step++;
    running->memory->brk=running->memory->heap_base+524288;
    running->memory->as.pages=running->memory->backing=148;
    if (run_step==1) {
        if (run_number==1) {
            /* Nonzero call-3 report offset must not shift console offsets. */
            if (!supervisor_report(&app_task,"case=lua-host-fixture",20)) abort();
            if (mode==7) { static struct task foreign; if (supervisor_output(&foreign,1,"assertion failed!",17)) abort(); }
            output(1,"Starting Tests\n");
            /* Fake application output has no place on the controller's
             * stack: the guest obtains it from the application's memory. */
            if (mode==3) { static char padding[2600]; memset(padding,'x',sizeof(padding)-1); padding[sizeof(padding)-1]=0;
                output(1,padding); output(1,"assertion failed!"); output(1,padding); }
            if (mode!=2) output(1,"total time: 2.00s (wall time: 3s)\nfinal OK !!!\n");
            if (mode==4) output(1,"final OK\n");
        } else {
            output(1,"case=file-roundtrip ok=1 bytes=65536 rewritten=4096 fnv1a32=f6671c1c rename=1 remove=1\n");
            if (mode!=6) output(1,mode==5 ? "case=allocation ok=0 error=assertion_failed\n" :
                "case=allocation ok=1 cycles=100 live_strings=4096 string_bytes=128 reference_errors=0\n");
            output(1,"case=time-utc ok=1 dates=2 epoch_2000=951782400 epoch_2040=2208988800 clock_nonnegative=1\n");
            output(1,"ciuki-f2 stdout 1\n"); output(2,"ciuki-f2 stderr 2\n");
            output(1,"ciuki-f2 stdout 3\n"); output(2,"ciuki-f2 stderr 4\n");
            output(1,"case=console ok=1 lines=4 ordered=1\n");
        }
    }
    if (run_step==3) {
        if (mode==19 && run_number==2) g_ticks+=900000;
        int code=mode==1 && run_number==1 ? 37 : 0;
        if (run_number == 2) {
            ledger_changed = true;
            if (identity_growth() || mode == 26) {
                test_nodes[59] = (struct px_node){.space=&test_space,.volume=&test_volume,.linked=mode != 26};
                test_nodes[58].next = &test_nodes[59];
            }
            if (mode == 23) test_storage.cache.blocks++;
            if (mode == 24) test_storage.cache.workspace_pages++;
            if (mode == 25) test_nodes[0].refs++;
            if (mode == 31) test_space.descriptions = &ledger_description;
        }
        proc_stop(running,code,mode==11 && run_number==1 ? SIGSEGV : 0);
        if (mode==11 && run_number==1) running->fault_vector=14;
        if (mode==8 && run_number==1) proc_stop(server,1,0);
    }
}
static int gate_test_run(void)
{
    if (mode==16) {
        rec_emit("app-gate","BEGIN",0);
        struct gate_memory peak={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
        gate_memory_record("lua-supplement",UINT32_MAX,&peak);
        struct proc_ledger p; struct desktop_ledger d; struct gate_resources r;
        memset(&p,255,sizeof(p)); memset(&d,255,sizeof(d)); memset(&r,255,sizeof(r));
        static char ledger[1024];
        unsigned used = (unsigned)ksnprintf(ledger,sizeof(ledger),"{\"baseline\":");
        used += gate_ledger_format(ledger+used,sizeof(ledger)-used,&p,&d,&r);
        used += (unsigned)ksnprintf(ledger+used,sizeof(ledger)-used,",\"final\":");
        used += gate_ledger_format(ledger+used,sizeof(ledger)-used,&p,&d,&r);
        used += (unsigned)ksnprintf(ledger+used,sizeof(ledger)-used,",\"restored\":1}");
        if (used >= sizeof(ledger)) abort();
        gate_metadata("resource_ledgers","json",ledger);
        ps[0].pid=UINT32_MAX; app_thread.process=&ps[0]; app_thread.tid=UINT32_MAX;
        supervisor_observe("app-gate",UINT32_MAX);
        static char text[3073]; memset(text,'x',sizeof(text)-1); text[sizeof(text)-1]=0;
        output(1,text);
        if (!supervisor_report(&app_task,text,240)) abort();
        supervisor_observe_end(); rec_emit("app-gate","END","status=PASS");
        return 0;
    }
    fs_lock_init(&test_storage.cache.lock); fs_lock_init(&test_storage.vfs.lock);
    test_storage.cache.blocks = 13440; test_storage.cache.workspace_pages = 128;
    test_space.vfs = &test_storage.vfs; test_space.nodes = test_nodes;
    for (unsigned i = 0; i < 59; i++) {
        test_nodes[i] = (struct px_node){.space=&test_space,.volume=&test_volume,.linked=true,
                                       .next=i == 58 ? 0 : &test_nodes[i+1]};
    }
    test_nodes[0].refs = 1;
    ps[0]=(struct process){.pid=1,.state=PROC_LIVE,.cwd=&test_nodes[0]}; framebuffer.present=mode!=12;
    if (mode==12) g_boot.flags=CBI_F_SAFE_MODE;
    (void)probe_f2_app_gate();
    if (running || ps[1].pid || ps[2].pid || ps[3].pid) { fputs("controller leaked processes\n",stderr); return 3; }
    fs_lock_destroy(&test_storage.cache.lock); fs_lock_destroy(&test_storage.vfs.lock);
    return 0;
}

#ifdef APP_GATE_KERNEL_STACK_TEST
/* Linux rejects pthread_attr_setstacksize(8192): PTHREAD_STACK_MIN is at
 * least 16384. Use a minimum-sized pthread for bootstrap/TLS, then execute
 * the actual probe (including metadata and both fake Lua runs) in that
 * thread on an exact KSTACK_SIZE mapping with inaccessible pages around it.
 * This binary is separate from the ASan harness: instrumentation and libc
 * thread startup are not part of the kernel's stack budget.
 * https://man7.org/linux/man-pages/man3/pthread_attr_setstacksize.3.html
 * https://sourceware.org/glibc/manual/latest/html_node/System-V-contexts.html */
static ucontext_t stack_caller, stack_probe;
static uint8_t *stack_mapping;
static size_t stack_guard;
static int stack_result;

/* Negative control: touch every frame, so the compiler cannot optimize away
 * the allocation or jump over the guard in one large subtraction. */
static __attribute__((noinline)) unsigned gate_stack_overflow(unsigned depth)
{
    volatile uint8_t bytes[512];
    for (unsigned i=0;i<sizeof(bytes);i++) bytes[i]=(uint8_t)depth;
    unsigned value=depth ? gate_stack_overflow(depth-1) : 0;
    return value+bytes[depth % sizeof(bytes)];
}
static void gate_stack_entry(void)
{
    uint8_t marker;
    uintptr_t address=(uintptr_t)&marker, bottom=(uintptr_t)stack_mapping+stack_guard;
    if (address < bottom || address >= bottom+KSTACK_SIZE) abort();
    if (mode==20) { (void)gate_stack_overflow(KSTACK_SIZE/512+8); abort(); }
    stack_result=gate_test_run();
}
static void *gate_stack_thread(void *unused)
{
    (void)unused;
    if (getcontext(&stack_probe)) abort();
    stack_probe.uc_stack.ss_sp=stack_mapping+stack_guard;
    stack_probe.uc_stack.ss_size=KSTACK_SIZE;
    stack_probe.uc_stack.ss_flags=0;
    stack_probe.uc_link=&stack_caller;
    makecontext(&stack_probe,gate_stack_entry,0);
    if (swapcontext(&stack_caller,&stack_probe)) abort();
    return 0;
}
static int gate_test_bounded(void)
{
    /* Overflow controls must not leave a core dump in the worktree. */
    const struct rlimit no_core={0,0};
    if (setrlimit(RLIMIT_CORE,&no_core)) abort();
    unsigned long page=getauxval(AT_PAGESZ);
    if (!page || KSTACK_SIZE % (size_t)page) abort();
    stack_guard=(size_t)page;
    size_t bytes=KSTACK_SIZE+2*stack_guard;
    stack_mapping=mmap(0,bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (stack_mapping==MAP_FAILED || mprotect(stack_mapping+stack_guard,KSTACK_SIZE,PROT_READ|PROT_WRITE)) abort();
    memset(stack_mapping+stack_guard,0xa5,KSTACK_SIZE);
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) || pthread_attr_setguardsize(&attr,stack_guard)) abort();
    int err=pthread_attr_setstacksize(&attr,KSTACK_SIZE);
    if (err && (err!=EINVAL || pthread_attr_setstacksize(&attr,PTHREAD_STACK_MIN))) abort();
    pthread_t thread;
    if (pthread_create(&thread,&attr,gate_stack_thread,0) || pthread_attr_destroy(&attr) ||
        pthread_join(thread,0)) abort();
    size_t untouched=0;
    while (untouched<KSTACK_SIZE && stack_mapping[stack_guard+untouched]==0xa5) untouched++;
    fprintf(stderr,"app-gate stack: usable=%u guard=%zu high_water=%zu\n",KSTACK_SIZE,stack_guard,KSTACK_SIZE-untouched);
    if (munmap(stack_mapping,bytes)) abort();
    return stack_result;
}
#endif

int main(int argc, char **argv)
{
    if (argc!=3) return 2;
    directory=argv[1]; mode=(unsigned)strtoul(argv[2],0,10);
#ifdef APP_GATE_KERNEL_STACK_TEST
    return gate_test_bounded();
#else
    return gate_test_run();
#endif
}

#endif
