/* Real formatted records and user envelopes share the production UART queue.
 * Reuse the IRQ/FIFO model, including its concurrent burst sender. */
#define FS_HOST
#define main serial_fixture_main
#include "serial_audio_test.c"
#undef main
static void cli(void) { flags=0; }
static void hlt(void) { abort(); }
static uint32_t read_cr2(void) { return 0; }
void console_write(const char *s, size_t n) { (void)s; (void)n; }
void bootlog_capture(const char *s, size_t n) { (void)s; (void)n; }
void bootlog_capture_ring(const char *s, size_t n) { (void)s; (void)n; }
#include "../../src/kernel/core/output.c"
static bool verify_blackbox;
static uint32_t capture_flags;
static unsigned captured_records;
static char captured_record[256];
void blackbox_capture(const char *s, size_t n)
{
    if (!verify_blackbox) return;
    CHECK(flags==capture_flags && n && n<sizeof(captured_record) && s[n-1]=='\n');
    memcpy(captured_record,s,n);captured_record[n]=0;captured_records++;
}
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include "../../src/kernel/proc/devnodes.c"

static bool console_emitter;
static void *record_emitter(void *arg)
{
    unsigned id=*(unsigned *)arg;
    playback_producer=true;
    for (unsigned i=0;i<100;i++) {
        /* Isolate byte integrity from admission: each bounded producer
         * starts a fresh virtual quota window; the clocked burst fixture
         * separately checks the unmodified production rate policy. */
        uint32_t f=irq_save();g_ticks+=1000;irq_restore(f);
        if (id) rec_emit("runner","DATA","case=concurrent n=%u payload=BBBBBBBBBBBBBBBB",i);
        else if (console_emitter) {
            char s[80]; int n=snprintf(s,sizeof(s),"emitter n=%u payload=AAAAAAAAAAAAAAAA",i);
            CHECK(device_write(s,(unsigned)n)==n);
        } else klog("[emitter] n=%u payload=AAAAAAAAAAAAAAAA",i);
        sched_yield();
    }
    playback_producer=false;
    atomic_fetch_add(&producers_done,1);
    return NULL;
}
static unsigned exact_size;
static void *large_producer(void *arg)
{
    unsigned id=*(unsigned *)arg;
    playback_producer=true;
    for (unsigned i=0;i<12;i++) {
        char record[SERIAL_RECORD_MAX];
        memset(record,'A'+id,exact_size);record[exact_size-1]='\n';
        memcpy(record,id ? "[console] " : "[kernel]  ",10);
        uint32_t f=irq_save();g_ticks+=1000;
        serial_write(record,exact_size);
        irq_restore(f);
        memset(record,'?',sizeof(record)); /* queue must own all source bytes */
        sched_yield();
    }
    playback_producer=false;atomic_fetch_add(&producers_done,1);return NULL;
}
static void maximum_records(void)
{
    for (unsigned kind=0;kind<2;kind++) for (unsigned size=0;size<2;size++) {
        reset_uart(kind);exact_size=size ? SERIAL_RECORD_MAX : 240;
        atomic_store(&producers_done,0);
        pthread_t a,b,c;unsigned ids[2]={0,1};
        CHECK(!pthread_create(&c,NULL,sender,NULL));
        CHECK(!pthread_create(&a,NULL,large_producer,&ids[0]));
        CHECK(!pthread_create(&b,NULL,large_producer,&ids[1]));
        CHECK(!pthread_join(a,NULL) && !pthread_join(b,NULL) && !pthread_join(c,NULL));
        emitted[emitted_count]=0;
        unsigned kept=0;
        for (const char *p=emitted;*p;) {
            const char *end=strstr(p,"\r\n");CHECK(end);
            if (strncmp(p,"[serial] dropped=",17)) {
                unsigned id=!memcmp(p,"[console] ",10);
                CHECK(!memcmp(p,id ? "[console] " : "[kernel]  ",10));
                CHECK((size_t)(end+2-p)==exact_size+1);
                for (unsigned i=10;i<exact_size-1;i++) CHECK(p[i]==(char)('A'+id));
                kept++;
            }
            p=end+2;
        }
        CHECK(kept==24 && !uart_dropped);
        /* Panic with a partially transmitted maximum-size task record and
         * an IF-clear producer behind it preserves exact byte order. */
        reset_uart(kind);char record[SERIAL_RECORD_MAX];memset(record,'P',sizeof(record));
        serial_write(record,sizeof(record));tick();
        uint32_t f=irq_save();serial_panic();serial_write("panic\n",6);irq_restore(f);
        CHECK(emitted_count==sizeof(record)+7);
        for (unsigned i=0;i<sizeof(record);i++) CHECK(emitted[i]=='P');
        CHECK(!memcmp(emitted+sizeof(record),"panic\r\n",7));
        reset_uart(kind);serial_write(record,SERIAL_RECORD_MAX+1u);
        CHECK(uart_dropped==1 && !uart_used); /* reject before inspecting input */
    }
    puts("serial bounds: two task/IRQ producers, 240/2048-byte records, source ownership, 16450/16550, panic order and oversize/rate whole drops PASS");
}
static void other_evidence_task(void)
{
    rec_emit("runner","DATA","case=quota task=other");
}
static void saturated_producers(void)
{
    for (unsigned producer=0;producer<4;producer++) {
        reset_uart(true);g_boot.flags=CBI_F_VERBOSE;
        rec_set_run("abcdef12");rec_emit("runner","BEGIN",NULL);drain_uart();
        emitted_count=0;g_ticks+=1000;
        char fill[1408];memset(fill,'Q',sizeof(fill));
        serial_write(fill,sizeof(fill));serial_write(fill,sizeof(fill));
        struct task task={0};g_current=&task;
        klog_periodic("[cpu] periodic must drop");
        rec_diag("runner","group=vm_heartbeat periodic=drop");
        CHECK(uart_dropped==2 && !evidence_sleeps);
        if (producer==0) klog("[supervisor] bootstrap: spawning /Programs/desktop");
        if (producer==1) klog("[supervisor] desktop pid=12 pgid=12");
        if (producer==2) rec_emit("runner","DATA","case=protected_evidence");
        if (producer==3) rec_diag("runner","case=protected_probe");
        CHECK(evidence_sleeps==1000 && uart_dropped==2 && flags==0x200);
        g_current=NULL;drain_uart();
        CHECK(!strstr(emitted,"periodic") && strstr(emitted,"[serial] dropped=2\r\n"));
        const char *expected[]={"[supervisor] bootstrap: spawning /Programs/desktop\r\n",
            "[supervisor] desktop pid=12 pgid=12\r\n","case=protected_evidence\r\n","case=protected_probe\r\n"};
        CHECK(strstr(emitted,expected[producer]));
    }
    /* Before scheduling starts, boot logs cannot wait for a quota window. */
    reset_uart(true);uart_admitted=uart_budget;
    klog("Ciuki VMM F0 build early - CiukiOS");
    CHECK(!uart_dropped && !evidence_sleeps);
    klog_periodic("[cpu] early periodic must drop");
    CHECK(uart_dropped==1);drain_uart();
    CHECK(strstr(emitted,"Ciuki VMM F0 build early - CiukiOS\r\n") && !strstr(emitted,"periodic"));
    puts("serial saturated producers: both desktop boot milestones, CIUKI_TEST evidence and probe DATA delivered after quota wait; only periodic records dropped/count=2; early boot protected PASS");
}
static void evidence_sequence(void)
{
    reset_uart(true);rec_set_run("abcdef12");rec_emit("runner","BEGIN",NULL);drain_uart();
    emitted_count=0;g_ticks+=1000;
    char fill[1407];memset(fill,'Q',sizeof(fill));fill[sizeof(fill)-1]='\n';
    serial_write(fill,sizeof(fill));serial_write(fill,sizeof(fill));
    CHECK(uart_admitted==uart_budget-UART_NOTICE_BYTES);
    uint32_t before=seq;
    struct task task={0};g_current=&task;
    evidence_wait_hook=other_evidence_task;
    rec_emit("runner","DATA","case=quota task=first");
    CHECK(evidence_sleeps==1000 && !uart_dropped && seq==before+2);
    g_current=NULL;drain_uart();
    char other[160],first_record[160];
    snprintf(other,sizeof(other),"CIUKI_TEST v=1 run=abcdef12 seq=%06u probe=runner event=DATA case=quota task=other\r\n",before+1);
    snprintf(first_record,sizeof(first_record),"CIUKI_TEST v=1 run=abcdef12 seq=%06u probe=runner event=DATA case=quota task=first\r\n",before+2);
    const char *a=strstr(emitted,other),*b=strstr(emitted,first_record);
    CHECK(a && b && a<b);
    puts("serial evidence: quota wait dispatches another producer before sequence allocation; wire sequence stays increasing with zero drops PASS");
}
static void blackbox_publication(void)
{
    reset_uart(true);verify_blackbox=true;capture_flags=0x200;captured_records=0;
    klog("[hwcheck] page record");
    CHECK(captured_records==1 && !strcmp(captured_record,"[hwcheck] page record\n"));
    klog_periodic("[kernel] periodic");
    rec_set_run("abcdef12");rec_emit("runner","BEGIN",NULL);
    g_boot.flags=CBI_F_VERBOSE;rec_diag("runner","case=ordinary");
    run_selected=false;g_boot.flags=CBI_F_DIAG_VERBOSE;
    rec_diag_disk("runner","case=disk_only");
    CHECK(captured_records==5 && strstr(captured_record,"case=disk_only\n"));
    /* Protected IRQ evidence survives saturated admission; the independent
     * black-box path still receives both it and a dropped periodic record. */
    uart_admitted=uart_budget;uint32_t f=irq_save();capture_flags=0;
    unsigned dropped=uart_dropped;
    rec_set_run("abcdef12");rec_emit("runner","BEGIN","case=irq_protected");
    CHECK(uart_dropped==dropped && captured_records==6 && strstr(captured_record,"case=irq_protected\n"));
    klog_periodic("[cpu] case=periodic_drop");
    CHECK(uart_dropped==dropped+1 && captured_records==7 && strstr(captured_record,"case=periodic_drop\n"));
    rec_emit_panicsafe("panic","PANIC","case=panic");
    CHECK(captured_records==8 && strstr(captured_record,"case=panic\n"));
    irq_restore(f);verify_blackbox=false;g_boot.flags=0;
    puts("output blackbox: page/periodic/evidence/diagnostic/disk-only/IRQ/panic records captured; task capture after IF restore, whole UART drop independent PASS");
}
int main(void)
{
    blackbox_publication();
    saturated_producers();
    evidence_sequence();
    maximum_records();
    for (unsigned kind=0;kind<2;kind++) for (unsigned envelopes=0;envelopes<2;envelopes++) {
        reset_uart(kind); console_emitter=envelopes;
        rec_set_run("12345678"); rec_emit("runner","BEGIN",NULL); drain_uart();
        emitted_count=0;
        atomic_store(&producers_done,0);
        pthread_t a,b,c; unsigned ids[2]={0,1};
        CHECK(!pthread_create(&c,NULL,sender,NULL));
        CHECK(!pthread_create(&a,NULL,record_emitter,&ids[0]));
        CHECK(!pthread_create(&b,NULL,record_emitter,&ids[1]));
        CHECK(!pthread_join(a,NULL) && !pthread_join(b,NULL) && !pthread_join(c,NULL));
        emitted[emitted_count]=0;
        CHECK(!uart_dropped);
        bool seen[2][100]={{false}}; unsigned rows=0;
        for (const char *line=emitted;*line;) {
            const char *end=strstr(line,"\r\n"); CHECK(end);
            if (!strncmp(line,"[serial] dropped=",17)) { line=end+2;continue; }
            unsigned id,n,sequence;
            char expected[256];
            if (!strncmp(line,"CIUKI_TEST ",11)) {
                CHECK(sscanf(line,"CIUKI_TEST v=1 run=12345678 seq=%u probe=runner event=DATA case=concurrent n=%u",&sequence,&n)==2);
                snprintf(expected,sizeof(expected),"CIUKI_TEST v=1 run=12345678 seq=%06u probe=runner event=DATA case=concurrent n=%u payload=BBBBBBBBBBBBBBBB\r\n",sequence,n);
                id=1;
            } else {
                const char *prefix=envelopes ? "[console] emitter" : "[emitter]";
                CHECK(!strncmp(line,prefix,strlen(prefix)));
                CHECK(sscanf(line+strlen(prefix)," n=%u",&n)==1);
                snprintf(expected,sizeof(expected),"%s n=%u payload=AAAAAAAAAAAAAAAA\r\n",prefix,n);
                id=0;
            }
            CHECK(n<100 && !seen[id][n]); seen[id][n]=true;
            CHECK((size_t)(end+2-line)==strlen(expected) && !memcmp(line,expected,strlen(expected)));
            rows++; line=end+2;
        }
        CHECK(rows==200);
    }
    reset_uart(true);
    char endings[1024]; memset(endings,'\n',sizeof(endings));
    unsigned consumed=0;
    while (consumed<sizeof(endings)) {
        int n=device_write(endings+consumed,sizeof(endings)-consumed);
        CHECK(n>0 && (unsigned)n<=sizeof(endings)-consumed);
        consumed+=(unsigned)n;
        drain_uart();
        g_ticks+=1000;
    }
    drain_uart(); CHECK(!uart_dropped);
    puts("serial records: concurrent klog/rec_emit and console/rec_emit, 16450/16550, 200 whole records per case, zero drops/interleaved bytes with paced admission PASS");
    puts("serial envelopes: 1024 line endings use bounded short writes with complete prefixes/terminators PASS");
    return 0;
}
