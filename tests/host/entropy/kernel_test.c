/* Production boot/guard/IRQ adapters and syscall with hardware/user shims.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/process.h>
#include <ciuki/entropy_pool.h>
#include <ciuki/entropy_jent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL entropy kernel %u: %s\n",__LINE__,#x); abort(); } } while(0)
#define CIUKI_CPU_H
static uint8_t physical[1024*1024];
static bool pages_used[256];
#define P2V(p) ((void *)(physical+(p)))
#define V2P(p) ((uint32_t)((uint8_t *)(p)-physical))
static unsigned owned_pages;
static bool refuse_pages;
uint32_t pmm_alloc_contig(uint32_t pages)
{
    if(refuse_pages)return 0;
    for(unsigned at=1;at+pages<=256;at++){unsigned i;for(i=0;i<pages && !pages_used[at+i];i++);
        if(i==pages){for(i=0;i<pages;i++)pages_used[at+i]=true;return at*4096;}
    }return 0;
}
void pmm_free(uint32_t p) { CHECK(pages_used[p/4096] && owned_pages);pages_used[p/4096]=false;owned_pages--; }
void ledger_move(uint32_t p,uint32_t pages,enum mm_owner from,enum mm_owner to)
{ CHECK(p && from==MM_KERNEL && to==MM_HEAP);owned_pages+=pages; }
static uint32_t fake_flags=0x200;
static uint64_t fake_clock;
static bool stalled, pit_stalled;
static unsigned pit_reads;
static uint8_t speaker=2;
static uint32_t irq_save(void) { uint32_t f=fake_flags;fake_flags=0;return f; }
static void irq_restore(uint32_t f) { fake_flags=f; }
static uint64_t read_tsc_serialized(void) { if(!stalled)fake_clock+=10000;return fake_clock; }
static uint64_t read_tsc_unserialized(void) { return ++fake_clock; }
static uint8_t inb(uint16_t port) { CHECK(port==0x61);return pit_stalled || pit_reads++<2 ? speaker&~0x20u : speaker|0x20u; }
static void outb(uint16_t port,uint8_t value) { if(port==0x61)speaker=value;else CHECK(port==0x42 || port==0x43); }
uint64_t ktime_cycles(void) { return read_tsc_serialized(); }
void *kmalloc(size_t n) { CHECK(n<=2040);return malloc(n); }
void kfree(void *p) { free(p); }
volatile uint64_t g_ticks;
bool g_cpu_tsc=true;
struct task *g_current;
static struct process process;
static struct task worker;
static bool refuse_worker, readonly, write_failure;
static unsigned started, locked, logs, safe_points, boot_logs, raw_logs;
static void (*worker_entry)(void *);
static jmp_buf worker_return;
void kmutex_init(struct kmutex *m) { memset(m,0,sizeof(*m)); }
void kmutex_lock(struct kmutex *m) { (void)m;CHECK(!locked++); }
void kmutex_unlock(struct kmutex *m) { (void)m;CHECK(locked--==1); }
void klog(const char *fmt,...) { CHECK(strncmp(fmt,"[entropy]",9)==0);logs++;
    if(strstr(fmt,"boot rounds="))boot_logs++;
    if(strstr(fmt,"raw index="))raw_logs++;
}
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
static const struct entropy_jent_ops *jent_ops;
static bool init_failure, read_failure;
static unsigned reads, closes;
void entropy_jent_begin(const struct entropy_jent_ops *o) { jent_ops=o; }
int entropy_jent_init(void) { return init_failure ? -1 : 0; }
int entropy_jent_read(uint8_t bytes[32])
{
    CHECK(!locked);reads++;jent_ops->yield();
    if(read_failure)return -1;
    for(unsigned i=0;i<1100;i++)jent_ops->raw(i,1,3);
    memset(bytes,0x42,32);return 0;
}
void entropy_jent_close(void) { closes++; }
unsigned entropy_jent_rounds(void) { return reads*963; }
bool entropy_jent_expired(void) { return false; }
struct ciuki_boot_info g_boot;
#include "../../../src/kernel/lib/entropy_alloc.c"
#include "../../../src/kernel/core/entropy.c"
#include "../../../src/kernel/lib/stackprot.c"
#include "../../../src/kernel/proc/syscalls_entropy.c"
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    void *allocation=entropy_alloc(262144);CHECK(allocation && owned_pages==65);entropy_free(allocation);CHECK(!owned_pages);
    allocation=entropy_alloc(500);CHECK(allocation && !owned_pages);entropy_free(allocation);
    refuse_pages=true;CHECK(!entropy_alloc(8192));refuse_pages=false;
    CHECK(!entropy_alloc(262145));
    struct trap_frame tf={.ecx=1,.ebx=4096};
    memset(user_buffer,0xa5,sizeof(user_buffer));
    CHECK(entropy_syscall(&tf)==-EAGAIN && user_buffer[0]==0xa5);
    tf.ecx=257;CHECK(entropy_syscall(&tf)==-EINVAL);tf.ecx=0;tf.ebx=0;CHECK(!entropy_syscall(&tf));
    tf.ecx=1;CHECK(entropy_syscall(&tf)==-EFAULT);
    tf.ebx=UINT32_MAX;CHECK(entropy_syscall(&tf)==-EFAULT);
    tf.ebx=4096;readonly=true;CHECK(entropy_syscall(&tf)==-EFAULT);readonly=false;
    stackprot_init();CHECK(booted && seeded && pool.ready && !pool.failed && __stack_chk_guard && logs==2);
    uintptr_t guard=__stack_chk_guard;stackprot_init();CHECK(__stack_chk_guard==guard && logs==2);
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
    CHECK(!fast.count && (pool.sources&ENTROPY_SOURCE_IRQ) && __stack_chk_guard==guard && safe_points==8);
    CHECK(pool.estimate_bits==256);unsigned collected=reads;
    if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(reads==collected); /* saturation does not sample jitter */
    g_ticks=ENTROPY_RESEED_MS;
    CHECK(!entropy_syscall(&tf) && pool.reseeds==2 && !pool.estimate_bits);
    if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(reads==collected+8 && pool.estimate_bits==256);
    g_ticks+=ENTROPY_RESEED_MS;CHECK(!entropy_syscall(&tf));
    g_ticks+=ENTROPY_RESEED_MS;
    memset(user_buffer,0xa5,sizeof(user_buffer));CHECK(entropy_syscall(&tf)==-EAGAIN && user_buffer[0]==0xa5);
    read_failure=true;unsigned freed=closes;worker_entry(NULL);
    CHECK(pool.failed && closes==freed+1 && entropy_syscall(&tf)==-EAGAIN);
    CHECK(speaker==2 && boot_logs==1 && !raw_logs);
    /* Exact boot token, capture saturation/drops and bounded flush. */
    g_current=NULL;booted=seeded=false;read_failure=false;g_ticks=0;pit_reads=0;
    memcpy(g_boot.options,"xentropyraw=1 entropyraw=10",27);CHECK(!raw_enabled());
    memset(g_boot.options,0,sizeof(g_boot.options));memcpy(g_boot.options,"entropyraw=1",12);
    CHECK(raw_enabled());stackprot_init();
    CHECK(pool.ready && raw_logs==RAW_LIMIT && raw_dropped==8800-RAW_LIMIT && !raw && !owned_pages);
    memset(g_boot.options,0,sizeof(g_boot.options));
    booted=seeded=false;g_cpu_tsc=false;boot_per_ms=0;stackprot_init();
    CHECK(pool.failed && !pool.ready && __stack_chk_guard);
    booted=seeded=false;g_cpu_tsc=true;stalled=true;pit_reads=0;stackprot_init();
    CHECK(pool.failed && !pool.ready);stalled=false;
    booted=seeded=false;pit_stalled=true;pit_reads=0;stackprot_init();
    CHECK(pool.failed && !pool.ready && speaker==2);pit_stalled=false;
    booted=seeded=false;init_failure=true;pit_reads=0;stackprot_init();
    CHECK(pool.failed && !pool.ready && !pool.estimate_bits);
    puts("PASS production boot/guard, absent/stalled TSC/PIT, library-init refusal, IRQ-only mix, worker saturation/resume/retirement, bounded opt-in raw capture and syscall 91 surface");
    return 0;
}
