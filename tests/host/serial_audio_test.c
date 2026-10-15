/* F4-14/F4-16: production serial queue and a clocked 38400 8N1 UART.
 * Deterministic IRQ preemption plus two host producers and a burst sender.
 * The mutex models the kernel's uniprocessor IRQ lock, not an SMP UART. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <time.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)

volatile uint64_t g_ticks;
static pthread_mutex_t cpu_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local uint32_t flags = 0x200;
static _Thread_local bool playback_producer;
static void preempt_writer(void);
static bool slow_ports;
static uint64_t latency_us, critical_start, critical_max;
static unsigned pending_ticks, lost_ticks;
static void latency_advance(unsigned us)
{
    uint64_t before=latency_us/1000;
    latency_us+=us;
    unsigned ticks=(unsigned)(latency_us/1000-before);
    if (flags & 0x200) g_ticks+=ticks;
    else pending_ticks+=ticks;
}
static uint32_t irq_save(void)
{
    uint32_t f = flags;
    if (f & 0x200) {
        CHECK(!pthread_mutex_lock(&cpu_lock));
        critical_start=latency_us;
    }
    flags = 0;
    return f;
}
static void irq_restore(uint32_t f)
{
    if ((f & 0x200) && latency_us-critical_start>critical_max)
        critical_max=latency_us-critical_start;
    flags = f;
    if (f & 0x200) {
        if (pending_ticks) { g_ticks++;lost_ticks+=pending_ticks-1;pending_ticks=0; }
        CHECK(!pthread_mutex_unlock(&cpu_lock));
        preempt_writer();
    }
}

static uint8_t regs[8];
static bool present = true, ready, fifo;
static bool fifo_enabled, timed, poll_clock;
static unsigned iir_fault;
static unsigned lsr_reads, filled, lsr_if_off;
static char emitted[400000];
static unsigned emitted_count;
/* Separate holding FIFO and shift register. LSR reads never empty these.
 * Time is in baud-scaled microseconds: ten 8N1 bits cost 10,000,000 units. */
static uint8_t holding[16], shift;
static unsigned holding_used, overflows, tx_writes;
static bool shifting;
static uint64_t clock_units, shift_end;
static void advance_us(unsigned us)
{
    uint64_t target = clock_units + (uint64_t)us * 38400;
    while (shifting && shift_end <= target) {
        CHECK(emitted_count < sizeof(emitted) - 1);
        emitted[emitted_count++] = (char)shift;
        if (holding_used) {
            shift = holding[0]; holding_used--;
            memmove(holding, holding + 1, holding_used);
            shift_end += 10000000;
        } else shifting = false;
    }
    clock_units = target;
}
static void (*preemption)(void);
static unsigned preempt_at, preemptions;
static void preempt_writer(void)
{
    if ((flags & 0x200) && preemption && emitted_count >= preempt_at) {
        void (*fn)(void) = preemption;
        preemption = NULL;
        preemptions++;
        fn();
    }
}
static uint8_t inb(uint16_t p)
{
    CHECK(p >= 0x3F8 && p < 0x400);
    if (!present) return 0xFF;
    unsigned off = p - 0x3F8;
    if (off == 2) {
        if (iir_fault == 2) return 0xC0; /* stuck FIFO indication */
        if (iir_fault == 3 && regs[2] == 1) return 0; /* enable did not stick */
        return fifo && fifo_enabled ? (iir_fault == 1 ? 0x80 : 0xC0) : 0;
    }
    if (off == 5) {
        CHECK(!playback_producer);
        lsr_reads++;
        if (!(flags & 0x200)) lsr_if_off++;
        if (slow_ports) latency_advance(130);
        if (timed) {
            if (poll_clock) advance_us(1);
            return ready ? ((!holding_used ? 0x20 : 0) | (!shifting ? 0x40 : 0)) : 0;
        }
        if (ready) filled = 0;
        return ready ? 0x20 : 0;
    }
    return regs[off];
}
static void outb(uint16_t p, uint8_t value)
{
    CHECK(p >= 0x3F8 && p < 0x400);
    unsigned off = p - 0x3F8;
    if (off == 2) {
        fifo_enabled = fifo && (value & 1) && iir_fault != 1;
        /* Original 16550 advertises a broken FIFO, but cannot buffer it. */
        if (iir_fault == 1) fifo_enabled = (value & 1) != 0;
        if (value & 4) holding_used = 0; /* TX reset does not clear TSR */
    }
    if (off == 0 && !(regs[3] & 0x80)) {
        CHECK(!(flags & 0x200) && ready);
        tx_writes++;
        if (slow_ports) latency_advance(130);
        if (timed) {
            if (!shifting) {
                shifting = true; shift = value; shift_end = clock_units + 10000000;
            } else if (holding_used < (fifo_enabled && iir_fault != 1 ? 16u : 1u))
                holding[holding_used++] = value;
            else overflows++; /* deliberately observable loss, no assertion */
        } else {
            CHECK(emitted_count < sizeof(emitted) - 1);
            CHECK(++filled <= (fifo_enabled ? 16u : 1u));
            emitted[emitted_count++] = (char)value;
        }
    }
    regs[off] = value;
    preempt_writer();
}
#include <ciuki/task.h>
struct task *g_current;
static struct task writer_task;
static void (*writer_fn)(void *);
static jmp_buf writer_done;
static bool writer_running;
static unsigned writer_creations;
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio)
{ CHECK(!strcmp(name, "log-writer") && fn && !arg && prio == P_DEVICE); writer_creations++;writer_fn = fn; return &writer_task; }
void task_start(struct task *t) { CHECK(t == &writer_task); }
static unsigned evidence_sleeps;
static void (*evidence_wait_hook)(void);
void task_sleep_ms(uint32_t ms)
{
    CHECK(ms==1);
    if (writer_running) longjmp(writer_done,1);
    CHECK(g_current && (flags & 0x200));g_ticks++;evidence_sleeps++;
    if (evidence_wait_hook) {
        void (*hook)(void)=evidence_wait_hook;evidence_wait_hook=NULL;hook();
    }
}
void kpreempt_point(void) { }
#include "../../src/kernel/core/serial.c"

