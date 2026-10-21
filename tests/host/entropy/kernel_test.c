/* Production boot/guard/IRQ adapters and syscall with hardware/user shims.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/process.h>
#include <ciuki/entropy_pool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL entropy kernel %u: %s\n",__LINE__,#x); abort(); } } while(0)
#define CIUKI_CPU_H
static uint32_t fake_flags=0x200, sample=0x65432101;
static uint64_t fake_clock;
static unsigned clock_mode, linear_step;
static uint32_t irq_save(void) { uint32_t f=fake_flags;fake_flags=0;return f; }
static void irq_restore(uint32_t f) { fake_flags=f; }
static uint64_t read_tsc_serialized(void)
{
    if(clock_mode==1)return fake_clock;
    if(clock_mode==2){fake_clock+=++linear_step;return fake_clock;}
    sample^=sample<<13;sample^=sample>>17;sample^=sample<<5;fake_clock+=sample;return fake_clock;
}
static uint64_t read_tsc_unserialized(void) { return ++fake_clock; }
uint64_t ktime_cycles(void) { return read_tsc_serialized(); }
volatile uint64_t g_ticks;
bool g_cpu_tsc=true;
struct task *g_current;
static struct process process;
static struct task worker;
static bool refuse_worker, readonly, write_failure;
static unsigned started, locked, logs, safe_points;
static void (*worker_entry)(void *);
static jmp_buf worker_return;
void kmutex_init(struct kmutex *m) { memset(m,0,sizeof(*m)); }
void kmutex_lock(struct kmutex *m) { (void)m;CHECK(!locked++); }
void kmutex_unlock(struct kmutex *m) { (void)m;CHECK(locked--==1); }
void klog(const char *fmt,...) { CHECK(strncmp(fmt,"[entropy]",9)==0);logs++; }
void panic(const char *fmt,...) { (void)fmt;abort(); }
struct task *task_create_kernel(const char *n,void (*fn)(void *),void *arg,enum task_prio prio)
{ CHECK(!strcmp(n,"entropy") && !arg && prio==P_NORMAL);worker_entry=fn;return refuse_worker?NULL:&worker; }
void task_start(struct task *t) { CHECK(t==&worker);started++; }
void task_sleep_ms(uint32_t n) { CHECK(n==10 && !locked && fake_flags==0x200);longjmp(worker_return,1); }
void kpreempt_point(void) { CHECK(!locked && fake_flags==0x200);safe_points++; }
struct process *proc_current(void) { return &process; }
static uint8_t user_buffer[512];
bool ua_range(const struct uaddr *u,uint32_t address,uint32_t n,uint32_t prot)
{ (void)u;CHECK(prot==PROT_WRITE);return !readonly && address>=4096 && address-4096<=sizeof(user_buffer) && n<=sizeof(user_buffer)-(address-4096); }
int ua_user_write(const struct uaddr *u,uint32_t address,const void *data,uint32_t n)
{ if(write_failure)return -ENOMEM;if(!ua_range(u,address,n,PROT_WRITE))return -EFAULT;memcpy(user_buffer+(address-4096),data,n);return 0; }
#include "../../../src/kernel/core/entropy.c"
#include "../../../src/kernel/lib/stackprot.c"
#include "../../../src/kernel/proc/syscalls_entropy.c"
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    struct trap_frame tf={.ecx=1,.ebx=4096};
    memset(user_buffer,0xa5,sizeof(user_buffer));
    CHECK(entropy_syscall(&tf)==-EAGAIN && user_buffer[0]==0xa5);
    tf.ecx=257;CHECK(entropy_syscall(&tf)==-EINVAL);tf.ecx=0;tf.ebx=0;CHECK(!entropy_syscall(&tf));
    tf.ecx=1;CHECK(entropy_syscall(&tf)==-EFAULT);
    tf.ebx=UINT32_MAX;CHECK(entropy_syscall(&tf)==-EFAULT);
    tf.ebx=4096;readonly=true;CHECK(entropy_syscall(&tf)==-EFAULT);readonly=false;
    stackprot_init();CHECK(booted && seeded && pool.ready && !pool.failed && __stack_chk_guard && logs==1);
    uintptr_t guard=__stack_chk_guard;stackprot_init();CHECK(__stack_chk_guard==guard && logs==1);
    tf.ecx=256;CHECK(!entropy_syscall(&tf) && user_buffer[256]==0xa5);
    write_failure=true;CHECK(entropy_syscall(&tf)==-EFAULT);write_failure=false;
    tf.edx=1;CHECK(entropy_syscall(&tf)==-EINVAL);tf.edx=0;
    tf.esi=1;CHECK(entropy_syscall(&tf)==-EINVAL);tf.esi=0;
    tf.edi=1;CHECK(entropy_syscall(&tf)==-EINVAL);tf.edi=0;
    tf.ebp=1;CHECK(entropy_syscall(&tf)==-EINVAL);tf.ebp=0;
    struct entropy_pool before=pool;
    for(unsigned i=0;i<16;i++)entropy_irq(i);entropy_irq(16);
    CHECK(fast.count==16 && fast.sources==65535 && !memcmp(&pool,&before,sizeof(pool)));
    refuse_worker=true;entropy_start();CHECK(!worker_started && !started);
    refuse_worker=false;entropy_start();entropy_start();CHECK(worker_started && started==1);
    g_current=&worker;
    if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(!fast.count && (pool.sources&ENTROPY_SOURCE_IRQ) && __stack_chk_guard==guard && safe_points==64);
    g_ticks=ENTROPY_RESEED_MS;pool.estimate_bits=0;pool.fraction=0;
    memset(user_buffer,0xa5,sizeof(user_buffer));CHECK(entropy_syscall(&tf)==-EAGAIN && user_buffer[0]==0xa5);
    entropy_pool_fail(&pool);CHECK(entropy_syscall(&tf)==-EAGAIN);
    g_current=NULL;booted=seeded=false;g_cpu_tsc=false;stackprot_init();
    CHECK(pool.failed && !pool.ready && __stack_chk_guard);
    booted=seeded=false;g_cpu_tsc=true;clock_mode=1;stackprot_init();
    CHECK(pool.failed && pool.health.failed && !pool.ready);
    booted=seeded=false;clock_mode=2;stackprot_init();
    CHECK(pool.failed && !pool.health.failed && !pool.ready && !pool.estimate_bits);
    puts("PASS production boot/guard lifecycle, missing/stalled/deterministic TSC and finite budget, IRQ-only mix, worker allocation/drain and syscall 91 bounds/permissions/readiness");
    return 0;
}
