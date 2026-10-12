/* CPU-wrapper evidence, independent of DOS media; no QEMU/hardware claim. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dosvm_guest.h"
static uint8_t memory[0x10000];
static bool big;
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
static bool read_memory(uint32_t at, void *p, unsigned n)
{ if (at > sizeof(memory) || n > sizeof(memory)-at) return false; memcpy(p,memory+at,n); return true; }
static bool write_memory(uint32_t at, const void *p, unsigned n)
{ if (at > sizeof(memory) || n > sizeof(memory)-at) return false; memcpy(memory+at,p,n); return true; }
static void descriptor(uint16_t sel, bool write, uint32_t d[2])
{
    (void)write; unsigned i=sel>>3; uint32_t base=i==16?0x1000:i==17?0x2000:0;
    d[0]=0xFFFFu | (base<<16); d[1]=(i==16?0xFA00u:0xF200u) | (big?0x400000u:0);
}
static void unexpected(const char *what) { fprintf(stderr,"CPU unexpected: %s\n",what); exit(1); }
static void word(unsigned at, uint32_t v, unsigned width)
{ for(unsigned i=0;i<width;i++) memory[at+i]=(uint8_t)(v>>(8*i)); }
int main(void)
{
    const struct gcpu_ops ops={read_memory,write_memory,descriptor,unexpected,0}; gcpu_init(&ops);
    for(unsigned bits=0;bits<2;bits++) {
        big=bits!=0; unsigned w=big?4:2;
        for(unsigned form=0;form<5;form++) {
            memset(memory,0,sizeof(memory));
            struct gcpu_state s={.cs=0x87,.ss=0x8F,.ds=0x97,.es=0x97,.fs=0x97,.gs=0x97,
                .esp=0x100,.eflags=0x202,.pm=true,.ldt_limit=159};
            memory[0x1000]=0x9D; /* POPF loads armed TF */
            if(form==0) { memory[0x1001]=0x9D; word(0x2100+w,0x202,w); }
            if(form==1) {
                memory[0x1001]=0xCF; /* IRET clears TF in its saved image */
                word(0x2100+w,0x20,w); word(0x2100+2*w,0x87,w); word(0x2100+3*w,0x202,w);
            }
            if(form==2 || form==4) {
                memory[0x1001]=0x8E; memory[0x1002]=0xD0; memory[0x1003]=form==4?0x9D:0x90; s.eax=0x8F;
                if(form==4) word(0x2100+w,0x202,w);
            }
            if(form==3) { memory[0x1001]=0x17; memory[0x1002]=0x90; word(0x2100+w,0x8F,w); }
            word(0x2100,0x302,w);
            struct gcpu_event ev;
            int rc=gcpu_run(&s,20,&ev);
            if(rc!=GCPU_TRAP || ev.vector!=1 || ev.dr6!=0x4000 || ev.fault)
                fprintf(stderr,"bits=%u form=%u rc=%d vector=%u dr6=%x fault=%u executed=%u eip=%x flags=%x\n",bits,form,rc,ev.vector,ev.dr6,ev.fault,ev.executed,s.eip,s.eflags);
            CHECK(rc==GCPU_TRAP && ev.vector==1 && ev.dr6==0x4000 && !ev.fault);
            CHECK(ev.executed==(form>=2?3u:2u));
            CHECK(s.eip==(form==1?0x20u:form==3?3u:form>=2?4u:2u));
            CHECK((bool)(s.eflags&0x100)==(form==2 || form==3));
        }
    }
    gcpu_done(); printf("guest CPU single step: 16/32-bit POPF/IRET TF clear, MOV/POP SS delay and delayed TF clear PASS (%u checks)\n",checks); return 0;
}
