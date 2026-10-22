/* Actual Linux i386 CPL3 instructions, not Ciuki runtime qualification.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#include "src/kernel/ndis/userhost.h"
static struct ndis_user_host h;
static struct ntimage image;
static volatile sig_atomic_t emulating,steps,fenced,retained,cancelled;
static bool own(void *c,uint32_t g) { (void)c;return g==77; }
static bool mapping(void *c,const struct ndis_host_map *m) { (void)c;(void)m;return true; }
static int map(void *c,const struct ndis_host_map *m,bool i) { (void)c;(void)m;(void)i;return 0; }
static bool mask(void *c,bool i) { (void)c;(void)i;return true; }
static int cause(void *c) { (void)c;return 0; }
static void noop(void *c) { (void)c; }
static bool fence(void *c) { (void)c;fenced=1;return false; }
static void detach(void *c,uint32_t g) { (void)c;(void)g; }
static void retain(void *c) { (void)c;retained=fenced; }
static void cancel(void *c) { (void)c;cancelled=retained; }
static void record(void *c,const char *s,uint32_t v,uint32_t o) { (void)c;(void)s;(void)v;(void)o; }
static void handler(int signal,siginfo_t *info,void *context)
{
    (void)info;
    ucontext_t *uc=context;
    if(emulating && signal==SIGSEGV) {
        struct trap_frame tf={.vector=(uint32_t)uc->uc_mcontext.gregs[REG_TRAPNO],
            .err=(uint32_t)uc->uc_mcontext.gregs[REG_ERR],
            .eip=(uint32_t)uc->uc_mcontext.gregs[REG_EIP],
            .cs=(uint32_t)uc->uc_mcontext.gregs[REG_CS],
            .eflags=(uint32_t)uc->uc_mcontext.gregs[REG_EFL]};
        uint8_t op=*(const uint8_t *)(uintptr_t)tf.eip;
        if(!ndis_host_gp(&h,&tf,op) || !(tf.eflags&0x200)) _exit(91);
        steps++;uc->uc_mcontext.gregs[REG_EIP]=(greg_t)tf.eip;
        uc->uc_mcontext.gregs[REG_EFL]=(greg_t)tf.eflags;return;
    }
    if(signal==SIGALRM) ndis_host_watchdog(&h,10000000);
    else ndis_host_stop(&h,"actual-user-fault",(uint32_t)uc->uc_mcontext.gregs[REG_TRAPNO],
                        (uint32_t)uc->uc_mcontext.gregs[REG_EIP],0);
    _exit(h.state==NDIS_HOST_QUARANTINED && fenced && retained && cancelled?0:92);
}
static void setup(void)
{
    image=(struct ntimage){.loaded=true,.platform={.user_host=true},
        .pe={.base=0x10000,.size=0x10000000,.section_count=1,
            .sections={{.rva=0,.span=0x10000000,.flags=0x60000020}}}};
    h=(struct ndis_user_host){0};fenced=retained=cancelled=steps=0;
    struct ndis_host_ops o={0,own,mapping,map,mask,cause,noop,fence,detach,retain,cancel,record};
    assert(ndis_host_init(&h,&image,77,&o,false,false,5));
    struct sigaction sa={.sa_sigaction=handler,.sa_flags=SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    assert(!sigaction(SIGSEGV,&sa,0) && !sigaction(SIGILL,&sa,0) &&
           !sigaction(SIGFPE,&sa,0) && !sigaction(SIGALRM,&sa,0));
}
__attribute__((noinline)) static void shim_fault(unsigned kind)
{
    if(kind==0) *(volatile uint32_t *)(uintptr_t)1=0;
    else if(kind==1) __asm__ volatile("xor %%edx,%%edx; xor %%ecx,%%ecx; div %%ecx" :::"eax","edx","ecx","cc");
    else if(kind==2) __asm__ volatile("ud2");
    else __asm__ volatile("outb %%al,$0x80" ::: "memory");
}
int main(void)
{
    setup();emulating=1;
    uint32_t physical;
    __asm__ volatile("cli; pushfl; popl %0":"=r"(physical)::"memory");
    assert(!h.virtual_if && (physical&0x200)); /* PUSHFD reveals physical IF */
    uint32_t saved=ndis_host_flags(&h,physical);assert(!(saved&0x200));
    __asm__ volatile("pushl %0; popfl"::"r"(saved):"cc","memory");
    __asm__ volatile("pushfl; popl %0":"=r"(physical)::"memory");
    assert(physical&0x200); /* POPFD silently preserves IF at CPL3/IOPL0 */
    __asm__ volatile("sti; hlt":::"memory");assert(h.virtual_if && steps==3);
    assert(ndis_host_restore_flags(&h,saved) && !h.virtual_if);
    assert(ndis_host_restore_flags(&h,0x202) && h.virtual_if);
    emulating=0;
    for(unsigned kind=0;kind<8;kind++) {
        pid_t pid=fork();assert(pid>=0);
        if(!pid) {
            setup();emulating=0;
            if(kind<4) shim_fault(kind);
            else {
                uint32_t token;struct ndis_host_event e;
                assert(ndis_host_enqueue(&h,(enum ndis_host_kind)(kind-4),0x101000,0,0,&token));
                assert(ndis_host_take(&h,0,&e));
                struct itimerval timer={.it_value={.tv_usec=10000}};
                assert(!setitimer(ITIMER_REAL,&timer,0));
                if(kind==4) {
                    emulating=1;__asm__ volatile("cli":::"memory");
                    assert(!h.virtual_if); /* physical timer still reaches us */
                }
                for(;;) __asm__ volatile("pause");
            }
            _exit(93);
        }
        int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    puts("PASS i386 CPL3 CLI/STI/HLT traps, physical PUSHFD/POPFD, forbidden OUT, PF/DE/UD, nonreturn deadlines");
    return 0;
}