static void reset_uart(bool with_fifo)
{
    CHECK(flags == 0x200 && !preemption);
    memset(regs,0,sizeof(regs));
    fifo=with_fifo;present=ready=true;fifo_enabled=timed=poll_clock=slow_ports=false;
    iir_fault=holding_used=overflows=tx_writes=pending_ticks=lost_ticks=0;
    shifting=false;clock_units=shift_end=latency_us=critical_start=critical_max=0;
    emitted_count=filled=lsr_reads=lsr_if_off=preemptions=writer_creations=0;g_ticks=0;
    g_current=NULL;evidence_sleeps=0;evidence_wait_hook=NULL;
    serial_init(0x3F8,3);
    CHECK(serial_present() && uart_fifo_bytes==(fifo ? 16u:1u));
    CHECK(uart_budget==2880 && uart_burst_bytes==3);
    serial_writer_init();
    serial_writer_init();CHECK(writer_creations==1);
}
static void writer_step(void)
{
    CHECK(flags==0x200 && !writer_running);
    writer_running=true;
    if (!setjmp(writer_done)) writer_fn(NULL);
    writer_running=false;
}
static void tick(void)
{
    uint32_t f=irq_save();
    unsigned before=lsr_reads,at=tx_writes;
    serial_tick();
    CHECK(lsr_reads==before && tx_writes==at); /* IRQ is never a consumer */
    if (timed) advance_us(1000);
    if (!slow_ports) g_ticks++;
    irq_restore(f);
    if (slow_ports) latency_advance(1000);
    writer_step();
}
static void drain_uart(void)
{
    ready=true;
    for (unsigned i=0;i<sizeof(emitted) && (uart_used || uart_drop_notice || shifting);i++) tick();
    CHECK(!uart_used && !uart_record_count && !uart_drop_notice && !shifting);
    emitted[emitted_count]=0;
}
static const char task_record[]="[task] complete record A with a long payload\n";
static const char irq_record[]="[irq] complete record B\n";
static const char expected_pair[]=
    "[task] complete record A with a long payload\r\n[irq] complete record B\r\n";
