/* Synthetic executable bytes only. SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/kernel/ndis/userhost.h"
static uint8_t memory[16384];
static struct ndis_flag_table table;
static struct ndis_flag_workspace workspace;
static bool owned(void *c,uint32_t g) { (void)c;return g==7; }
static void notify(void *c) { (void)c; }
static int read_mem(void *c,uint32_t at,void *out,uint32_t n)
{ (void)c;if(at<0x100000 || at-0x100000>sizeof(memory) || n>sizeof(memory)-(at-0x100000)) return 0;
  memcpy(out,memory+(at-0x100000),n);return 1; }
static int write_mem(void *c,uint32_t at,const void *in,uint32_t n)
{ (void)c;if(at<0x102000 || at-0x100000>sizeof(memory) || n>sizeof(memory)-(at-0x100000)) return 0;
  memcpy(memory+(at-0x100000),in,n);return 1; }
static const struct ndis_flag_access access={0,read_mem,write_mem};
static struct ntimage image;
static struct ndis_user_host host;
static void setup(void)
{
    memset(memory,0x90,sizeof(memory));memset(&table,0,sizeof(table));
    image=(struct ntimage){.loaded=true,.hash={42},.platform={.user_host=true,.flag_table=&table},
        .pe={.image=memory,.base=0x100000,.size=sizeof(memory),.section_count=2,
        .sections={{.rva=0,.span=128,.flags=0x60000000},{.rva=4096,.span=128,.flags=0xc0000000}}}};
    host=(struct ndis_user_host){.image=&image,.generation=7,.state=NDIS_HOST_LIVE,.virtual_if=true,
        .ops={.owned=owned,.notify=notify}};
}
static void patch(void) { assert(ndis_flags_patch(&image.pe,&table,&workspace,image.hash)); }
static void bp(struct trap_frame *tf,unsigned site)
{ tf->vector=3;tf->eip=0x100001+site;assert(ndis_flags_breakpoint(&host,tf,&access));assert(tf->eflags&0x200); }
int main(void)
{
    setup();memory[0]=0x9c;memory[1]=0xfa;memory[2]=0x9c;memory[3]=0x9d;memory[4]=0x9d;
    memory[16]=0x9c; /* a separately called function saves the current virtual IF */
    memory[32]=0xcf;memory[40]=0x66;memory[41]=0x9c;memory[42]=0x66;memory[43]=0x9d;
    memory[50]=0xb8;memory[51]=0x9c;memory[52]=0x9d;memory[53]=0xcf;memory[54]=0;
    memory[4096]=0x9c;memory[4097]=0x9d;
    patch();assert(table.count==8 && memory[51]==0x9c && memory[52]==0x9d && memory[53]==0xcf && memory[4096]==0x9c);
    struct trap_frame tf={.cs=0x1b,.eflags=0x246,.user_esp=0x104000};
    bp(&tf,0);assert(pe_u32(memory+0x3ffc)==0x246);
    tf.vector=13;tf.eip=0x100001;assert(ndis_host_gp(&host,&tf,0xfa));
    bp(&tf,16);assert(!(pe_u32(memory+0x3ff8)&0x200)); /* callee */
    bp(&tf,3);assert(!host.virtual_if);bp(&tf,4);assert(host.virtual_if && tf.user_esp==0x104000);
    bp(&tf,40+1);assert(tf.user_esp==0x103ffe);bp(&tf,42+1);assert(tf.user_esp==0x104000);
    tf.user_esp-=12;pe_put32(memory+0x3ff4,0x100010);pe_put32(memory+0x3ff8,tf.cs);pe_put32(memory+0x3ffc,0x42);
    bp(&tf,32);assert(tf.eip==0x100010 && !host.virtual_if && tf.user_esp==0x104000);
    tf.vector=3;tf.eip=0x100061;assert(!ndis_flags_breakpoint(&host,&tf,&access)); /* unrecorded INT3 */
    memory[16]=0x9c;tf.eip=0x100011;assert(!ndis_flags_breakpoint(&host,&tf,&access));memory[16]=0xcc;
    table.image_hash[0]++;assert(!ndis_flags_breakpoint(&host,&tf,&access));table.image_hash[0]--;
    tf.user_esp=0x100004;assert(!ndis_flags_breakpoint(&host,&tf,&access));
    tf.user_esp=0x103ff4;pe_put32(memory+0x3ff8,8);tf.eip=0x100021;
    assert(!ndis_flags_breakpoint(&host,&tf,&access)); /* privilege-changing IRET */
    setup();image.pe.dirs[5]=(struct pe_directory){8192,12};pe_put32(memory+8192,0);pe_put32(memory+8196,12);
    pe_put16(memory+8200,0x3020);pe_put16(memory+8202,0);memory[32]=0x9c;memory[33]=0x9d;memory[34]=0xcf;memory[35]=0;
    memory[36]=0x9c;patch();assert(table.count==1 && memory[32]==0x9c && memory[36]==0xcc);
    setup();memory[0]=0xf3;memory[1]=0x9c;assert(!ndis_flags_patch(&image.pe,&table,&workspace,image.hash));
    setup();memory[0]=0x9c;image.pe.sections[0].flags|=0x80000000;
    assert(!ndis_flags_patch(&image.pe,&table,&workspace,image.hash) && memory[0]==0x9c);
    puts("PASS binary flag sites, nesting, callee, operand16, IRET, operands and tamper");fflush(stdout);return 0;
}
