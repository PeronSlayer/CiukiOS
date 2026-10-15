/* Real formatted records and user envelopes share the production UART queue.
 * Reuse the IRQ/FIFO model, including its concurrent burst sender. */
#define FS_HOST
#define main serial_fixture_main
#include "serial_audio_test.c"
#undef main
static void cli(void) { flags=0; }
static void hlt(void) { abort(); }
static uint32_t read_cr2(void) { return 0; }
struct ciuki_boot_info g_boot;
struct task *g_current;
void console_write(const char *s, size_t n) { (void)s; (void)n; }
void bootlog_capture(const char *s, size_t n) { (void)s; (void)n; }
void bootlog_capture_ring(const char *s, size_t n) { (void)s; (void)n; }
#include "../../src/kernel/core/output.c"
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
int main(void)
{
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
    }
    drain_uart(); CHECK(!uart_dropped);
    puts("serial records: concurrent klog/rec_emit and console/rec_emit, 16450/16550, 200 whole records per case, zero drops/interleaved bytes PASS");
    puts("serial envelopes: 1024 line endings use bounded short writes with complete prefixes/terminators PASS");
    return 0;
}
