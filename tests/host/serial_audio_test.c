/* F4-13: production serial output with a busy 16550/16450 register model.
 * A log producer must not wait for UART readiness during playback. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
static uint32_t flags=0x200;
static uint32_t irq_save(void) { uint32_t f=flags; flags=0; return f; }
static void irq_restore(uint32_t f) { flags=f; }
static uint8_t regs[8];
static bool present=true, ready, fifo;
static unsigned lsr_reads, filled;
static char emitted[40000];
static unsigned emitted_count;
static uint8_t inb(uint16_t p)
{
    CHECK(p>=0x3F8 && p<0x400);
    if (!present) return 0xFF;
    unsigned off=p-0x3F8;
    if(off==2) return fifo?0xC0:0;
    if(off==5) { lsr_reads++; if(ready) filled=0; return ready?0x20:0; }
    return regs[off];
}
static void outb(uint16_t p, uint8_t value)
{
    CHECK(p>=0x3F8 && p<0x400);
    unsigned off=p-0x3F8;
    if(off==0 && !(regs[3]&0x80)) {
        CHECK(ready && emitted_count<sizeof(emitted)-1);
        CHECK(++filled<=(fifo?16u:1u));
        emitted[emitted_count++]=(char)value;
        /* The synchronous writer sees THRE again at its next poll. A timer
         * burst fills the FIFO without polling between bytes. */
        if(flags&0x200) filled=0;
    }
    regs[off]=value;
}
#include "../../src/kernel/core/serial.c"
static void tick(void)
{
    flags=0; filled=0; serial_tick(); flags=0x200;
}
static void drain_uart(void)
{
    ready=true;
    for(unsigned i=0;i<10000 && (uart_used || uart_drop_notice);i++) tick();
    CHECK(!uart_used && !uart_drop_notice);
    emitted[emitted_count]=0;
}
int main(void)
{
    for(unsigned kind=0;kind<2;kind++) {
        memset(regs,0,sizeof(regs)); fifo=kind; present=true; ready=true;
        emitted_count=filled=lsr_reads=0; serial_init(0x3F8,3);
        CHECK(serial_present() && uart_fifo_bytes==(kind?16u:1u));
        serial_write("boot\n",5); CHECK(!memcmp(emitted,"boot\r\n",6));
        serial_buffered(true); ready=false; unsigned before=lsr_reads;
        serial_write("[vm-audio] reset=1\n",19);
        serial_write("[usb] verbose\n",14);
        CHECK(lsr_reads==before && flags==0x200 && uart_used==35);
        tick(); CHECK(lsr_reads==before+1 && emitted_count==6 && uart_used==35);
        ready=true; tick(); CHECK(filled==(kind?16u:1u));
        serial_buffered(false); /* older queued records retain order */
        serial_write("tail\n",5); drain_uart();
        CHECK(!strcmp(emitted,"boot\r\n[vm-audio] reset=1\r\n[usb] verbose\r\ntail\r\n"));
        /* Complete-record overflow is explicit, with no torn dropped line. */
        serial_buffered(true); ready=false;
        static char full[UART_LOG_BYTES]; memset(full,'X',sizeof(full));
        full[sizeof(full)-2]='\n';
        unsigned start=emitted_count;
        serial_write(full,sizeof(full)-1); CHECK(uart_used==UART_LOG_BYTES);
        serial_write("DROP\n",5); CHECK(uart_dropped==1 && uart_used==UART_LOG_BYTES);
        drain_uart(); CHECK(emitted_count>start+UART_LOG_BYTES);
        CHECK(!strstr(emitted+start,"DROP") && strstr(emitted+start,"[serial] result=log_dropped\r\n"));
        /* Physical IF clear: panic output drains without the timer. */
        serial_write("pending\n",8); CHECK(uart_used==9);
        flags=0; ready=true;
        /* Synchronous panic writes need each THRE poll to model an empty UART. */
        uart_fifo_bytes=1; fifo=true;
        serial_write("panic\n",6); flags=0x200;
        CHECK(!uart_used);
    }
    present=false; serial_init(0x3F8,3); CHECK(!serial_present());
    unsigned before=lsr_reads; serial_buffered(true); serial_write("absent\n",7); tick();
    CHECK(lsr_reads==before && !uart_used);
    puts("serial audio: busy producer never polls, 16550/16450 bursts, CRLF/order/wrap/drop, panic/absence PASS");
    return 0;
}
