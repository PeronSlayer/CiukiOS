/* Production transmitter + wait queue with a clocked 38400 8N1 16550.
 * Fake CPU/PIC/ports only. The mutex models UP IRQ exclusion, not SMP.
 * Deterministic cost model: 1 us/port access, 64 copied wire bytes/us.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/timing.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
#ifndef SERIAL_WITH_OUTPUT
void klog(const char *fmt, ...) { (void)fmt;CHECK(false); }
__attribute__((noreturn)) void panic(const char *fmt, ...)
{ fprintf(stderr,"unexpected panic: %s\n",fmt);abort(); }
#endif

volatile uint64_t g_ticks;
struct ciuki_boot_info g_boot;
struct task *g_current;
volatile bool g_need_resched;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
static pthread_mutex_t cpu_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local uint32_t flags = 0x200;
static _Thread_local bool playback_producer, in_irq;
static bool initializing;
static uint64_t model_ns, critical_start, critical_max;
static const char *section, *max_section;
static unsigned copy_start;
static unsigned queue_size(void);
static void preempt_writer(void);
void crit_label(const char *label) { section=label; }
static uint32_t read_eflags(void) { return flags; }
static uint32_t irq_save(void)
{
    uint32_t f=flags;
    if (f & 0x200) {
        CHECK(!pthread_mutex_lock(&cpu_lock));
        critical_start=model_ns;copy_start=queue_size();section="irq_save";
    }
    flags=0;return f;
}
static void irq_restore(uint32_t f)
{
    if (f & 0x200) {
        unsigned queued=queue_size();
        if (queued>copy_start) model_ns+=(queued-copy_start+63u)/64u*1000u;
        model_ns+=1000;
        if (model_ns-critical_start>critical_max) {
            critical_max=model_ns-critical_start;max_section=section;
        }
        flags=f;CHECK(!pthread_mutex_unlock(&cpu_lock));preempt_writer();
    } else flags=f;
}

static uint8_t regs[8], holding[16], shift;
static uint8_t divisor_bytes[2];
static uint16_t model_base=0x3F8;
static bool present=true, ready, fifo, fifo_enabled, timed, poll_clock, shifting, thr_pending;
static unsigned scratch_fault, divisor_fault;
static bool lsr_float;
static unsigned iir_fault, holding_used, overflows, tx_writes, lsr_reads, io_count, rx_reads;
static unsigned irq_calls, registrations, unmasks, last_irq_bytes;
static irq_handler_t handlers[16];
static bool masked[16];
static char emitted[400000];
static unsigned emitted_count;
static uint64_t clock_units, shift_end;
static bool panic_io(void);
static void advance_us(unsigned us)
{
    uint64_t target=clock_units+(uint64_t)us*38400;
    while (shifting && shift_end<=target) {
        CHECK(emitted_count<sizeof(emitted)-1);
        emitted[emitted_count++]=(char)shift;
        if (holding_used) {
            shift=holding[0];holding_used--;memmove(holding,holding+1,holding_used);
            shift_end+=10000000;
            if (!holding_used) thr_pending=true;
        } else shifting=false;
    }
    clock_units=target;
}
static uint8_t inb(uint16_t p)
{
    CHECK(!playback_producer || in_irq || panic_io());
    CHECK(initializing || in_irq || panic_io());
    CHECK(p>=model_base && p<model_base+8);
    io_count++;model_ns+=1000;
    if (!present) return 0xFF;
    unsigned off=p-model_base;
    if (off==2) {
        uint8_t bits=fifo_enabled ? 0xC0:0;
        if (iir_fault==1 && fifo_enabled) bits=0x80;
        if (iir_fault==2) bits=0xC0;
        if (iir_fault==3 && regs[2]==1) bits=0;
        if ((regs[1]&2) && ready && thr_pending) { thr_pending=false;return bits|2; }
        return bits|1;
    }
    if (off==5) {
        lsr_reads++;
        if (lsr_float) return 0xFF;
        if (poll_clock) advance_us(1);
        return ready ? ((!holding_used ? 0x20:0) | (!shifting ? 0x40:0)):0;
    }
    if (off==7 && ((scratch_fault==1 && regs[7]==0x5A) ||
                   (scratch_fault==2 && regs[7]==0xA5))) return 0;
    if ((regs[3]&0x80) && off<2)
        return divisor_bytes[off] ^ (divisor_fault==off+1 ? 1:0);
    if (!off && !(regs[3]&0x80)) rx_reads++;
    return regs[off];
}
static void outb(uint16_t p,uint8_t value)
{
    CHECK(!playback_producer || in_irq || panic_io());
    CHECK(initializing || in_irq || panic_io());
    CHECK(p>=model_base && p<model_base+8);
    io_count++;model_ns+=1000;
    unsigned off=p-model_base;
    if ((regs[3]&0x80) && off<2) { divisor_bytes[off]=value; return; }
    if (off==1 && !(regs[3]&0x80)) {
        if ((regs[1]^value)&2) thr_pending=(value&2) && !holding_used;
    }
    if (off==2) {
        fifo_enabled=(value&1) && (fifo || iir_fault==1);
        if (value&4) { holding_used=0;thr_pending=true; }
    }
    if (!off && !(regs[3]&0x80)) {
        CHECK(!(flags&0x200) && ready);tx_writes++;thr_pending=false;
        if (timed) {
            if (!shifting) { shifting=true;shift=value;shift_end=clock_units+10000000;thr_pending=true; }
            else if (holding_used<(fifo_enabled && iir_fault!=1 ? 16u:1u)) holding[holding_used++]=value;
            else overflows++;
        } else {
            CHECK(emitted_count<sizeof(emitted)-1);emitted[emitted_count++]=(char)value;thr_pending=true;
        }
    }
    regs[off]=value;
}
void irq_set_handler(unsigned irq,irq_handler_t h)
{ CHECK(irq<16 && h);handlers[irq]=h;registrations++; }
void pic_unmask(unsigned irq) { CHECK(irq<16);masked[irq]=false;unmasks++; }
static void tick(void);
static unsigned evidence_sleeps, evidence_wakes;
static void (*evidence_wait_hook)(void);
void task_start(struct task *t) { CHECK(!(flags&0x200) && t->state==T_BLOCKED);t->state=T_READY;evidence_wakes++; }
void schedule(void)
{
    /* f0-30: waits publish the waiter and restore IF before entering schedule. */
    CHECK((flags&0x200) && g_current &&
          (g_current->state==T_BLOCKED || g_current->state==T_READY));
    struct task *waiter=g_current;evidence_sleeps++;
    if (evidence_wait_hook) {
        void (*hook)(void)=evidence_wait_hook;evidence_wait_hook=NULL;
        struct task other={.state=T_RUNNING};g_current=&other;hook();g_current=waiter;
    }
    /* The real kwait waiter stays linked while device IRQs run. */
    for (unsigned n=0;waiter->state==T_BLOCKED;n++) {
        CHECK(n<2000);tick();
        if (g_ticks>=waiter->wake_tick && waiter->wake_tick) waiter->state=T_READY;
    }
    /* The caller reacquires its queue lock after schedule returns. */
    CHECK(flags==0x200 && waiter->state==T_READY);waiter->state=T_RUNNING;
}
#include "../../src/kernel/core/sync.c"
#include "../../src/kernel/core/serial.c"
static unsigned queue_size(void) { return uart_used; }
static bool panic_io(void) { return uart_panic_mode; }
static void (*preemption)(void);
static unsigned preempt_at,preemptions;
static void preempt_writer(void)
{
    if ((flags&0x200) && preemption && emitted_count>=preempt_at) {
        void (*fn)(void)=preemption;preemption=NULL;preemptions++;fn();
    }
}
static void reset_uart(bool with_fifo)
{
    CHECK(flags==0x200 && !preemption);
    memset(regs,0,sizeof(regs));memset(handlers,0,sizeof(handlers));memset(masked,1,sizeof(masked));
    memset(divisor_bytes,0,sizeof(divisor_bytes));
    fifo=with_fifo;present=ready=true;fifo_enabled=timed=poll_clock=shifting=thr_pending=false;
    scratch_fault=divisor_fault=0;lsr_float=false;
    iir_fault=holding_used=overflows=tx_writes=lsr_reads=io_count=rx_reads=0;
    irq_calls=registrations=unmasks=last_irq_bytes=emitted_count=preemptions=0;
    clock_units=shift_end=0;g_ticks=0;g_current=NULL;g_need_resched=false;
    evidence_sleeps=evidence_wakes=0;evidence_wait_hook=NULL;g_boot.flags=0;
    initializing=true;serial_init(model_base,3);serial_writer_init();serial_writer_init();initializing=false;
    CHECK(serial_present() && uart_fifo_bytes==(with_fifo ? 16u:1u));
    CHECK(registrations==1 && unmasks==1 && regs[3]==3 && (regs[4]&8));
    CHECK(!uart_armed && !(regs[1]&2));
    io_count=lsr_reads=rx_reads=0;model_ns=critical_start=critical_max=0;max_section="none";
}
static bool irq_pending(void)
{ return ready && (regs[1]&2) && (regs[4]&8) && thr_pending && !masked[uart_irq]; }
static void deliver_one(void)
{
    CHECK(!(flags&0x200));in_irq=true;
    unsigned before=tx_writes;struct trap_frame tf={.vector=0x20+uart_irq};
    handlers[uart_irq](&tf);irq_calls++;last_irq_bytes=tx_writes-before;
    CHECK(last_irq_bytes<=uart_fifo_bytes);in_irq=false;
}
static void tick(void)
{
    uint32_t f=irq_save();in_irq=true;
    unsigned before=tx_writes,reads=lsr_reads;
    serial_tick();CHECK(tx_writes==before && lsr_reads==reads);
    advance_us(1000);g_ticks++;in_irq=false;irq_restore(f);
    /* Chardev THRE edges run immediately, with no synthetic PIT pacing. */
    for (unsigned n=0;n<UART_LOG_BYTES+1;n++) {
        f=irq_save();
        if (!irq_pending()) { irq_restore(f);break; }
        deliver_one();irq_restore(f);
        CHECK(n<UART_LOG_BYTES);
    }
}
static void drain_uart(void)
{
    ready=true;
    for (unsigned n=0;n<40000 && (uart_used || shifting);n++) tick();
    CHECK(!uart_used && !uart_record_count && !shifting && !overflows);
    emitted[emitted_count]=0;
}
static uint64_t protected_write(const char *s,size_t n)
{
    uint32_t f=serial_evidence_begin(2*n);
    uint64_t ticket=serial_write_ticket(s,n);irq_restore(f);return ticket;
}
static const char task_record[]="[task] complete record A with a long payload\n";
static const char irq_record[]="[irq] complete record B\n";
static const char expected_pair[]="[task] complete record A with a long payload\r\n[irq] complete record B\r\n";
static bool panic_preemption;
static void second_producer(void)
{
    uint32_t f=irq_save();if (panic_preemption) serial_panic();
    serial_write(irq_record,sizeof(irq_record)-1);irq_restore(f);
}
static void ordering_tests(bool with_fifo)
{
    for (unsigned mode=0;mode<2;mode++) for (unsigned at=1;at<sizeof(task_record);at++) {
        reset_uart(with_fifo);panic_preemption=mode;preempt_at=at;preemption=second_producer;
        serial_write(task_record,sizeof(task_record)-1);drain_uart();
        CHECK(preemptions==1 && !strcmp(emitted,expected_pair));
    }
    reset_uart(with_fifo);playback_producer=true;
    char source[]="producer owns no ports\n";
    serial_write(source,sizeof(source)-1);
    uint32_t f=irq_save();serial_write(source,sizeof(source)-1);irq_restore(f);
    CHECK(protected_write("evidence\n",9));serial_buffered(true);
    struct serial_stats s;serial_snapshot(&s);CHECK(serial_dropped()==0);
    CHECK(!io_count && !lsr_reads && !tx_writes && s.accepted==3);
    playback_producer=false;memset(source,'?',sizeof(source));drain_uart();
    CHECK(!strcmp(emitted,"producer owns no ports\r\nproducer owns no ports\r\nevidence\r\n"));
    /* A spurious/non-TX source must not consume a byte, even if queued. */
    reset_uart(with_fifo);serial_write("abcd",4);
    f=irq_save();deliver_one();irq_restore(f);CHECK(!tx_writes && uart_used==4 && !rx_reads);
    drain_uart();
    printf("serial ordering %s: task/IRQ interleaving, source ownership, zero producer port accesses, IIR source gate PASS\n",with_fifo ? "16550":"16450");
}
static void admission_tests(bool with_fifo)
{
    reset_uart(with_fifo);char block[SERIAL_RECORD_MAX];memset(block,'X',sizeof(block));
    for (unsigned i=0;i<(UART_LOG_BYTES-UART_EVIDENCE_BYTES)/sizeof(block);i++) serial_write(block,sizeof(block));
    CHECK(uart_used==UART_LOG_BYTES-UART_EVIDENCE_BYTES && !uart_dropped);
    CHECK(!serial_write_ticket("REJECT\n",7));
    for (unsigned i=0;i<UART_EVIDENCE_BYTES/sizeof(block);i++) CHECK(protected_write(block,sizeof(block)));
    CHECK(uart_used==UART_LOG_BYTES && !serial_write_ticket("REJECT\n",7));
    CHECK(!protected_write("EVIDENCE REJECT\n",16));
    CHECK(uart_dropped==3 && uart_periodic_dropped==2 && uart_evidence_dropped==1);
    CHECK(!io_count);memset(block,'?',sizeof(block));drain_uart();CHECK(emitted_count==UART_LOG_BYTES);
    for (unsigned i=0;i<emitted_count;i++) CHECK(emitted[i]=='X');
    reset_uart(with_fifo);
    for (unsigned i=0;i<UART_RECORD_SLOTS-UART_EVIDENCE_SLOTS;i++) serial_write("x",1);
    CHECK(!serial_write_ticket("rejected",8));
    for (unsigned i=0;i<UART_EVIDENCE_SLOTS;i++) CHECK(protected_write("x",1));
    CHECK(uart_record_count==UART_RECORD_SLOTS && !protected_write("rejected",8));
    drain_uart();CHECK(emitted_count==UART_RECORD_SLOTS && uart_dropped==2);
    reset_uart(with_fifo);serial_write("x",SIZE_MAX);serial_write("x",SERIAL_RECORD_MAX+1);
    CHECK(uart_dropped==2 && !uart_used && !io_count);
    char ends[SERIAL_RECORD_MAX];memset(ends,'\n',sizeof(ends));serial_write(ends,sizeof(ends));drain_uart();
    CHECK(emitted_count==UART_RECORD_BYTES);
    for (unsigned i=0;i<emitted_count;i+=2) CHECK(emitted[i]=='\r' && emitted[i+1]=='\n');
    /* Wrap both rings without modifying accepted records or their tickets. */
    reset_uart(with_fifo);memset(block,'W',sizeof(block));block[sizeof(block)-1]='\n';
    for (unsigned i=0;i<1100;i++) { serial_write(block,i<40 ? sizeof(block):1);drain_uart(); }
    CHECK(!uart_dropped && uart_accepted==1100 && uart_sent==1100);
    printf("serial admission %s: whole byte/descriptor drops, reserved evidence bytes/slots, CRLF/max bounds, both rings wrap PASS\n",with_fifo ? "16550":"16450");
}
enum { PRODUCERS=2, RECORDS=100 };
static atomic_uint producers_done;
static size_t producer_record(char *s,size_t size,unsigned id,unsigned number)
{
    int n=snprintf(s,size,"[producer=%u] seq=%03u payload=AAAAAAAAAAAAAAAA\n",id,number);
    CHECK(n>0 && (size_t)n<size);return (size_t)n;
}
static void *producer(void *arg)
{
    unsigned id=*(unsigned *)arg;playback_producer=true;
    for (unsigned i=0;i<RECORDS;i++) {
        char s[96];size_t n=producer_record(s,sizeof(s),id,i);
        if (id) { uint32_t f=irq_save();serial_write(s,n);irq_restore(f); }
        else serial_write(s,n);
        sched_yield();
    }
    playback_producer=false;atomic_fetch_add(&producers_done,1);return NULL;
}
static void *sender(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t f=irq_save();bool done=atomic_load(&producers_done)==PRODUCERS && !uart_used;
        irq_restore(f);if (done) break;tick();sched_yield();
    }
    return NULL;
}
static void concurrent_tests(bool with_fifo)
{
    reset_uart(with_fifo);atomic_store(&producers_done,0);
    pthread_t a,b,c;unsigned ids[2]={0,1};
    CHECK(!pthread_create(&c,NULL,sender,NULL));CHECK(!pthread_create(&a,NULL,producer,&ids[0]));
    CHECK(!pthread_create(&b,NULL,producer,&ids[1]));
    CHECK(!pthread_join(a,NULL) && !pthread_join(b,NULL) && !pthread_join(c,NULL));
    emitted[emitted_count]=0;bool seen[2][RECORDS]={{false}};unsigned rows=0;
    for (const char *p=emitted;*p;) {
        const char *end=strstr(p,"\r\n");unsigned id,n;CHECK(end);
        CHECK(sscanf(p,"[producer=%u] seq=%u",&id,&n)==2 && id<2 && n<RECORDS && !seen[id][n]);
        seen[id][n]=true;char expected[96];size_t length=producer_record(expected,sizeof(expected),id,n);
        expected[length-1]='\r';expected[length++]='\n';
        CHECK((size_t)(end+2-p)==length && !memcmp(p,expected,length));rows++;p=end+2;
    }
    CHECK(rows==200 && !uart_dropped);
    printf("serial concurrent %s: 200 complete ordered records, zero drops/interleaved/resent bytes PASS\n",with_fifo ? "16550":"16450");
}
static void line_rate_case(bool with_fifo,unsigned fault)
{
    reset_uart(with_fifo);iir_fault=fault;regs[7]=0x39;
    initializing=true;serial_init(model_base,3);serial_writer_init();initializing=false;
    CHECK(regs[7]==0x39 && uart_fifo_bytes==(with_fifo && !fault ? 16u:1u));
    timed=true;lsr_reads=io_count=0;
    char record[96],expected[8192];unsigned length=0;
    for (unsigned i=0;i<100;i++) {
        int n=snprintf(record,sizeof(record),"[usb] controller=%02u payload=abcdefghijklmnopqrstuvwxyz0123456789\n",i);
        serial_write(record,(size_t)n);memcpy(expected+length,record,(size_t)n-1);length+=(unsigned)n-1;
        expected[length++]='\r';expected[length++]='\n';
    }
    CHECK(!uart_dropped && !io_count);
    unsigned milliseconds=0;
    while (uart_used || shifting) {
        tick();milliseconds++;CHECK(milliseconds<10000 && !overflows);
        CHECK((uint64_t)emitted_count*100<=(uint64_t)milliseconds*384);
        CHECK(tx_writes-emitted_count<=uart_fifo_bytes+1);
    }
    CHECK(emitted_count==length && !memcmp(emitted,expected,length) && !lsr_reads && !rx_reads && irq_calls);
    /* Empty IRQ disabled; the first subsequent record is restarted with
     * one control write by IRQ0, then delivered by an actual THRE IRQ. */
    CHECK(!uart_armed && !(regs[1]&2));unsigned old=io_count;
    for (unsigned i=0;i<10;i++) tick();CHECK(io_count==old);
    serial_write("idle restart\n",13);CHECK(io_count==old);drain_uart();
    printf("serial 38400 %s IIR-fault=%u: %u exact wire bytes/%u ms, zero polling/FIFO overflows, idle rearm PASS\n",with_fifo ? "16550":"16450",fault,length,milliseconds);
}
static void panic_tests(bool with_fifo)
{
    reset_uart(with_fifo);timed=poll_clock=true;serial_write(task_record,sizeof(task_record)-1);tick();
    CHECK(tx_writes && uart_used);
    unsigned before=tx_writes;uint32_t f=irq_save();serial_panic();
    serial_write(irq_record,sizeof(irq_record)-1);irq_restore(f);
    emitted[emitted_count]=0;
    CHECK(!strcmp(emitted,expected_pair) && tx_writes==sizeof(expected_pair)-1 && tx_writes>before);
    CHECK(!shifting && !holding_used && !(regs[1]&2) && !uart_used && serial_present());
    unsigned writes=tx_writes;tick();CHECK(tx_writes==writes);
    reset_uart(with_fifo);ready=false;serial_write(task_record,sizeof(task_record)-1);
    f=irq_save();serial_panic();irq_restore(f);
    CHECK(!serial_present() && !emitted_count && lsr_reads==UART_POLL_LIMIT*UART_DEAD_LIMIT);
    reset_uart(with_fifo);timed=true;serial_write("x",1);tick();
    f=irq_save();serial_panic();irq_restore(f);
    CHECK(!serial_present() && lsr_reads==UART_POLL_LIMIT); /* TEMT, no THRE confusion */
    printf("serial panic %s: partial FIFO takeover, no duplicate bytes/reset, final TEMT, finite stuck-TX/TEMT bounds PASS\n",with_fifo ? "16550":"16450");
}
static void evidence_test(void)
{
    reset_uart(true);char fill[SERIAL_RECORD_MAX];memset(fill,'F',sizeof(fill));
    for (unsigned i=0;i<UART_LOG_BYTES/sizeof(fill);i++) CHECK(protected_write(fill,sizeof(fill)));
    struct task task={.state=T_RUNNING};g_current=&task;
    uint32_t f=serial_evidence_begin(256);serial_write("evidence\n",9);irq_restore(f);
    CHECK(evidence_sleeps==1 && evidence_wakes==1 && !uart_dropped && !uart_space.head);
    g_current=NULL;drain_uart();CHECK(strstr(emitted,"evidence\r\n"));
    /* Drain between initial capacity check and waiter registration. Real
     * kwait must observe space without sleeping or losing a wake. */
    reset_uart(true);
    for (unsigned i=0;i<UART_LOG_BYTES/sizeof(fill);i++) CHECK(protected_write(fill,sizeof(fill)));
    g_current=&task;preempt_at=0;preemption=drain_uart;
    f=serial_evidence_begin(256);serial_write("raced\n",6);irq_restore(f);
    CHECK(!evidence_sleeps && !uart_space.head);g_current=NULL;drain_uart();
    reset_uart(true);
    for (unsigned i=0;i<UART_LOG_BYTES/sizeof(fill);i++) CHECK(protected_write(fill,sizeof(fill)));
    g_current=&task;f=irq_save();CHECK(!protected_write("irq\n",4));irq_restore(f);
    task.preempt_holds=1;CHECK(!protected_write("held\n",5));task.preempt_holds=0;
    CHECK(!evidence_sleeps && uart_evidence_dropped==2);g_current=NULL;drain_uart();
    puts("serial evidence: production kwait blocks/wakes from TX IRQ, check/register race has no lost wake, full IRQ/held producers drop without sleeping PASS");
}
static void ticket_tests(bool with_fifo)
{
    reset_uart(with_fifo);ready=false;
    uint64_t first=serial_write_ticket(task_record,sizeof(task_record)-1);
    serial_write(irq_record,sizeof(irq_record)-1);
    uint64_t last=serial_write_ticket("last\n",5);struct serial_stats s;serial_snapshot(&s);
    CHECK(first==1 && last==3 && s.accepted==3 && !s.sent);tick();serial_snapshot(&s);CHECK(!s.sent);
    ready=true;uint32_t f=irq_save();in_irq=true;serial_tick();in_irq=false;deliver_one();irq_restore(f);
    serial_snapshot(&s);CHECK(!s.sent && s.queued<sizeof(expected_pair)+5);
    drain_uart();serial_snapshot(&s);CHECK(s.sent==last && s.accepted==last && !s.queued && !s.resent);
    reset_uart(with_fifo);timed=true;uint64_t tickets[100];unsigned length=0;char expected[4096];
    for (unsigned i=0;i<100;i++) {
        char srecord[64];int n=ksnprintf(srecord,sizeof(srecord),"[hwcheck] serial record=%03u/100\n",i+1);
        tickets[i]=protected_write(srecord,(size_t)n);CHECK(tickets[i]);
        memcpy(expected+length,srecord,(size_t)n-1);length+=(unsigned)n-1;expected[length++]='\r';expected[length++]='\n';
    }
    CHECK(!lsr_reads && !io_count);drain_uart();serial_snapshot(&s);unsigned sent=0;
    for (unsigned i=0;i<100;i++) if (tickets[i]<=s.sent) sent++;
    CHECK(sent==100 && !s.dropped && !s.resent && !s.queued && !s.evidence_dropped && !s.periodic_dropped);
    CHECK(emitted_count==length && !memcmp(emitted,expected,length));
    printf("serial hardware-check %s: requested=100 accepted=100 whole_sent=%u resent=%u dropped=%u periodic=%u evidence=%u queued=%u PASS\n",with_fifo ? "16550":"16450",sent,s.resent,s.dropped,s.periodic_dropped,s.evidence_dropped,s.queued);
}
static void latency_test(void)
{
    reset_uart(true);char maximum[SERIAL_RECORD_MAX];memset(maximum,'\n',sizeof(maximum));
    for (unsigned i=0;i<10;i++) { serial_write(maximum,sizeof(maximum));drain_uart(); }
    CHECK(critical_max<100000 && !lsr_reads && !strcmp(max_section,"serial_queue"));
    printf("[kernel] irqoff_section=%s irqoff_max_us=%llu violations=0 model=1us_port_64bytes_per_us_copy PASS\n",max_section,(unsigned long long)(critical_max/1000));
    reset_uart(true);serial_write(maximum,sizeof(maximum));critical_max=0;drain_uart();
    CHECK(critical_max<100000 && !strcmp(max_section,"serial_irq"));
    printf("[kernel] irqoff_section=%s irqoff_max_us=%llu violations=0 model=1us_port_64bytes_per_us_copy PASS\n",max_section,(unsigned long long)(critical_max/1000));
}
int main(void)
{
    line_rate_case(true,0);line_rate_case(false,0);line_rate_case(true,1);line_rate_case(false,2);line_rate_case(true,3);
    for (unsigned kind=0;kind<2;kind++) {
        ordering_tests(kind);admission_tests(kind);concurrent_tests(kind);panic_tests(kind);ticket_tests(kind);
    }
    evidence_test();latency_test();
    for (unsigned i=0;i<4;i++) {
        static const uint16_t bases[]={0x3F8,0x2F8,0x3E8,0x2E8};model_base=bases[i];reset_uart(true);
        CHECK(uart_irq==(i%2 ? 3u:4u));serial_write("route\n",6);drain_uart();CHECK(!strcmp(emitted,"route\r\n"));
    }
    model_base=0x3F8;reset_uart(true);present=false;initializing=true;
    serial_init(model_base,3);serial_writer_init();initializing=false;CHECK(!serial_present());
    unsigned before=io_count;serial_write("absent\n",7);tick();uint32_t f=irq_save();serial_panic();irq_restore(f);
    CHECK(io_count==before && !uart_used);
    initializing=true;serial_init(0,3);serial_init(0x1234,3);initializing=false;CHECK(!serial_present() && io_count==before);
    puts("serial IRQ transmitter: same hardware/chardev path, IRQ3/4 routes, idempotent registration, absence and unsupported mapping PASS");
    return 0;
}
