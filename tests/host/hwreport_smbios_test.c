/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CIUKI_CPU_H
#define KERNEL_VBASE 0xc0000000u
uint8_t inb(uint16_t p) { (void)p; assert(!"unexpected device I/O"); return 0; }
void outb(uint16_t p,uint8_t v) { (void)p; (void)v; assert(!"unexpected device I/O"); }
volatile uint64_t g_ticks;
void task_sleep_ms(uint32_t ms) { (void)ms; assert(!"unexpected scheduler wait"); }
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
#include "../../src/kernel/drivers/thinkpad_ec.c"
int main(void)
{
    unsigned char table[256]={0,9,0,0,2,1,0,0,3};
    const char bios[]="1.20\0IBM\0" "10/22/1999\0";
    size_t n=9; memcpy(table+n,bios,sizeof(bios)); n+=sizeof(bios);
    const unsigned char system[]={1,8,1,0,1,2,3,4};
    memcpy(table+n,system,sizeof(system)); n+=sizeof(system);
    const char identity[]="IBM\0" "2647\0" "ThinkPad\0" "private serial\0";
    memcpy(table+n,identity,sizeof(identity)); n+=sizeof(identity);
    const unsigned char end[]={127,4,2,0,0,0}; memcpy(table+n,end,sizeof(end)); n+=sizeof(end);
    struct thinkpad_ec_smbios out;
    assert(thinkpad_ec_smbios_decode(table,n,&out) && out.valid && out.has_bios && out.has_system);
    assert(!strcmp(out.bios_date,"10/22/1999") && !strcmp(out.bios_version,"1.20") && !strcmp(out.vendor,"IBM"));
    retained_identity=out; memset(&out,0,sizeof(out)); thinkpad_ec_smbios_snapshot(&out);
    assert(!strcmp(out.product,"2647") && !strcmp(out.bios_date,"10/22/1999"));
    table[8]=99; assert(thinkpad_ec_smbios_decode(table,n,&out) && !out.bios_date[0]); table[8]=3;
    table[9+5]='\n'; assert(thinkpad_ec_smbios_decode(table,n,&out) && strchr(out.bios_vendor,'?'));
    for (size_t i=0;i<9+sizeof(bios);i++) assert(!thinkpad_ec_smbios_decode(table,i,&out));
    table[1]=255; assert(!thinkpad_ec_smbios_decode(table,n,&out));
    puts("PASS SMBIOS report identity: BIOS date/index/missing/malformed/string bounds, retained copy, no serial field");
}
