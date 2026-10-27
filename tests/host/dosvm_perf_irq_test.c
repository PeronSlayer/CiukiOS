/* Count executed production work with IF clear, rather than host wall time.
 * Only CPU transport, scheduling and record sinks are fake.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
/* Host libc can expose wait macros through stdlib.h in POSIX mode. The
 * production DOSVM header now reaches the guest ABI via display_driver.h;
 * select its status encoding just as the other DOSVM fixtures do. */
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/timing.h>
#include <ciuki/probe.h>
static uint32_t irq_save(void);
static void irq_restore(uint32_t f);
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
#include <ciuki/cpustat.h>
#include "../../src/kernel/vm/dosvm_internal.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
#define NO_COVERAGE __attribute__((no_sanitize("coverage")))
static uint32_t cpu_flags = 0x200;
static bool measuring;
static unsigned blocks, access_bytes, copy_bytes, clear_bytes, divisions, labels, rows;
static void (*restore_hook)(void);
static uint64_t fake_tsc;
static char periodic[4096];
static unsigned periodic_bytes, periodic_rows;

#ifndef F186_BUDGET
void klog_periodic(const char *fmt,...)
{
    CHECK(cpu_flags & 0x200);
    va_list ap; va_start(ap,fmt);
    int n=vsnprintf(periodic+periodic_bytes,sizeof(periodic)-periodic_bytes,fmt,ap); va_end(ap);
    CHECK(n>0 && n<=254 && periodic_bytes+(unsigned)n+2<sizeof(periodic));
    periodic_bytes+=(unsigned)n; periodic[periodic_bytes++]='\n'; periodic[periodic_bytes]=0;
    periodic_rows++;
}
#endif
uint64_t g_tsc_per_ms;
volatile uint64_t g_ticks;
volatile bool g_need_resched;
struct ciuki_boot_info g_boot;

/* Clang supplies basic-block, scalar load/store and division callbacks.
 * Bulk memory wrappers below also count work hidden in libc intrinsics. */
NO_COVERAGE void __sanitizer_cov_trace_pc_guard_init(uint32_t *first, uint32_t *last)
{ for (; first != last; first++) *first = 1; }
NO_COVERAGE void __sanitizer_cov_trace_pc_guard(uint32_t *guard)
{ (void)guard; if (measuring && !(cpu_flags & 0x200)) blocks++; }
#define ACCESS(n) \
    NO_COVERAGE void __sanitizer_cov_load##n(void *p) \
    { (void)p; if (measuring && !(cpu_flags & 0x200)) access_bytes += n; } \
    NO_COVERAGE void __sanitizer_cov_store##n(void *p) \
    { (void)p; if (measuring && !(cpu_flags & 0x200)) access_bytes += n; }
ACCESS(1) ACCESS(2) ACCESS(4) ACCESS(8) ACCESS(16)
#undef ACCESS
NO_COVERAGE void __sanitizer_cov_trace_div4(uint32_t divisor)
{ (void)divisor; if (measuring && !(cpu_flags & 0x200)) divisions++; }
NO_COVERAGE void __sanitizer_cov_trace_div8(uint64_t divisor)
{ (void)divisor; if (measuring && !(cpu_flags & 0x200)) divisions++; }
NO_COVERAGE static void *work_memcpy(void *to, const void *from, size_t n)
{ if (measuring && !(cpu_flags & 0x200)) copy_bytes += n; return memcpy(to,from,n); }
NO_COVERAGE static void *work_memset(void *to, int value, size_t n)
{ if (measuring && !(cpu_flags & 0x200)) clear_bytes += n; return memset(to,value,n); }

