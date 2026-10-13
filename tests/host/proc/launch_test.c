/* f2-39: real process/ELF/marker code; deterministic clock and I/O model.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main proc_existing_main
#include "proc_test.c"
#undef main
#include <stdarg.h>

static bool quiet_boot = true;
static unsigned launch_lines;
static char launch_log[4096];
bool console_quiet(void) { return quiet_boot; }
void klog(const char *fmt, ...)
{
    size_t at = strlen(launch_log);
    va_list args; va_start(args,fmt);
    vsnprintf(launch_log+at,sizeof(launch_log)-at,fmt,args); va_end(args);
    strcat(launch_log,"\n"); launch_lines++;
}
static int measured_open(void *cwd, const char *path, struct ciuki_file *file)
{
    (void)cwd; CHECK(!strcmp(path,"/fixture"));
    *file = fixture_file();
    struct elf_header *h = (void *)fixture;
    struct elf_phdr *ph = (void *)(fixture+sizeof(*h));
    h->phnum=1; ph->filesz=3*PAGE_SIZE-7; ph->memsz=3*PAGE_SIZE;
    for (unsigned i=PAGE_SIZE; i<sizeof(fixture); i++) fixture[i]=(uint8_t)(i*7);
    /* The immutable image snapshot reads four pages; a cold sector cache
     * reads 32 sectors. These are distinct from subsequent ELF RAM reads. */
    for (unsigned i=0; i<4; i++) proc_launch_volume_read(PAGE_SIZE,PAGE_SIZE);
    for (unsigned i=0; i<32; i++) proc_launch_disk_read(512);
    proc_launch_dma_read(8192, 2, false); proc_launch_dma_read(8192, 3, true);
    g_ticks += 75;
    return 0;
}
static void measurement(void)
{
    struct process *desktop, *other;
    struct proc_thread *dt=make_process(proc_supervisor(),&desktop);
    make_process(proc_supervisor(),&other);
    fake_desktop_pid=desktop->pid; g_current=dt->task; dt->task->state=T_RUNNING;
    static const struct ciuki_file_ops ops={.open=measured_open}; proc_set_file_ops(&ops);
    g_ticks=1005;
    CHECK(!proc_launch_mark(desktop,CIUKI_LAUNCH_STATUS,0,0));
    quiet_boot=false;
    CHECK(proc_launch_mark(desktop,CIUKI_LAUNCH_STATUS,0,0)==1);
    CHECK(proc_launch_mark(other,CIUKI_LAUNCH_BEGIN,0,1000000)==-EPERM);
    CHECK(proc_launch_mark(desktop,CIUKI_LAUNCH_BEGIN,0,1006000)==-EINVAL);
    CHECK(proc_launch_mark(desktop,CIUKI_LAUNCH_BEGIN,0,1000000)==0);
    uint32_t args=prepare_spawn_args(desktop);
    int pid=proc_spawn(args); CHECK(pid>0);
    struct process *child=proc_find((uint32_t)pid); CHECK(child);
    CHECK(!dt->launch_loading && !desktop->launch_pending);
    struct proc_launch *l=&child->launch;
    CHECK(l->click_us==1000000 && l->spawn_us==1005000 && l->elf_us==1080000);
    CHECK(l->volume_bytes==16384 && l->volume_reads==4 && l->volume_max==4096);
    CHECK(l->disk_bytes==16384 && l->disk_reads==32 && l->disk_max==512);
    CHECK(l->dma_bytes==16384 && l->dma_reads==2 && l->dma_max==8192 && l->dma_bounces==1 && l->dma_ms==5);
    CHECK(l->image_bytes==52+32+3*PAGE_SIZE-7 && l->image_reads==8 && l->image_max==2048);
    uint8_t check[7]; CHECK(!ua_read(child->memory,check,CIUKI_IMAGE_BASE+3*PAGE_SIZE-7,sizeof(check)));
    for (unsigned i=0;i<sizeof(check);i++) CHECK(!check[i]); /* BSS, including tail of batch */
    CHECK(proc_launch_mark(desktop,CIUKI_LAUNCH_FRAME,(uint32_t)pid,0)==-EINVAL);
    CHECK(proc_launch_mark(desktop,CIUKI_LAUNCH_DAMAGE,(uint32_t)pid,0)==-EINVAL);
    CHECK(proc_launch_mark(other,CIUKI_LAUNCH_FRAME,(uint32_t)pid,0)==-EPERM);
    CHECK(proc_launch_mark(desktop,CIUKI_LAUNCH_CONFIGURE,other->pid,0)==-ESRCH);
    g_ticks=1090; proc_launch_surface(child);
    g_ticks=1091; CHECK(!proc_launch_mark(desktop,CIUKI_LAUNCH_CONFIGURE,(uint32_t)pid,0));
    g_ticks=1092; CHECK(!proc_launch_mark(desktop,CIUKI_LAUNCH_CONFIGURE,(uint32_t)pid,0));
    CHECK(l->configure_us==1091000);
    struct task *saved=g_current;
    for (unsigned i=0;i<CIUKI_THREAD_MAX;i++) {
        struct proc_thread *t=proc_thread_slot(i); if(t && t->process==child) { g_current=t->task; break; }
    }
    struct trap_frame tf={.vector=14,.cs=0x1b}; proc_trap_dispatch(&tf);
    CHECK(l->page_faults==1);
    quiet_boot=true;
    proc_launch_volume_read(600,113); proc_launch_disk_read(2048); proc_trap_dispatch(&tf);
    CHECK(l->volume_bytes==16384 && l->disk_bytes==16384 && l->page_faults==1);
    quiet_boot=false;
    proc_launch_volume_read(600,113); proc_launch_disk_read(2048);
    CHECK(l->volume_bytes==16497 && l->volume_reads==5 && l->disk_bytes==18432);
    g_current=saved; g_ticks=1100;
    CHECK(!proc_launch_mark(desktop,CIUKI_LAUNCH_DAMAGE,(uint32_t)pid,0));
    CHECK(!l->finished && launch_lines==1); /* DAMAGE alone is not visible */
    g_ticks=1102; CHECK(!proc_launch_mark(desktop,CIUKI_LAUNCH_FRAME,(uint32_t)pid,0));
    CHECK(l->finished && l->frame_us==1102000 && launch_lines==6);
    CHECK(strstr(launch_log,"[launch] name=fixture") && strstr(launch_log,"total_us=102000") &&
          strstr(launch_log,"read_bytes=16497") && strstr(launch_log,"page_faults=1") &&
          strstr(launch_log,"dma_bytes=16384 dma_requests=2 dma_max=8192 dma_bounces=1 dma_ms=5"));
    CHECK(!proc_launch_mark(desktop,CIUKI_LAUNCH_FRAME,(uint32_t)pid,0) && launch_lines==6);
    /* Register marshalling works beyond the low word's ~71-minute wrap. */
    g_ticks=5000001;
    struct trap_frame marker={.eax=CIUKI_SYS_LAUNCH_MARK,.ebx=CIUKI_LAUNCH_BEGIN,
        .edx=(uint32_t)5000000000ull,.esi=(uint32_t)(5000000000ull>>32)};
    CHECK(!proc_syscall(&marker) && desktop->launch_click_us==5000000000ull);
    marker.edi=1; CHECK(proc_syscall(&marker)==-EINVAL); marker.edi=0;
    marker.ebp=1; CHECK(proc_syscall(&marker)==-EINVAL);
    /* Invalid spawn consumes BEGIN; a following launch starts at spawn. */
    CHECK(!proc_launch_mark(desktop,CIUKI_LAUNCH_BEGIN,0,1100000));
    CHECK(proc_spawn(CIUKI_IMAGE_BASE-1)==-EFAULT && !desktop->launch_pending);
    unsigned lines=launch_lines; quiet_boot=true;
    CHECK(!proc_launch_mark(desktop,CIUKI_LAUNCH_BEGIN,0,1100000));
    int quiet_pid=proc_spawn(prepare_spawn_args(desktop)); CHECK(quiet_pid>0);
    CHECK(!proc_find((uint32_t)quiet_pid)->launch.active && launch_lines==lines);
    CHECK(!proc_find((uint32_t)quiet_pid)->launch.dma_reads);
    /* Two threads of one process may block in simultaneous spawns: each
     * preload must keep its own provisional-child attribution. */
    quiet_boot=false;
    struct proc_thread *second;
    CHECK(!proc_thread_prepare(desktop,CIUKI_IMAGE_BASE,0,CIUKI_IMAGE_BASE,
                               CIUKI_THREAD_STACK_MIN,0,0,false,&second));
    struct process *a,*b; CHECK(!proc_prepare(desktop,&a) && !proc_prepare(desktop,&b));
    g_current=dt->task; proc_launch_start(desktop,a,proc_launch_now(),proc_launch_now());
    g_current=second->task; proc_launch_start(desktop,b,proc_launch_now(),proc_launch_now());
    proc_launch_volume_read(4096,4096); proc_launch_disk_read(512);
    proc_launch_dma_read(512, 1, false);
    g_current=dt->task; proc_launch_volume_read(2048,113); proc_launch_disk_read(2048);
    proc_launch_dma_read(2048, 4, true);
    CHECK(a->launch.volume_bytes==113 && a->launch.disk_bytes==2048);
    CHECK(b->launch.volume_bytes==4096 && b->launch.disk_bytes==512);
    CHECK(a->launch.dma_bytes==2048 && a->launch.dma_ms==4 && a->launch.dma_bounces==1);
    CHECK(b->launch.dma_bytes==512 && b->launch.dma_ms==1 && !b->launch.dma_bounces);
    dt->launch_loading=second->launch_loading=0; proc_discard(a); proc_discard(b);
    quiet_boot=false; puts(launch_log); quiet_boot=true;
    stop_collect(proc_find((uint32_t)quiet_pid)); CHECK(!proc_reap(desktop,proc_find((uint32_t)quiet_pid)));
    stop_collect(child); CHECK(!proc_reap(desktop,child));
    stop_collect(other); stop_collect(desktop);
    proc_set_file_ops(0); fake_desktop_pid=0;
}
int main(void)
{
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram); controller.state=T_RUNNING;
    proc_init(); measurement();
    CHECK(!pages_used && heap_blocks==1);
    puts("PASS launch: quiet/verbose, authority, stages, duplicate/failure markers, bounded ELF batches, BSS, concurrent preload I/O and #PF");
    free(ram); return 0;
}
