/* Actual Linux i386 CPL3 instructions, not Ciuki runtime qualification.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#include "src/kernel/ndis/userhost.h"
static struct ndis_user_host h;
static struct ntimage image;
static struct ndis_flag_table flag_table;
static struct ndis_flag_workspace flag_workspace;
static uint8_t *flag_code,*flag_stack;
static int flag_read(void *ctx,uint32_t address,void *out,uint32_t bytes)
{
    (void)ctx;
    if((address>=(uintptr_t)flag_code && address-(uintptr_t)flag_code<4096 && bytes<=4096-(address-(uintptr_t)flag_code)) ||
       (address>=(uintptr_t)flag_stack && address-(uintptr_t)flag_stack<16384 && bytes<=16384-(address-(uintptr_t)flag_stack))) {
        memcpy(out,(const void *)(uintptr_t)address,bytes);return 1;
    }
    return 0;
}
static int flag_write(void *ctx,uint32_t address,const void *in,uint32_t bytes)
{
    (void)ctx;
    if(address<(uintptr_t)flag_stack || address-(uintptr_t)flag_stack>=16384 || bytes>16384-(address-(uintptr_t)flag_stack)) return 0;
    memcpy((void *)(uintptr_t)address,in,bytes);return 1;
}
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
    if(emulating && (signal==SIGSEGV || signal==SIGTRAP)) {
        struct trap_frame tf={.vector=(uint32_t)uc->uc_mcontext.gregs[REG_TRAPNO],
            .err=(uint32_t)uc->uc_mcontext.gregs[REG_ERR],
            .eip=(uint32_t)uc->uc_mcontext.gregs[REG_EIP],
            .cs=(uint32_t)uc->uc_mcontext.gregs[REG_CS],
            .eflags=(uint32_t)uc->uc_mcontext.gregs[REG_EFL],
            .user_esp=(uint32_t)uc->uc_mcontext.gregs[REG_UESP]};
        int result;
        if(signal==SIGTRAP) {
            struct ndis_flag_access access={0,flag_read,flag_write};
            result=tf.vector==1?ndis_host_debug(&h,&tf):ndis_flags_breakpoint(&h,&tf,&access);
        } else result=ndis_host_gp(&h,&tf,*(const uint8_t *)(uintptr_t)tf.eip);
        if(!result || !(tf.eflags&0x200)) _exit(91);
        uc->uc_mcontext.gregs[REG_UESP]=(greg_t)tf.user_esp;
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
           !sigaction(SIGFPE,&sa,0) && !sigaction(SIGALRM,&sa,0) && !sigaction(SIGTRAP,&sa,0));
}
__attribute__((noinline)) static void shim_fault(unsigned kind)
{
    if(kind==0) *(volatile uint32_t *)(uintptr_t)1=0;
    else if(kind==1) __asm__ volatile("xor %%edx,%%edx; xor %%ecx,%%ecx; div %%ecx" :::"eax","edx","ecx","cc");
    else if(kind==2) __asm__ volatile("ud2");
    else __asm__ volatile("outb %%al,$0x80" ::: "memory");
}
static uint32_t execute_flag_code(void)
{
    uint32_t result;
    uint32_t top=(uint32_t)(uintptr_t)flag_stack+16384;
    __asm__ volatile("movl %%esp,%%esi; movl %2,%%esp; call *%1; movl %%esi,%%esp"
        : "=a"(result) : "r"(flag_code),"r"(top) : "esi","ecx","edx","cc","memory");
    return result;
}
static void binary_flags(void)
{
    setup();
    flag_code=mmap((void *)0x20000000,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    flag_stack=mmap((void *)0x30000000,16384,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(flag_code!=(void *)-1 && flag_stack!=(void *)-1);
    memset(flag_code,0x90,4096);
    /* PUSHFD; CLI; CALL callee; POPFD; RET. The callee saves/restores IF twice. */
    const uint8_t code[]={0x9c,0xfa,0xe8,9,0,0,0,0x9d,0xc3};
    memcpy(flag_code,code,sizeof(code));
    const uint8_t callee[]={0x9c,0xfa,0x9c,0x9d,0x9d,0x9c,0x58,0xc3};memcpy(flag_code+16,callee,sizeof(callee));
    image.pe=(struct pe_image){.image=flag_code,.base=(uint32_t)(uintptr_t)flag_code,.size=4096,.section_count=1,
        .sections={{.rva=0,.span=64,.flags=0x60000000}}};
    image.hash[0]=12;image.platform.flag_table=&flag_table;
    assert(ndis_flags_patch(&image.pe,&flag_table,&flag_workspace,image.hash));
    assert(!mprotect(flag_code,4096,PROT_READ|PROT_EXEC));emulating=2;
    assert(!(execute_flag_code()&0x200) && h.virtual_if);
    emulating=0;assert(!mprotect(flag_code,4096,PROT_READ|PROT_WRITE));
    memset(flag_code,0x90,4096);memset(&flag_table,0,sizeof(flag_table));
    /* PUSHFD; zero extend CS; PUSH CS; PUSH target; IRETD; target: RET. */
    const uint8_t iret_code[]={0x9c,0x31,0xc0,0x66,0x8c,0xc8,0x50,0x68,0,0,0,0,0xcf};
    memcpy(flag_code,iret_code,sizeof(iret_code));pe_put32(flag_code+8,(uint32_t)(uintptr_t)flag_code+32);flag_code[32]=0xc3;
    assert(ndis_flags_patch(&image.pe,&flag_table,&flag_workspace,image.hash));
    assert(!mprotect(flag_code,4096,PROT_READ|PROT_EXEC));emulating=2;(void)execute_flag_code();
    assert(h.virtual_if);emulating=0;
    const uint8_t shadow[][5]={{0xfa,0xfb,0xc3,0x90,0x90},{0xfa,0xfb,0x9c,0x58,0xc3}};
    for(unsigned i=0;i<2;i++) {
        assert(!mprotect(flag_code,4096,PROT_READ|PROT_WRITE));
        memset(flag_code,0x90,4096);memset(&flag_table,0,sizeof(flag_table));memcpy(flag_code,shadow[i],5);
        assert(ndis_flags_patch(&image.pe,&flag_table,&flag_workspace,image.hash));
        assert(!mprotect(flag_code,4096,PROT_READ|PROT_EXEC));emulating=2;
        uint32_t saved_flags=execute_flag_code();emulating=0;
        assert(h.virtual_if && !h.sti_shadow);
        if(i) assert((saved_flags&0x300)==0x200); /* internal TF is absent */
    }
    assert(!munmap(flag_code,4096) && !munmap(flag_stack,16384));
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
    binary_flags();
    puts("PASS i386 CPL3 CLI/STI/HLT traps, physical PUSHFD/POPFD, forbidden OUT, PF/DE/UD, nonreturn deadlines");
    return 0;
}
