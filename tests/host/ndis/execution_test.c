/* SPDX-License-Identifier: GPL-2.0-only
 * Actual i386 exceptions/signals, using the production saved-context code.
 * Linux signal return stands in for the kernel's ring-0 IRET trampoline. */
#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "src/kernel/ndis/execution.h"
#include "src/kernel/ndis/scan.h"
extern uint8_t foreign_start[], foreign_end[];
#define DECL(n) extern int32_t NDIS_STDCALL foreign_##n(uint32_t,uint32_t)
DECL(sum); DECL(page); DECL(divide); DECL(invalid); DECL(shim_page); DECL(shim_divide);
DECL(shim_invalid); DECL(nested); DECL(locks); DECL(spin); DECL(shim_timeout);
static struct ntimage image;
static struct ndis_callback callback;
static struct ndis_runtime runtime;
static struct ndis_execution *slot;
static unsigned irq_depth, fences, quarantines, records;
static volatile sig_atomic_t unexpected_return, delivered;
static uint32_t locks[2];
struct ndis_execution **ndis_execution_slot(void) { return &slot; }
unsigned ndis_execution_irq_depth(void) { return irq_depth; }
bool registry_valid(int h,gen_t g) { return h==7 && g==11; }
int registry_quarantine(int h,gen_t g) { assert(h==7 && g==11); quarantines++; return 0; }
static uint64_t now(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}
static bool fence(void *p,const char *why) { (void)p; assert(why); fences++; return false; }
static void record(void *p,const char *driver,uint32_t vector,uint32_t offset,bool shim) {
    (void)p;(void)vector;(void)offset;(void)shim; assert(!strcmp(driver,"authored.sys")); records++;
}
static void fresh(void) {
    assert(!slot); memset(&callback,0,sizeof(callback)); memset(&runtime,0,sizeof(runtime));
    memset(&image,0,sizeof(image));
    image.pe=(struct pe_image){.base=(uintptr_t)foreign_start,.image=foreign_start,
        .size=(uint32_t)(foreign_end-foreign_start),.section_count=1,
        .sections={{.span=(uint32_t)(foreign_end-foreign_start),.flags=0x20000000}}};
    struct ndis_scan_error scan; assert(ndis_scan_image(&image.pe,&scan));
    image.file="authored.sys"; image.loaded=true; image.scanned=true;
    callback=(struct ndis_callback){.image=&image,.ops={.fence=fence,.fault_record=record},
        .dirql=5,.claims={7},.generations={11},.claim_count=1};
    runtime.callback=&callback; runtime.now=now;
    runtime.budgets[NDIS_PASSIVE]=1000; runtime.budgets[NDIS_DPC]=2000; runtime.budgets[NDIS_ISR]=50;
    unexpected_return=0; delivered=0; irq_depth=0; locks[0]=locks[1]=0;
}
static void handler(int sig,siginfo_t *info,void *context) {
    (void)info; ucontext_t *u=context;
    struct trap_frame tf={.eip=u->uc_mcontext.gregs[REG_EIP],.cs=8,
        .vector=u->uc_mcontext.gregs[REG_TRAPNO],.eflags=u->uc_mcontext.gregs[REG_EFL]};
    delivered++;
    if(sig==SIGALRM) ndis_execution_tick(&tf,now());
    else if(!ndis_execution_exception(&tf)) _exit(91);
    u->uc_mcontext.gregs[REG_EIP]=tf.eip;
    u->uc_mcontext.gregs[REG_EAX]=tf.eax;
    u->uc_mcontext.gregs[REG_EFL]=tf.eflags;
}
static int call(ndis_foreign2 f,enum ndis_callback_kind kind) {
    int32_t result=0;
    int ok=ndis_execute2(&runtime,(uintptr_t)f,f,3,4,kind,&result);
    if(!ok) assert(result==-1);
    return ok;
}
void test_shim_page(void) {
    assert(ndis_shim_enter()); __asm__ volatile("xor %%eax,%%eax; mov (%%eax),%%eax" ::: "eax","memory");
    unexpected_return=1; ndis_shim_leave();
}
void test_shim_divide(void) {
    assert(ndis_shim_enter()); __asm__ volatile("xor %%ecx,%%ecx; xor %%edx,%%edx; mov $1,%%eax; div %%ecx" ::: "eax","ecx","edx");
    unexpected_return=1; ndis_shim_leave();
}
void test_shim_invalid(void) { assert(ndis_shim_enter()); __asm__ volatile("ud2"); unexpected_return=1; ndis_shim_leave(); }
void test_nested(void) {
    assert(ndis_shim_enter()); call(foreign_invalid,NDIS_ISR);
    unexpected_return=1; ndis_shim_leave();
}
void test_locks(void) {
    assert(ndis_shim_enter()); uint8_t old;
    assert(ndis_spin_acquire(&callback,&locks[0],false,&old) && old==0);
    assert(ndis_spin_acquire(&callback,&locks[1],true,&old) && old==2);
    ndis_shim_leave();
}
void test_shim_timeout(void) {
    assert(ndis_shim_enter());
    while(!callback.timeout_pending) __asm__ volatile("" ::: "memory");
    /* Timer did not discard native shim state mid-service. */
    assert(callback.state==NDIS_CALLBACK_ACTIVE && !image.quarantined);
    ndis_shim_leave(); unexpected_return=1;
}
static void recovered(void) {
    assert(!slot && image.quarantined && callback.state==NDIS_CALLBACK_QUARANTINED);
    assert(!callback.depth && !callback.irql && !callback.lock_count && !unexpected_return);
    assert(!locks[0] && !locks[1] && !callback.fence_idle);
    assert(!call(foreign_sum,NDIS_PASSIVE));
}
int main(void) {
    _Static_assert(sizeof(void *)==4,"actual i386 execution");
    struct sigaction action={.sa_sigaction=handler,.sa_flags=SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGSEGV,&action,0)); assert(!sigaction(SIGFPE,&action,0));
    assert(!sigaction(SIGILL,&action,0)); assert(!sigaction(SIGALRM,&action,0));
    fresh(); assert(call(foreign_sum,NDIS_PASSIVE)); assert(!slot && !image.quarantined);
    ndis_foreign2 faults[]={foreign_page,foreign_divide,foreign_invalid,
        foreign_shim_page,foreign_shim_divide,foreign_shim_invalid,foreign_nested,foreign_locks};
    uint32_t vectors[]={14,0,6,14,0,6,6,6};
    for(unsigned i=0;i<sizeof(faults)/sizeof(faults[0]);i++) {
        fresh(); unsigned before=fences; assert(!call(faults[i],NDIS_PASSIVE)); recovered();
        assert(delivered==1 && fences==before+1 && callback.fault_vector==vectors[i]);
        assert(callback.fault_in_shim==(i>=3 && i<=5));
    }
    for(unsigned i=0;i<4;i++) {
        fresh(); struct itimerval timer={.it_value={0,5000}};
        assert(!setitimer(ITIMER_REAL,&timer,0));
        assert(!call(i==3?foreign_shim_timeout:foreign_spin,i==3?NDIS_PASSIVE:(enum ndis_callback_kind)i));
        timer=(struct itimerval){0}; assert(!setitimer(ITIMER_REAL,&timer,0));
        recovered(); assert(delivered==1);
    }
    assert(fences==12 && quarantines==12 && records==12);
    /* Same active shim must not swallow an unrelated host IRQ's exception. */
    fresh(); struct ndis_execution e={.callback=&callback,.shim_depth=1}; slot=&e;
    struct trap_frame tf={.cs=8,.vector=14,.eip=1}; irq_depth=1;
    assert(!ndis_execution_exception(&tf)); irq_depth=0;
    ndis_host_critical_enter(); assert(!ndis_execution_exception(&tf)); ndis_host_critical_leave();
    tf.vector=18; assert(!ndis_execution_exception(&tf));
    tf.vector=14; tf.cs=3; assert(!ndis_execution_exception(&tf)); slot=0;
    /* Ordinary successful release has actual exclusion and restores IRQL. */
    fresh(); assert(ndis_callback_begin(&callback,(uintptr_t)foreign_sum,0,1,100)); uint8_t a,b;
    assert(ndis_spin_acquire(&callback,&locks[0],false,&a) && locks[0]==1 && a==0);
    assert(ndis_spin_acquire(&callback,&locks[1],true,&b) && b==2);
    assert(ndis_spin_release(&callback,&locks[1],true,b) && !locks[1]);
    assert(ndis_spin_release(&callback,&locks[0],false,a) && !locks[0] && !callback.irql);
    assert(ndis_callback_end(&callback));
    puts("PASS i386 actual PF DE UD image/shim nested unwind ISR/DPC/passive nonreturn locks quarantine");
}
