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

void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    char line[1024];
    int n = snprintf(line,sizeof(line),"CIUKI_TEST v=1 run=12345678 seq=%06u probe=%s event=%s",++seq,probe,event);
    if (fmt) {
        line[n++] = ' ';
        va_list ap; va_start(ap,fmt); n += vsnprintf(line+n,sizeof(line)-(size_t)n,fmt,ap); va_end(ap);
    }
    if (n>240) { fprintf(stderr,"oversize record: %d %s\n",n,line); exit(2); }
    puts(line);
}
int ksnprintf(char *dst, size_t capacity, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt); int n=vsnprintf(dst,capacity,fmt,ap); va_end(ap); return n;
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
}
void desktop_snapshot(struct desktop_ledger *l) { memset(l,0,sizeof(*l)); }
void desktop_activity_snapshot(struct desktop_activity *a) { *a=activity; }
const struct fb_device *fbdev_get(void) { return &framebuffer; }
struct storage *storage_get(void) { return 0; }
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
uint32_t pmm_free_count(void) { return mode==18 && run_number==2 ? 999 : 1000; }
size_t kheap_in_use(void) { return 1024; }
uint32_t file_description_count(void) { return 2; }
void files_snapshot(struct px_namespace *space, struct file_ledger *l) { (void)space; memset(l,0,sizeof(*l)); }
struct proc_thread *proc_thread_slot(unsigned slot) { (void)slot; return 0; }
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
            if (mode==7) { struct task foreign={0}; if (supervisor_output(&foreign,1,"assertion failed!",17)) abort(); }
            output(1,"Starting Tests\n");
            if (mode==3) { char padding[2600]; memset(padding,'x',sizeof(padding)-1); padding[sizeof(padding)-1]=0;
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
        proc_stop(running,code,mode==11 && run_number==1 ? SIGSEGV : 0);
        if (mode==11 && run_number==1) running->fault_vector=14;
        if (mode==8 && run_number==1) proc_stop(server,1,0);
    }
}
int main(int argc, char **argv)
{
    if (argc!=3) return 2;
    directory=argv[1]; mode=(unsigned)strtoul(argv[2],0,10);
    if (mode==16) {
        rec_emit("app-gate","BEGIN",0);
        struct gate_memory peak={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
        gate_memory_record("lua-supplement",UINT32_MAX,&peak);
        struct proc_ledger p; struct desktop_ledger d; struct gate_resources r;
        memset(&p,255,sizeof(p)); memset(&d,255,sizeof(d)); memset(&r,255,sizeof(r));
        char ledger[512];
        if (gate_ledger_format(ledger,sizeof(ledger),&p,&d,&r)>=sizeof(ledger)) abort();
        gate_metadata("resource_ledgers","json",ledger);
        ps[0].pid=UINT32_MAX; app_thread.process=&ps[0]; app_thread.tid=UINT32_MAX;
        supervisor_observe("app-gate",UINT32_MAX);
        char text[3073]; memset(text,'x',sizeof(text)-1); text[sizeof(text)-1]=0;
        output(1,text);
        if (!supervisor_report(&app_task,text,240)) abort();
        supervisor_observe_end(); rec_emit("app-gate","END","status=PASS");
        return 0;
    }
    ps[0]=(struct process){.pid=1,.state=PROC_LIVE}; framebuffer.present=mode!=12;
    if (mode==12) g_boot.flags=CBI_F_SAFE_MODE;
    (void)probe_f2_app_gate();
    if (running || ps[1].pid || ps[2].pid || ps[3].pid) { fputs("controller leaked processes\n",stderr); return 3; }
    return 0;
}
