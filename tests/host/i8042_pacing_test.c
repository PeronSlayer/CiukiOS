/* Production physical port read; substitute only ports and TSC sampling. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"pacing:%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#define DATA_PORT 0x60u
uint64_t g_tsc_per_ms;
static uint64_t clock_value, clock_step;
static unsigned samples, reads, records;
static uint32_t flags;
static uint64_t ktime_cycles(void) { samples++;clock_value+=clock_step;return clock_value; }
static uint8_t inb(uint16_t port) { CHECK(port==0x60 || port==0x64);reads++;return 0x42; }
static void klog(const char *fmt,...)
{
    CHECK(strstr(fmt,"[i8042] pacing reason=clock_bound") && strstr(fmt,"bound_us=100"));records++;
}
#include "latency_i8042_pacing.inc"
int main(void)
{
    g_tsc_per_ms=1000;clock_step=1;
    CHECK(physical_read(NULL,0x64)==0x42 && !samples && !records && !flags);
    CHECK(physical_read(NULL,DATA_PORT)==0x42 && clock_value==8 && samples==8 && !records && !flags);
    flags=0x200;samples=0;CHECK(physical_read(NULL,DATA_PORT)==0x42 && samples==8 && flags==0x200);
    /* A late sample is reported; sampling then stops without another wait. */
    flags=0;clock_step=101;samples=0;
    CHECK(physical_read(NULL,DATA_PORT)==0x42 && samples==2 && records==1 && !flags);
    clock_step=0;samples=0;
    CHECK(physical_read(NULL,DATA_PORT)==0x42 && samples==257 && records==1 && !flags);
    g_tsc_per_ms=0;samples=0;
    CHECK(physical_read(NULL,DATA_PORT)==0x42 && samples==1 && records==1);
    CHECK(reads==6);
    puts("i8042 pacing: status has zero waits, data waits 7us with IF preserved, late/stalled/absent clocks bounded and one 100us-bound record PASS");
    return 0;
}