static bool panic_preemption;
static void second_producer(void)
{
    uint32_t f=irq_save();
    if (panic_preemption) serial_panic();
    serial_write(irq_record,sizeof(irq_record)-1);
    unsigned reads=lsr_reads,writes=tx_writes;
    serial_tick();CHECK(reads==lsr_reads && writes==tx_writes);
    irq_restore(f);
}
static void preemption_tests(bool with_fifo)
{
    for (unsigned mode=0;mode<2;mode++) {
        panic_preemption=mode;
        for (unsigned at=1;at<sizeof(task_record);at++) {
            reset_uart(with_fifo);
            preempt_at=at;preemption=second_producer;
            serial_write(task_record,sizeof(task_record)-1);
            drain_uart();
            CHECK(preemptions==1 && !strcmp(emitted,expected_pair));
        }
    }
}
static void admission_tests(bool with_fifo)
{
    reset_uart(with_fifo);ready=false;
    char source[1001];memset(source,'P',sizeof(source));source[1000]='\n';
    playback_producer=true;
    serial_write(source,sizeof(source));
    uint32_t f=irq_save();serial_write(source,sizeof(source));irq_restore(f);
    unsigned used_before=uart_used;
    serial_write(source,sizeof(source));
    serial_write(source,SIZE_MAX); /* reject before reading the source */
    playback_producer=false;
    CHECK(!lsr_reads && !tx_writes && uart_used==used_before && uart_record_count==2 && serial_dropped()==2);
    memset(source,'?',sizeof(source));drain_uart();
    CHECK(emitted_count==2004+strlen("[serial] dropped=2\r\n"));
    for (unsigned row=0;row<2;row++) {
        for (unsigned i=0;i<1000;i++) CHECK(emitted[row*1002+i]=='P');
        CHECK(!memcmp(emitted+row*1002+1000,"\r\n",2));
    }
    CHECK(!strcmp(emitted+2004,"[serial] dropped=2\r\n"));
    /* The window counts expanded wire bytes, and does not bank idle quota. */
    reset_uart(with_fifo);char newlines[SERIAL_RECORD_MAX];memset(newlines,'\n',sizeof(newlines));
    serial_write(newlines,sizeof(newlines));CHECK(serial_dropped()==1 && !uart_used);
    g_ticks=999;serial_write("ok\n",3);CHECK(uart_admitted==4);
    g_ticks=1000;serial_write("next\n",5);CHECK(uart_admitted==6);
    g_ticks=100000;serial_write("later\n",6);CHECK(uart_admitted==7);
    drain_uart();CHECK(!strcmp(emitted,"ok\r\nnext\r\nlater\r\n[serial] dropped=1\r\n"));
    /* Descriptor capacity is bounded independently of byte capacity. */
    reset_uart(with_fifo);
    for (unsigned i=0;i<UART_RECORD_SLOTS+1;i++) serial_write("x",1);
    CHECK(uart_record_count==UART_RECORD_SLOTS && uart_dropped==1);
    drain_uart();
    /* Quota windows cannot hide a stopped consumer's byte-ring overflow. */
    reset_uart(with_fifo);char block[SERIAL_RECORD_MAX];memset(block,'X',sizeof(block));
    for (unsigned i=0;i<UART_LOG_BYTES/sizeof(block);i++) {
        g_ticks+=1000;serial_write(block,sizeof(block));
    }
    CHECK(uart_used==UART_LOG_BYTES && !uart_dropped);
    g_ticks+=1000;serial_write("REJECT\n",7);
    CHECK(uart_used==UART_LOG_BYTES && uart_dropped==1);
    memset(block,'?',sizeof(block));drain_uart();
    CHECK(emitted_count==UART_LOG_BYTES+strlen("[serial] dropped=1\r\n"));
    for (unsigned i=0;i<UART_LOG_BYTES;i++) CHECK(emitted[i]=='X');
    CHECK(!strcmp(emitted+UART_LOG_BYTES,"[serial] dropped=1\r\n"));
    /* Wrap both rings, publishing each complete maximum-size record. */
    reset_uart(with_fifo);char large[SERIAL_RECORD_MAX];memset(large,'W',sizeof(large));large[sizeof(large)-1]='\n';
    for (unsigned i=0;i<40;i++) {
        uint32_t saved=irq_save();g_ticks+=1000;irq_restore(saved);
        serial_write(large,sizeof(large));drain_uart();
    }
    CHECK(!uart_dropped && emitted_count==40*(SERIAL_RECORD_MAX+1));
    for (unsigned i=0;i<40;i++) {
        const char *record=emitted+i*(SERIAL_RECORD_MAX+1);
        for (unsigned j=0;j<SERIAL_RECORD_MAX-1;j++) CHECK(record[j]=='W');
        CHECK(!memcmp(record+SERIAL_RECORD_MAX-1,"\r\n",2));
    }
}
enum { PRODUCERS=2, RECORDS=100 };
static atomic_uint producers_done;
static size_t producer_record(char *s,size_t size,unsigned id,unsigned seqno)
{
    int n=snprintf(s,size,"[producer=%u] seq=%03u payload=AAAAAAAAAAAAAAAA\n",id,seqno);
    CHECK(n>0 && (size_t)n<size);return (size_t)n;
}
static void *producer(void *arg)
{
    unsigned id=*(unsigned *)arg;playback_producer=true;
    for (unsigned i=0;i<RECORDS;i++) {
        char record[96];size_t n=producer_record(record,sizeof(record),id,i);
        if (id) { uint32_t f=irq_save();serial_write(record,n);irq_restore(f); }
        else serial_write(record,n);
        sched_yield();
    }
    playback_producer=false;atomic_fetch_add(&producers_done,1);return NULL;
}
static void *sender(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t f=irq_save();
        bool done=atomic_load(&producers_done)==PRODUCERS && !uart_used && !uart_drop_notice;
        irq_restore(f);if (done) break;
        tick();sched_yield();
    }
    return NULL;
}
static void concurrent_tests(bool with_fifo)
{
    reset_uart(with_fifo);atomic_store(&producers_done,0);
    pthread_t a,b,c;unsigned ids[2]={0,1};
    CHECK(!pthread_create(&c,NULL,sender,NULL));
    CHECK(!pthread_create(&a,NULL,producer,&ids[0]));CHECK(!pthread_create(&b,NULL,producer,&ids[1]));
    CHECK(!pthread_join(a,NULL) && !pthread_join(b,NULL) && !pthread_join(c,NULL));
    emitted[emitted_count]=0;bool seen[2][RECORDS]={{false}};unsigned kept=0,notices=0;
    for (const char *line=emitted;*line;) {
        const char *end=strstr(line,"\r\n");CHECK(end);
        if (!strncmp(line,"[serial] dropped=",17)) notices++;
        else {
            unsigned id,n;CHECK(sscanf(line,"[producer=%u] seq=%u",&id,&n)==2);
            CHECK(id<2 && n<RECORDS && !seen[id][n]);seen[id][n]=true;
            char expected[96];size_t bytes=producer_record(expected,sizeof(expected),id,n);
            expected[bytes-1]='\r';expected[bytes++]='\n';
            CHECK((size_t)(end+2-line)==bytes && !memcmp(line,expected,bytes));kept++;
        }
        line=end+2;
    }
    CHECK(kept+uart_dropped==2*RECORDS && kept);
    CHECK((uart_dropped!=0)==(notices!=0));
    printf("serial concurrent %s: %u whole records + %u whole drops = 200, %u notices PASS\n",
           with_fifo ? "16550":"16450",kept,uart_dropped,notices);
}
static void line_rate_case(bool with_fifo,unsigned fault)
{
    reset_uart(with_fifo);iir_fault=fault;regs[7]=0x39;serial_init(0x3F8,3);
    CHECK(regs[7]==0x39 && uart_fifo_bytes==(with_fifo && !fault ? 16u:1u));
    timed=true;
    char expected[8192];unsigned length=0,accepted=0;
    for (unsigned i=0;i<100;i++) {
        char record[96];int n=snprintf(record,sizeof(record),"[usb] controller=%02u payload=abcdefghijklmnopqrstuvwxyz0123456789\n",i);
        unsigned drops=uart_dropped;serial_write(record,(size_t)n);
        if (drops==uart_dropped) {
            memcpy(expected+length,record,(size_t)n-1);length+=(unsigned)n-1;
            expected[length++]='\r';expected[length++]='\n';accepted++;
        }
    }
    unsigned dropped=100-accepted;
    length+=(unsigned)snprintf(expected+length,sizeof(expected)-length,"[serial] dropped=%u\r\n",dropped);
    CHECK(dropped && !lsr_reads && !tx_writes && uart_admitted<=uart_budget-UART_NOTICE_BYTES);
    unsigned milliseconds=0;
    while (uart_used || uart_drop_notice || shifting) {
        tick();milliseconds++;
        CHECK(milliseconds<10000 && !overflows);
        CHECK((uint64_t)emitted_count*100 <= (uint64_t)milliseconds*384);
        CHECK(tx_writes-emitted_count<=(with_fifo && !fault ? 17u:2u));
    }
    CHECK(emitted_count==length && !memcmp(emitted,expected,length) && !lsr_if_off);
    printf("serial 38400 %s IIR-fault=%u: %u complete wire bytes/%u ms, %u whole drops, zero FIFO overflows PASS\n",
           with_fifo ? "16550A":"16450",fault,emitted_count,milliseconds,dropped);
    /* No second normal sender, including timer calls during a busy FIFO. */
    unsigned before=lsr_reads,at=tx_writes;
    uint32_t f=irq_save();serial_tick();irq_restore(f);
    CHECK(before==lsr_reads && at==tx_writes);
}
static void panic_tests(bool with_fifo)
{
    reset_uart(with_fifo);serial_buffered(true);
    serial_write(task_record,sizeof(task_record)-1);tick();
    uint32_t f=irq_save();serial_panic();serial_write(irq_record,sizeof(irq_record)-1);irq_restore(f);
    emitted[emitted_count]=0;CHECK(!strcmp(emitted,expected_pair) && !uart_used);
    reset_uart(with_fifo);ready=false;serial_write(task_record,sizeof(task_record)-1);
    f=irq_save();serial_panic();irq_restore(f);
    CHECK(!serial_present() && !emitted_count && uart_used==sizeof(task_record));
    CHECK(lsr_reads==UART_POLL_LIMIT*UART_DEAD_LIMIT);
    reset_uart(with_fifo);timed=poll_clock=true;
    serial_write(task_record,sizeof(task_record)-1);
    f=irq_save();serial_panic();irq_restore(f);
    while (shifting) advance_us(1000);
    CHECK(!overflows && emitted_count==sizeof(task_record));
}
static void slow_port_test(void)
{
    reset_uart(true);slow_ports=true;
    /* The old 16-write IRQ lock accounts for 2080 us at this port latency. */
    uint32_t f=irq_save();latency_advance(16*130);irq_restore(f);
    CHECK(critical_max==2080 && lost_ticks==1);
    reset_uart(true);slow_ports=true;
    for (unsigned i=0;i<40;i++) serial_write(task_record,sizeof(task_record)-1);
    drain_uart();CHECK(!uart_dropped && critical_max==130 && !lost_ticks && !lsr_if_off);
    printf("serial slow ports: 16-write lock 2080 us/1 lost tick; production max IRQ-off %llu us/zero lost ticks, readiness with IF=1 PASS\n",
           (unsigned long long)critical_max);
}
static void evidence_test(void)
{
    reset_uart(true);
    char record[1408];memset(record,'E',sizeof(record));
    serial_write(record,sizeof(record));serial_write(record,sizeof(record));
    CHECK(uart_admitted==uart_budget-UART_NOTICE_BYTES);
    struct task task={0};g_current=&task;
    uint32_t f=serial_evidence_begin(256);
    CHECK(!(flags & 0x200));serial_write("evidence\n",9);irq_restore(f);
    CHECK(evidence_sleeps==1000 && !uart_dropped && !lsr_reads && !tx_writes);
    /* IRQ/held-lock evidence is bounded and drops the entire publication. */
    uart_admitted=uart_budget-UART_NOTICE_BYTES;
    f=irq_save();uint32_t saved=serial_evidence_begin(256);
    serial_write("irq\n",4);irq_restore(saved);irq_restore(f);
    task.preempt_holds=1;saved=serial_evidence_begin(256);
    serial_write("held\n",5);irq_restore(saved);
    CHECK(uart_dropped==2 && evidence_sleeps==1000);
    g_current=NULL;drain_uart();
    CHECK(strstr(emitted,"evidence\r\n") && !strstr(emitted,"irq\r\n") && !strstr(emitted,"held\r\n"));
    puts("serial evidence: task waits for quota without UART polling/drops; IRQ and held-lock evidence drops whole without sleeping PASS");
}
int main(void)
{
    line_rate_case(true,0);line_rate_case(false,0);
    line_rate_case(true,1);line_rate_case(false,2);line_rate_case(true,3);
    for (unsigned kind=0;kind<2;kind++) {
        admission_tests(kind);preemption_tests(kind);concurrent_tests(kind);panic_tests(kind);
    }
    slow_port_test();evidence_test();
    reset_uart(true);present=false;serial_init(0x3F8,3);
    CHECK(!serial_present());unsigned before=lsr_reads;
    serial_write("absent\n",7);tick();uint32_t f=irq_save();serial_panic();irq_restore(f);
    CHECK(lsr_reads==before && !uart_used);
    puts("serial: single task consumer, no IRQ/producer UART I/O, CRLF byte quota, descriptor/byte wrap, 2KiB bounds, whole drops, panic takeover/timeout/absence PASS");
    return 0;
}