static uint32_t irq_save(void) { uint32_t f=cpu_flags; cpu_flags=0; return f; }
static void irq_restore(uint32_t f)
{
    cpu_flags=f;
    if ((f & 0x200) && restore_hook) {
        void (*hook)(void) = restore_hook; restore_hook=0; hook();
    }
}
void crit_label(const char *name)
{ CHECK(!(cpu_flags & 0x200) && (!strcmp(name,"dosvm_perf_snapshot") || !strcmp(name,"dosvm_serial_snapshot") || !strcmp(name,"record_publish"))); labels++; }
static uint64_t read_tsc_serialized(void) { return fake_tsc; }
static bool dosvm_in_irq(const struct dosvm *v) { (void)v; return false; }
#ifndef F186_BUDGET
void rec_diag_disk_bootlog(const char *probe, const char *fmt, ...)
{ (void)probe; (void)fmt; CHECK(cpu_flags & 0x200); rows++; }
/* Other public entry points in this production include are not exercised. */
int dosvm_read(struct dosvm *v, uint32_t at, void *out, uint32_t n)
{ (void)v; (void)at; (void)out; (void)n; abort(); }
void rec_diag(const char *probe, const char *fmt, ...)
{ (void)probe; (void)fmt; abort(); }
bool rec_active(void) { return false; }
void rec_emit(const char *probe,const char *event,const char *fmt,...)
{ (void)probe;(void)event;(void)fmt;abort(); }
#endif
#ifndef F186_BUDGET
int kvsnprintf(char *out,size_t size,const char *fmt,va_list args)
{ return vsnprintf(out,size,fmt,args); }
#endif
void schedule(void) { abort(); }

#define memcpy work_memcpy
#define memset work_memset
#include "../../src/kernel/vm/dosvm_perf.inc"
#undef memcpy
#undef memset

