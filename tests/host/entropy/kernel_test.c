/* Production boot/guard/IRQ adapters and syscall with hardware/user shims.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/process.h>
#include <ciuki/entropy.h>
#include <ciuki/entropy_pool.h>
#include <ciuki/entropy_jent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
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
static bool irq_counter_stopped;
static uint64_t read_tsc_unserialized(void) { return irq_counter_stopped ? fake_clock : ++fake_clock; }
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
static unsigned stream_samples, stream_complete, serial_waits;
static uint64_t serial_ticket;
static bool serial_refuse, serial_lost, serial_pending, in_jent_read, read_failure;
static void (*worker_entry)(void *);
static jmp_buf worker_return;
void kmutex_init(struct kmutex *m) { memset(m,0,sizeof(*m)); }
void kmutex_lock(struct kmutex *m) { (void)m;CHECK(!locked++); }
void kmutex_unlock(struct kmutex *m) { (void)m;CHECK(locked--==1); }
static char last_record[512], last_jitter[512];
static unsigned jitter_logs;
void klog(const char *fmt,...) { CHECK(strncmp(fmt,"[entropy]",9)==0);logs++;
    if(strstr(fmt,"boot rounds="))boot_logs++;
    char line[512];va_list ap;va_start(ap,fmt);vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);
    if(!strncmp(line,"[entropy] sources=",18))strcpy(last_record,line);
    if(!strncmp(line,"[entropy] jitter ",17)){strcpy(last_jitter,line);jitter_logs++;}
}
uint32_t serial_evidence_begin(size_t bytes)
{
    CHECK(g_current && !locked && !in_jent_read && fake_flags==0x200 && bytes<224);
    /* A user can still obtain DRBG output during serial backpressure. */
    uint8_t byte;CHECK(!entropy_get(&byte,1) || read_failure);
    return irq_save();
}
uint64_t serial_write_ticket(const char *line,size_t bytes)
{
    CHECK(!locked && !in_jent_read && !fake_flags && bytes<224 && line[bytes-1]=='\n');
    if(serial_refuse)return 0;
    unsigned index,count;int end=0;char data[129];
    if(sscanf(line,"[entropy] raw index=%u count=%u data=%128[0-9a-f]\n%n",&index,&count,data,&end)==3){
        CHECK((size_t)end==bytes && index==stream_samples && count && count<=64 && strlen(data)==2*count);
        for(unsigned i=0;i<count;i++){
            unsigned byte;CHECK(sscanf(data+2*i,"%2x",&byte)==1);
            CHECK(byte==(uint8_t)(stream_samples%1100));stream_samples++;
        }
        CHECK(stream_samples<=1048576);raw_logs++;
    }else{
        CHECK(sscanf(line,"[entropy] raw complete samples=%u bits=8\n%n",&count,&end)==1);
        CHECK((size_t)end==bytes && count==1048576 && stream_samples==count && !stream_complete);
        stream_complete++;
    }
    return ++serial_ticket;
}
void serial_snapshot(struct serial_stats *s)
{
    CHECK(!locked && !in_jent_read && fake_flags==0x200);
    *s=(struct serial_stats){.present=!serial_lost,.sent=serial_pending?serial_ticket-1:serial_ticket};
}
void panic(const char *fmt,...) { (void)fmt;abort(); }
struct task *task_create_kernel(const char *n,void (*fn)(void *),void *arg,enum task_prio prio)
{ CHECK(!strcmp(n,"entropy") && !arg && prio==P_NORMAL);worker_entry=fn;return refuse_worker?NULL:&worker; }
void task_start(struct task *t) { CHECK(t==&worker);started++; }
void task_sleep_ms(uint32_t n) {
    CHECK(n==10 && !locked && fake_flags==0x200);
    if(serial_pending){serial_pending=false;serial_waits++;return;}
    longjmp(worker_return,1);
}
void kpreempt_point(void) { CHECK(!locked && fake_flags==0x200);safe_points++; }
struct process *proc_current(void) { return &process; }
static uint8_t user_buffer[512];
bool ua_range(const struct uaddr *u,uint32_t address,uint32_t n,uint32_t prot)
{ (void)u;CHECK(prot==PROT_WRITE);return !readonly && address>=4096 && address-4096<=sizeof(user_buffer) && n<=sizeof(user_buffer)-(address-4096); }
int ua_user_write(const struct uaddr *u,uint32_t address,const void *data,uint32_t n)
{ if(write_failure)return -ENOMEM;if(!ua_range(u,address,n,PROT_WRITE))return -EFAULT;memcpy(user_buffer+(address-4096),data,n);return 0; }
static const struct entropy_jent_ops *jent_ops;
static bool init_failure, read_permanent;
static unsigned reads, closes, inits, init_first, init_last, report_osr;
void entropy_jent_begin(const struct entropy_jent_ops *o) { jent_ops=o; }
int entropy_jent_init(unsigned first,unsigned last)
{
    CHECK(!locked && first>=ENTROPY_JENT_MIN_OSR && first<=last && last<=ENTROPY_JENT_MAX_OSR);inits++;init_first=first;init_last=last;
    /* A failed escalation stops where its 5 s budget ran out (here four OSRs on). */
    report_osr=!init_failure || first==last ? first : first+4<last ? first+4 : last;
    return init_failure ? -1 : 0;
}
unsigned entropy_jent_stage(void) { return ENTROPY_JENT_OUTPUT; }
void entropy_jent_report(struct entropy_jent_report *r)
{
    memset(r,0,sizeof(*r));r->osr=report_osr;r->result=init_failure || read_failure ? -1 : 0;
    r->permanent=read_failure && read_permanent;
    if(init_failure){r->alarm=1;r->alarm_stage=ENTROPY_JENT_SHA3;r->alarm_osr=init_first;r->alarm_round=2644;
        r->rounds[0]=1125;r->rounds[1]=1428;r->rounds[2]=91;r->refused=524288;}
}
int entropy_jent_read(uint8_t bytes[32])
{
    CHECK(!locked);reads++;jent_ops->yield();
    if(read_failure)return -1;
    in_jent_read=true;
    for(unsigned i=0;i<1100;i++)jent_ops->raw(i,1,3);
    in_jent_read=false;
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
    /* f6-19: a jitter read failure retires the collector, never the pool. */
    read_failure=true;unsigned freed=closes,seen=jitter_logs;
    if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(!pool.failed && closes==freed+1 && jitter==JITTER_RETRY && jitter_attempts==1 && jitter_logs==seen+1);
    CHECK(strstr(last_jitter,"attempt=read result=-1 osr=3 ") && strstr(last_jitter," state=retry"));
    CHECK(strstr(last_record," health=ok ") && strstr(last_record," jitter=retry "));
    CHECK(entropy_syscall(&tf)==-EAGAIN && jitter_retry_ms==g_ticks+1000 && jitter_next_osr==4);
    /* Interrupt timing alone supplies the fresh 256 bits: 128 edges per
     * 10 ms drain on a moving counter, 2 bits each (Linux: count/64). The
     * runtime retry at +1 s resumes at OSR 4 and fails here (init refused). */
    read_failure=false;init_failure=true;
    unsigned jitter_reads=reads,jitter_inits=inits,reseeded=pool.reseeds,drains=0,irq_bits=pool.irq_bits;
    while(pool.reseeds==reseeded){
        for(unsigned i=0;i<128;i++)entropy_irq(i%3==0 ? 0 : i%3==1 ? 14 : 11);
        g_ticks+=10;drains++;
        if(!setjmp(worker_return))worker_entry(NULL);
        CHECK(drains<=128 && !pool.failed);
    }
    CHECK(pool.irq_bits-irq_bits>=248 && reads==jitter_reads && inits==jitter_inits+1 && init_first==4 && init_last==ENTROPY_JENT_MAX_OSR);
    CHECK(jitter==JITTER_RETRY && jitter_attempts==2 && jitter_retry_ms>=g_ticks+29000 && strstr(last_jitter,"attempt=runtime "));
    CHECK(!entropy_syscall(&tf) && strstr(last_record," health=ok reseeds=4 jitter=retry "));
    /* A stopped counter: the edges are mixed, never credited. */
    unsigned credited=pool.irq_bits,estimate=pool.estimate_bits;
    irq_counter_stopped=true;
    for(unsigned k=0;k<4;k++){
        for(unsigned i=0;i<128;i++)entropy_irq(0);
        g_ticks+=10;if(!setjmp(worker_return))worker_entry(NULL);
    }
    CHECK(pool.irq_bits==credited && pool.estimate_bits==estimate && !fast.count);irq_counter_stopped=false;
    /* Backoff 30 s, then 120 s; the fourth failed attempt retires jitter for this boot. */
    g_ticks=jitter_retry_ms;if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(jitter==JITTER_RETRY && jitter_attempts==3 && jitter_retry_ms==g_ticks+120000);
    g_ticks=jitter_retry_ms;if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(jitter==JITTER_FAILED && jitter_attempts==4 && strstr(last_record," health=ok ") && strstr(last_record," jitter=failed "));
    g_ticks+=600000;unsigned failed_inits=inits;if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(inits==failed_inits && !pool.failed);
    init_failure=false;
    CHECK(speaker==2 && boot_logs==1 && !raw_logs);
    /* Exact token; boot produces no raw allocation/output. */
    g_current=NULL;booted=seeded=false;read_failure=false;g_ticks=0;pit_reads=0;
    memcpy(g_boot.options,"xentropyraw=1 entropyraw=10",27);CHECK(!raw_enabled());
    memset(g_boot.options,0,sizeof(g_boot.options));memcpy(g_boot.options,"entropyraw=1",12);
    CHECK(raw_enabled());stackprot_init();
    CHECK(pool.ready && !raw_logs && !raw && !owned_pages);
    g_current=&worker;serial_pending=true;
    if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(raw && raw_sent==8800 && !raw_count && serial_waits==1 && owned_pages==33);
    /* Continue past full credit, preserve order/zero/rejected low bytes,
     * service reseed demand during capture, and stop at exactly 2^20. */
    for(unsigned batch=1;raw;batch++){
        if(batch==32){g_ticks=ENTROPY_RESEED_MS;uint8_t byte;CHECK(!entropy_get(&byte,1));}
        if(!setjmp(worker_return))worker_entry(NULL);
    }
    CHECK(raw_sent==RAW_LIMIT && stream_samples==RAW_LIMIT && stream_complete==1 && !owned_pages);
    CHECK(pool.ready && !pool.failed && pool.reseeds==2 && pool.estimate_bits==256);
    collected=reads;
    if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(reads==collected); /* capture stops and saturated collection pauses */
    /* The observation hook never overruns an attempt buffer, even if an
     * invalid caller supplies excess callbacks. No serial I/O in the hook. */
    raw=entropy_alloc(ENTROPY_COLLECT_ROUNDS);CHECK(raw);raw_sent=0;
    for(unsigned i=0;i<ENTROPY_COLLECT_ROUNDS+16;i++)capture(i,1,3);
    CHECK(raw_count==ENTROPY_COLLECT_ROUNDS && raw[0]==0 && raw[raw_count-1]==255);
    raw_release();CHECK(!owned_pages);
    /* Refused serial publication stops only capture, without failing DRBG. */
    raw_sent=0;stream_samples=stream_complete=0;raw_requested=true;serial_refuse=true;
    if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(!raw && !raw_requested && !owned_pages && !pool.failed && !raw_sent);
    serial_refuse=false;serial_lost=true;raw_requested=true;
    if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(!raw && !raw_requested && !owned_pages && !pool.failed && raw_sent==1100);
    serial_lost=false;raw_sent=0;stream_samples=0;raw_requested=true;refuse_pages=true;
    if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(!raw && !raw_requested && !owned_pages && !pool.failed);refuse_pages=false;
    /* A permanent library health failure retires jitter (and the capture) for this boot. */
    raw_requested=true;read_failure=true;read_permanent=true;freed=closes;
    if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(!pool.failed && jitter==JITTER_FAILED && jitter_attempts==1 && !raw && !owned_pages && closes==freed+1);
    read_failure=read_permanent=false;
    g_current=NULL;
    memset(g_boot.options,0,sizeof(g_boot.options));
    booted=seeded=false;g_cpu_tsc=false;boot_per_ms=0;stackprot_init();
    CHECK(pool.failed && !pool.ready && __stack_chk_guard);
    booted=seeded=false;g_cpu_tsc=true;stalled=true;pit_reads=0;stackprot_init();
    CHECK(pool.failed && !pool.ready);stalled=false;
    booted=seeded=false;pit_stalled=true;pit_reads=0;stackprot_init();
    CHECK(pool.failed && !pool.ready && speaker==2);pit_stalled=false;
    /* f6-19, T23 1.0.0 alpha profile: the boot attempt fails (the stub reports
     * the reproduction of t23_test.c); health stays ok and interrupt edges
     * alone initialise the pool; the runtime retry then adds jitter. */
    booted=seeded=false;init_failure=true;pit_reads=0;g_ticks=0;memset(&fast,0,sizeof(fast));drained_last=0;
    stackprot_init();
    CHECK(!pool.failed && !pool.ready && !pool.estimate_bits && pool.source_ready && jitter==JITTER_RETRY && init_first==3 && init_last==3);
    CHECK(strstr(last_jitter,"[entropy] jitter attempt=boot result=-1 osr=3 alarm=0001 stage=sha3 alarm_osr=3 round=2644 rounds=1125/1428/91/0 refused=524288 expired=0 state=retry"));
    CHECK(!strcmp(last_record,"[entropy] sources=0 irq_mask=0000 estimate_bits=0 initialised_ms=0 health=ok reseeds=0 jitter=retry irq_bits=0"));
    worker_started=false;started=0;entropy_start();CHECK(worker_started && jitter_retry_ms==1000);
    g_current=&worker;unsigned boot_reads=reads;drains=0;
    while(!pool.ready){
        for(unsigned i=0;i<128;i++)entropy_irq(i%3==0 ? 0 : i%3==1 ? 14 : 11);
        g_ticks+=10;drains++;
        if(!setjmp(worker_return))worker_entry(NULL);
        CHECK(drains<=128 && !pool.failed);
    }
    CHECK(reads==boot_reads && pool.sources==ENTROPY_SOURCE_IRQ && pool.irq_bits==256 && pool.initialised_ms==g_ticks && jitter_attempts==2);
    char expected[200];
    snprintf(expected,sizeof(expected),"[entropy] sources=2 irq_mask=4801 estimate_bits=256 initialised_ms=%llu health=ok reseeds=1 jitter=retry irq_bits=256",
             (unsigned long long)g_ticks);
    CHECK(!strcmp(last_record,expected));
    uint8_t secret[32];CHECK(!entropy_get(secret,sizeof(secret)));
    printf("T23 IRQ-only initialisation after %u drains of 128 edges: %s\n",drains,expected);
    init_failure=false;g_ticks=jitter_retry_ms;
    if(!setjmp(worker_return))worker_entry(NULL);
    CHECK(jitter==JITTER_READY && reads==boot_reads+8 && pool.sources==(ENTROPY_SOURCE_IRQ|ENTROPY_SOURCE_JITTER) && pool.estimate_bits==256);
    CHECK(strstr(last_jitter,"attempt=runtime result=0 osr=9 ") && strstr(last_record,"[entropy] sources=3 irq_mask=4801 ") && strstr(last_record," jitter=ready "));
    g_current=NULL;
    puts("PASS f6-19 two credited sources: boot jitter failure keeps health=ok, IRQ-only initialisation (sources=2, irq_mask=4801), runtime retry resumes above the failed OSRs and adds jitter (sources=3), backoff and retirement, stopped counter uncredited");
    puts("PASS production boot/guard, absent/stalled TSC/PIT, library-init refusal, IRQ mix, worker saturation/resume/retirement and syscall 91 surface");
    puts("PASS post-boot opt-in raw stream: exactly 1048576 ordered low bytes, bounded hex frames, serial backpressure/drain, concurrent DRBG/reseed, cap/pause, allocation/serial/collector failure cleanup");
    return 0;
}
