/* Actual H-page serial check, production queue/kwait and fake UART/PIC. */
#define main serial_fixture_main
#include "serial_audio_test.c"
#undef main
#include <ciuki/hardware_check.h>
static unsigned page_sleeps, captures;
void console_write(const char *s,size_t n) { CHECK(s && n); }
void bootlog_capture(const char *s,size_t n) { CHECK(s && n);captures++; }
static void (*blackbox_capture)(const char *,size_t);
static uint64_t check_now(void *arg) { (void)arg;return g_ticks; }
void task_sleep_ms(uint32_t ms) { CHECK(ms==1);page_sleeps++;tick(); }
#include "serial_page.inc"
int main(void)
{
    for (unsigned kind=0;kind<2;kind++) {
        reset_uart(kind);timed=true;page_sleeps=captures=0;
        struct hw_check_result result;serial_check(30000,&result);
        CHECK(result.status==HW_PASS && captures==100 && page_sleeps);
        CHECK(strstr(result.detail,"requested=100 accepted=100 whole_sent=100 resent=0 dropped=0 periodic=0 evidence=0 queued=0"));
        /* Ticket completion is FIFO submission; finish the physical tail. */
        drain_uart();unsigned rows=0;
        for (const char *p=emitted;*p;) {
            const char *end=strstr(p,"\r\n");CHECK(end);rows++;
            char expected[64];int n=snprintf(expected,sizeof(expected),"[hwcheck] serial record=%03u/100\r\n",rows);
            CHECK(end+2-p==n && !memcmp(p,expected,(size_t)n));p=end+2;
        }
        CHECK(rows==100);
        printf("serial H-page %s: %s, exact wire records=100 PASS\n",kind ? "16550":"16450",result.detail);
    }
    reset_uart(true); g_boot.uart_state=CBI_UART_NO_BDA;
    present=false; initializing=true; serial_init(model_base,3); initializing=false;
    struct hw_check_result absent; page_sleeps=captures=0;
    serial_check(30000,&absent);
    CHECK(absent.status==HW_SKIP && !page_sleeps && !captures);
    CHECK(!strcmp(absent.detail,"UART absent (loader step 2, kernel probe 3)"));
    reset_uart(true); g_boot.uart_state=CBI_UART_THRE_TIMEOUT; ready=false;
    initializing=true; serial_init(model_base,3); initializing=false;
    serial_check(30000,&absent);
    CHECK(absent.status==HW_SKIP);
    CHECK(!strcmp(absent.detail,"UART absent (loader step 6, kernel probe 6)"));
    return 0;
}