static struct dosvm vm;
static void reset(void)
{
    memset(&vm,0,sizeof(vm)); perf_current=&vm;
    cpu_flags=0x200; g_tsc_per_ms=1000; g_ticks=DOSVM_PERF_INTERVAL; fake_tsc=100;
    vm.perf.serial.tick=g_ticks;
    blocks=access_bytes=copy_bytes=clear_bytes=divisions=labels=rows=0;
    periodic_bytes=periodic_rows=0; periodic[0]=0;
}
static void seed(bool dense, unsigned phase)
{
    reset(); vm.perf.phase=phase; vm.perf.stamp=fake_tsc;
    vm.perf.guest_cycles=1000; vm.perf.monitor_cycles=2000;
    vm.video_frames=17; vm.perf.timer_cycles=30; vm.perf.audio_cycles=20;
    if (!dense) return;
    struct dosvm_perf_interval *b=&vm.perf.interval[0];
    for (unsigned i=0;i<DOSVM_PERF_PORTS;i++)
        b->ports[i]=(struct dosvm_hot_cost){ .id=i, .count=1, .cycles=i+1 };
    for (unsigned p=0;p<3;p++) for (unsigned i=0;i<256;i++)
        b->vectors[p][i]=(struct dosvm_hot_cost){ .id=p*256+i, .count=1, .cycles=p*256+i+1 };
    for (unsigned i=0;i<DOSVM_COST_COUNT;i++) {
        vm.perf.cost_count[i]=1; vm.perf.cost_cycles[i]=i+1;
    }
}
static void next_interval_producer(void)
{
    CHECK(cpu_flags & 0x200);
    CHECK(vm.perf.active_interval==1 && vm.perf.reporting);
    /* Run real producers immediately on IF restore, before ranking/clearing.
     * Raw scalar updates and hot entries belong solely to the next interval. */
    vm.video_frames+=3; vm.perf.timer_cycles+=7; vm.perf.audio_cycles+=5;
    vm.perf.interval[1].disk.reads=7;
    vm.perf.interval[1].disk.bytes=8192;
    vm.perf.interval[1].disk.hits=11;
    vm.perf.interval[1].disk.misses=3;
    vm.perf.interval[1].disk.regions=1;
    vm.perf.interval[1].disk.touched[8]=1;
    vm.perf.interval[1].disk.latency[4]=2;
    vm.perf.io_valid=true; vm.perf.io_port=0x1234; vm.perf.cause=DOSVM_COST_IO;
    vm.perf.trap_active=true; vm.perf.trap_cycles=19;
    perf_exit_end(&vm);
    perf_vector_end(&vm,2,0x80,0,perf_monitor_stamp(&vm));
    uint64_t tick=g_ticks;
    g_ticks+=DOSVM_PERF_INTERVAL;
    dosvm_perf_report(&vm); /* due but already reporting: no bank reuse */
    g_ticks=tick;
    CHECK(vm.perf.active_interval==1 && !vm.perf.sample_count);
}
static void next_serial_producer(void) {
    CHECK((cpu_flags&0x200) && vm.perf.serial.active==1 && vm.perf.serial.reporting);
    vm.perf.io_valid=true; vm.perf.io_port=0x1234; vm.perf.cause=DOSVM_COST_IO;
    vm.perf.trap_active=true; vm.perf.trap_cycles=19;
    perf_exit_end(&vm);
    dosvm_perf_report(&vm); /* no reentrant bank reuse */
    CHECK(vm.perf.serial.active==1 && !vm.perf.sample_count);
}
#ifndef F186_BUDGET
int main(void)
{
    reset(); vm.perf.serial.tick=0; vm.info.key.id=35; vm.info.key.generation=7;
    for (unsigned i=0;i<6;i++) vm.perf.exits[i]=11+i;
    for (unsigned i=0;i<DOSVM_IDLE_REASONS;i++) vm.idle.count[i]=1+i;
    vm.idle.blocks=10; vm.idle.reasons=15; vm.idle.busy_permille=99;
    vm.perf.cost_count[DOSVM_COST_IO]=4; vm.perf.cost_cycles[DOSVM_COST_IO]=500;
    for (unsigned i=0;i<5;i++) vm.perf.serial.ports[0][i]=vm.perf.interval[0].ports[i]=
        (struct dosvm_hot_cost){.id=0x330+i,.count=2+i,.cycles=100*(i+1)};
    g_ticks=1999; dosvm_perf_report(&vm); CHECK(!periodic_rows && !rows);
    g_ticks=2000; dosvm_perf_report(&vm);
    CHECK(periodic_rows==5 && !vm.perf.sample_count && !rows);
    CHECK(strstr(periodic,"[dosvm] perf vm=35 gen=7 exits=65 gp=11 pf=12 db=13 timer=14 other=15 pm=16 busy_pm=0 idle_2f=1 idle_28=2 idle_16=3 idle_hlt=4\n"));
    CHECK(strstr(periodic,"[dosvm] perf_port vm=35 port=0334 exits=6 us=500\n"));
    CHECK(strstr(periodic,"[dosvm] perf_port vm=35 port=0333 exits=5 us=400\n"));
    CHECK(strstr(periodic,"[dosvm] perf_port vm=35 port=0332 exits=4 us=300\n"));
    CHECK(!strstr(periodic,"port=0331") && strstr(periodic,"[dosvm] perf_cost vm=35 cause=io exits=4 us=500\n"));
    CHECK(vm.idle.blocks==10 && vm.idle.reasons==15 && vm.idle.busy_permille==99);
    for (unsigned i=0;i<DOSVM_IDLE_REASONS;i++)
        CHECK(vm.idle.count[i]==1+i && !vm.perf.last_idle_count[i]);
    CHECK(!vm.perf.last_idle_blocks);
    periodic_bytes=periodic_rows=0; periodic[0]=0;
    vm.perf.exits[0]+=7; vm.perf.exits[5]+=3; vm.idle.count[DOSVM_IDLE_HLT]+=2;
    vm.idle.blocks+=2;
    g_ticks=4000; dosvm_perf_report(&vm);
    CHECK(periodic_rows==2 && !vm.perf.sample_count && !rows);
    CHECK(strstr(periodic,"[dosvm] perf vm=35 gen=7 exits=7 gp=7 pf=0 db=0 timer=0 other=0 pm=3 busy_pm=0 idle_2f=0 idle_28=0 idle_16=0 idle_hlt=2\n"));
    CHECK(!strstr(periodic,"perf_port") && strstr(periodic,"exits=0 us=0\n"));
    CHECK(vm.idle.blocks==12 && vm.idle.reasons==15 && vm.idle.busy_permille==99);
    for (unsigned i=0;i<DOSVM_IDLE_REASONS;i++)
        CHECK(vm.idle.count[i]==1+i+(i==DOSVM_IDLE_HLT ? 2u : 0u) && !vm.perf.last_idle_count[i]);
    CHECK(!vm.perf.last_idle_blocks);
    vm.perf.dac_counted=767;
    g_ticks=5000; dosvm_perf_report(&vm);
    CHECK(vm.perf.sample_count==1 && rows && periodic_rows==2);
    CHECK(vm.perf.sample[0].ms==5000 && vm.perf.sample[0].exits[0]==18);
    CHECK(vm.perf.sample[0].dac_counted==767);
    CHECK(vm.perf.sample[0].top_port[0].id==0x334);
    CHECK(vm.perf.sample[0].idle_blocks==12 && vm.perf.sample[0].idle_reasons==15);
    for (unsigned i=0;i<DOSVM_IDLE_REASONS;i++)
        CHECK(vm.perf.sample[0].idle_count[i]==vm.idle.count[i]);
    periodic_bytes=periodic_rows=0; periodic[0]=0;
    g_ticks=6000; dosvm_perf_report(&vm);
    CHECK(periodic_rows==2 && vm.perf.sample_count==1);
    CHECK(strstr(periodic,"busy_pm=0 idle_2f=0 idle_28=0 idle_16=0 idle_hlt=0\n"));
    g_ticks=10000; dosvm_perf_report(&vm);
    CHECK(vm.perf.sample_count==2 && !vm.perf.sample[1].idle_blocks && !vm.perf.sample[1].idle_reasons);
    CHECK(!vm.perf.sample[1].dac_counted);
    CHECK(vm.idle.blocks==12 && vm.idle.reasons==15 && !vm.idle.busy_permille);
    for (unsigned i=0;i<DOSVM_IDLE_REASONS;i++) {
        CHECK(!vm.perf.sample[1].idle_count[i]);
        CHECK(vm.idle.count[i]==1+i+(i==DOSVM_IDLE_HLT ? 2u : 0u));
    }
    puts("PASS f1-86 idle accounting: cumulative API counters preserved across independent UART/disk windows, empty deltas");
    reset(); vm.info.key.id=vm.info.key.generation=UINT32_MAX;
    struct dosvm_perf_sample maximum={.busy_permille=1000};
    for (unsigned i=0;i<6;i++) maximum.exits[i]=UINT32_MAX;
    for (unsigned i=0;i<DOSVM_IDLE_REASONS;i++) maximum.idle_count[i]=UINT32_MAX;
    perf_periodic(&vm,&maximum); CHECK(periodic_rows==2 && strstr(periodic,"exits=21474836475"));
    puts("f1-86 VM periodic: no probe, 2s cadence, exact deltas, top three ports/cause, empty next window PASS");
    for (unsigned phase=0;phase<=2;phase++) {
        unsigned sparse_blocks=0,sparse_access=0,sparse_copy=0;
        for (unsigned dense=0;dense<=1;dense++) {
            seed(dense,phase); vm.perf.report_tick=g_ticks;
            vm.perf.serial.tick=g_ticks-DOSVM_SERIAL_INTERVAL;
            memcpy(vm.perf.serial.ports[0],vm.perf.interval[0].ports,sizeof(vm.perf.serial.ports[0]));
            measuring=true; dosvm_perf_report(&vm); measuring=false;
            CHECK(labels==1 && blocks>0 && blocks<=256 && !rows && !vm.perf.sample_count);
            CHECK(!divisions && !clear_bytes && copy_bytes<=512 && access_bytes<=1024);
            if (dense) {
                CHECK(blocks==sparse_blocks && access_bytes==sparse_access && copy_bytes==sparse_copy);
                printf("f1-86 serial IRQ work phase=%u sparse=dense: blocks=%u scalar_bytes=%u copied_bytes=%u cleared_bytes=%u divisions=%u PASS\n",
                       phase,blocks,access_bytes,copy_bytes,clear_bytes,divisions);
            } else { sparse_blocks=blocks; sparse_access=access_bytes; sparse_copy=copy_bytes; }
        }
    }
    seed(true,0); vm.perf.report_tick=g_ticks; vm.perf.serial.tick=g_ticks-DOSVM_SERIAL_INTERVAL;
    memcpy(vm.perf.serial.ports[0],vm.perf.interval[0].ports,sizeof(vm.perf.serial.ports[0]));
    restore_hook=next_serial_producer; dosvm_perf_report(&vm);
    CHECK(strstr(periodic,"port=00ff") && !strstr(periodic,"port=1234"));
    CHECK(vm.perf.serial.ports[1][0x1234%DOSVM_PERF_PORTS].count==1);
    periodic_rows=periodic_bytes=0; periodic[0]=0;
    g_ticks+=DOSVM_SERIAL_INTERVAL; dosvm_perf_report(&vm);
    CHECK(!rows && !vm.perf.sample_count && periodic_rows==3);
    CHECK(strstr(periodic,"port=1234 exits=1 us=19\n") && strstr(periodic,"cause=io exits=1 us=19\n"));
    puts("PASS f1-86 serial banks: concurrent producer/reentry, next-window port and cause, disk banks untouched");
    for (unsigned phase=0;phase<=2;phase++) {
        unsigned sparse_blocks=0,sparse_access=0,sparse_copy=0;
        for (unsigned dense=0;dense<=1;dense++) {
            seed(dense,phase);
            measuring=true; dosvm_perf_report(&vm); measuring=false;
            CHECK(labels==1 && blocks>0 && blocks<=256);
            CHECK(access_bytes>0 && access_bytes<=1024);
            CHECK(copy_bytes>0 && copy_bytes<=sizeof(struct dosvm_perf_raw));
            CHECK(!clear_bytes && !divisions && cpu_flags==0x200);
            CHECK(vm.perf.sample_count==1 && vm.perf.active_interval==1);
            CHECK(vm.perf.sample[0].guest_us==1000 && vm.perf.sample[0].monitor_us==2000);
            CHECK(vm.perf.sample[0].frames==17 && vm.perf.sample[0].timer_us==30);
            CHECK(vm.perf.sample[0].top_port[0].count==dense);
            CHECK(vm.perf.sample[0].top_vector[0].count==dense);
            if (dense) {
                CHECK(rows==45 && vm.perf.sample[0].top_port[0].id==255 && /* baseline rows, IF row, two F3-47 disk rows, t23-dosperf presenter row */
                      vm.perf.sample[0].top_vector[0].id==767);
                CHECK(blocks==sparse_blocks && access_bytes==sparse_access && copy_bytes==sparse_copy);
                printf("f3-31 IRQ work phase=%u sparse=dense: blocks=%u scalar_bytes=%u copied_bytes=%u cleared_bytes=%u divisions=%u PASS\n",
                       phase,blocks,access_bytes,copy_bytes,clear_bytes,divisions);
            } else { sparse_blocks=blocks; sparse_access=access_bytes; sparse_copy=copy_bytes; }
        }
    }
    seed(true,0); restore_hook=next_interval_producer; dosvm_perf_report(&vm);
    CHECK(vm.perf.sample[0].frames==17 && vm.perf.sample[0].timer_us==30 && vm.perf.sample[0].audio_us==20);
    CHECK(vm.perf.interval[1].ports[0x1234 % DOSVM_PERF_PORTS].count==1);
    CHECK(vm.perf.interval[1].vectors[2][0x80].count==1);
    CHECK(vm.perf.interval[1].disk.reads==7 && vm.perf.interval[1].disk.bytes==8192);
    CHECK(vm.perf.interval[1].disk.hits==11 && vm.perf.interval[1].disk.misses==3);
    CHECK(vm.perf.interval[1].disk.regions==1 && vm.perf.interval[1].disk.touched[8]==1);
    CHECK(vm.perf.interval[1].disk.latency[4]==2 && !vm.perf.interval[0].disk.latency[4]);
    for (unsigned i=0;i<DOSVM_PERF_PORTS;i++) CHECK(!vm.perf.interval[0].ports[i].count);
    for (unsigned p=0;p<3;p++) for (unsigned i=0;i<256;i++) CHECK(!vm.perf.interval[0].vectors[p][i].count);
    g_ticks+=DOSVM_PERF_INTERVAL; dosvm_perf_report(&vm);
    CHECK(vm.perf.sample_count==2 && vm.perf.active_interval==0);
    CHECK(!vm.perf.interval[1].disk.reads && !vm.perf.interval[1].disk.bytes);
    CHECK(!vm.perf.interval[1].disk.regions && !vm.perf.interval[1].disk.touched[8]);
    CHECK(vm.perf.sample[1].frames==3 && vm.perf.sample[1].timer_us==7 && vm.perf.sample[1].audio_us==5);
    CHECK(vm.perf.sample[1].top_port[0].id==0x1234 && vm.perf.sample[1].top_port[0].cycles==19);
    CHECK(vm.perf.sample[1].top_vector[0].id==0x280 && vm.perf.sample[1].top_vector[0].count==1);
    g_ticks+=DOSVM_PERF_INTERVAL; dosvm_perf_report(&vm);
    CHECK(vm.perf.sample_count==3 && !vm.perf.sample[2].top_port[0].count && !vm.perf.sample[2].top_vector[0].count);
    cpu_flags=0; g_ticks+=DOSVM_PERF_INTERVAL; dosvm_perf_report(&vm);
    CHECK(!cpu_flags && vm.perf.sample_count==3);
    puts("f3-31 IRQ snapshot: detached banks, concurrent producers, reentrant reporter guard, reuse and IF-off deferral PASS");
    return 0;
}

#endif /* !F186_BUDGET */
