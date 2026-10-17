/* Production initialization/recovery with fake banked UART ports. */
#define SERIAL_WITH_OUTPUT
#define main serial_fixture_main
#include "serial_audio_test.c"
#undef main
static char probe_logs[512];
static unsigned probe_log_used;
void klog(const char *fmt, ...)
{
    CHECK(initializing && probe_log_used < sizeof(probe_logs));
    va_list ap; va_start(ap,fmt);
    int n=vsnprintf(probe_logs+probe_log_used,sizeof(probe_logs)-probe_log_used,fmt,ap);
    va_end(ap);
    CHECK(n>0 && (unsigned)n+1<sizeof(probe_logs)-probe_log_used);
    probe_log_used+=(unsigned)n;
    probe_logs[probe_log_used++]='\n'; probe_logs[probe_log_used]=0;
}
__attribute__((noreturn)) void panic(const char *fmt, ...)
{ fprintf(stderr,"unexpected panic: %s\n",fmt); abort(); }
static void boot_probe(unsigned state, enum serial_probe_result expected)
{
    g_boot.uart_state=state;
    g_boot.uart_base=state==CBI_UART_OK ? 0x3F8 : 0;
    g_boot.uart_divisor=state==CBI_UART_OK ? 3 : 0;
    g_boot.flags=CBI_F_HW_CHECK | CBI_F_SERIAL_LOG;
    probe_log_used=0;
    /* DLAB inherited set: clearing IER must not overwrite DLM. */
    regs[3]=0x80; regs[7]=0x37;
    io_count=lsr_reads=rx_reads=0;
    initializing=true; serial_boot_init(); initializing=false;
    struct serial_stats s; serial_snapshot(&s);
    CHECK(s.probe_result==expected && s.present==(expected==SERIAL_PROBE_OK));
    CHECK(g_boot.uart_state==state && (g_boot.flags&CBI_F_HW_CHECK));
    char expected_log[100];
    snprintf(expected_log,sizeof(expected_log),"[serial] loader_uart state=%u base=%04x divisor=%u\n",
             state,state==CBI_UART_OK ? 0x3F8 : 0,state==CBI_UART_OK ? 3 : 0);
    CHECK(strstr(probe_logs,expected_log));
    snprintf(expected_log,sizeof(expected_log),"[serial] kernel_probe result=%u base=%04x divisor=%u\n",
             expected,s.present ? 0x3F8 : 0,s.present ? 3 : 0);
    CHECK(strstr(probe_logs,expected_log));
    CHECK(!rx_reads && !tx_writes);
    CHECK(g_boot.uart_base==(s.present ? 0x3F8u : 0));
    CHECK(g_boot.uart_divisor==(s.present ? 3u : 0));
    CHECK(!!(g_boot.flags&CBI_F_SERIAL_LOG)==s.present);
    if (present) CHECK(regs[7]==0x37); /* reversible SCR probe on every path */
    if (expected==SERIAL_PROBE_THRE_TIMEOUT) CHECK(lsr_reads==1+UART_INIT_POLL_LIMIT);
    if (!s.present) {
        unsigned before=io_count;
        serial_writer_init(); serial_write("absent\n",7); serial_tick();
        CHECK(io_count==before && !uart_used && !uart_irq_started);
    } else {
        CHECK(regs[3]==3 && divisor_bytes[0]==3 && !divisor_bytes[1] && !regs[1] && regs[4]==0x0B);
        initializing=true; serial_writer_init(); initializing=false;
        serial_write("recovered\n",10); drain_uart();
        CHECK(!strcmp(emitted,"recovered\r\n"));
    }
}
int main(void)
{
    for (unsigned fifo_kind=0;fifo_kind<2;fifo_kind++)
        for (unsigned state=CBI_UART_UNKNOWN;state<=CBI_UART_BUDGET;state++) {
            reset_uart(fifo_kind); boot_probe(state,SERIAL_PROBE_OK);
        }
    for (unsigned fault=0;fault<6;fault++) {
        reset_uart(true);
        enum serial_probe_result expected;
        switch (fault) {
        case 0: present=false; expected=SERIAL_PROBE_SCRATCH; break;
        case 1: scratch_fault=1; expected=SERIAL_PROBE_SCRATCH; break;
        case 2: scratch_fault=2; expected=SERIAL_PROBE_SCRATCH; break;
        case 3: lsr_float=true; expected=SERIAL_PROBE_LSR; break;
        case 4: divisor_fault=1; expected=SERIAL_PROBE_DIVISOR; break;
        default: divisor_fault=2; expected=SERIAL_PROBE_DIVISOR; break;
        }
        boot_probe(CBI_UART_NO_BDA,expected);
        if (expected==SERIAL_PROBE_DIVISOR) CHECK(regs[3]==3 && !regs[1]);
    }
    reset_uart(true); ready=false;
    boot_probe(CBI_UART_THRE_TIMEOUT,SERIAL_PROBE_THRE_TIMEOUT);
    reset_uart(true); present=false;
    boot_probe(CBI_UART_OK,SERIAL_PROBE_SCRATCH);
    reset_uart(true); initializing=true; serial_init(0x1234,3); initializing=false;
    struct serial_stats mapping; serial_snapshot(&mapping);
    CHECK(!mapping.present && mapping.probe_result==SERIAL_PROBE_MAPPING);
    /* A loader OK port retains an explicit serial=0 policy. */
    reset_uart(true); g_boot.uart_state=CBI_UART_OK;
    g_boot.uart_base=model_base; g_boot.uart_divisor=3; g_boot.flags=0;
    probe_log_used=0; initializing=true; serial_boot_init(); initializing=false;
    CHECK(serial_present() && !g_boot.flags);
    puts("serial probe: loader states 0..7, 16450/16550, absent/SCR/LSR/DLL/DLM/THRE faults, recovery logs/resources, bounded timeout PASS");
    return 0;
}
