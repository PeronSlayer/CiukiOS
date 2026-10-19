/* Actual protected formatter: the whole long record reaches both sinks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
static uint32_t irq_save(void){return 0x200;}
static void irq_restore(uint32_t n){(void)n;}
static void cli(void){}
static void hlt(void){abort();}
static uint32_t read_cr2(void){return 0;}
static char serial[2048],disk[2048];
void serial_write(const char *s,size_t n){if(n>=sizeof(serial))abort();memcpy(serial,s,n);serial[n]=0;}
void console_write(const char *s,size_t n){(void)s;(void)n;}
void bootlog_capture(const char *s,size_t n){if(n>=sizeof(disk))abort();memcpy(disk,s,n);disk[n]=0;}
#include "../../../../src/kernel/core/output.c"
int main(void){char shims[1100];memset(shims,'a',sizeof(shims)-1);shims[sizeof(shims)-1]=0;
    w32_log("[win32] start pid=1 exe=fixture.exe pe=0:1000 shims=%s",shims);
    if(strlen(serial)<1100||strcmp(serial,disk)||serial[strlen(serial)-1]!='\n')return 1;
    puts("PASS complete bounded Win32 record to serial and boot log");return 0;
}
