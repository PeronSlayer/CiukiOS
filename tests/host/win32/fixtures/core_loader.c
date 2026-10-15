/* Test-only builtin loader; full ntdll belongs to f5-05. Executes f5-02 SEH
 * images and checks real ring-3 memory/fault/continue behavior.
 * SPDX-License-Identifier: MIT */
#include <ciuki/abi.h>
extern uint32_t core_call(void *, uint32_t);
extern void core_dispatch(void);
extern void core_stack_grow(void);
static uint32_t faults, errors;
static volatile uint32_t resume_ip, expected_status, escape_esp, escape_ebp;
volatile uint32_t CoreRelease;
#define release_gate CoreRelease
struct core_result { uint32_t magic, done, errors, faults, memory, fs, sse, stack, threads; };
struct core_result CoreResult;
#define result CoreResult
static void clear(void *p,uint32_t bytes) { uint8_t *b=p; while(bytes--) *b++=0; }
static void check(int value) { if(!value) errors++; }
static uint32_t invoke(void *p,uint32_t size,uint32_t op)
{
    struct ciuki_w32_header *h=p;h->version=1;h->size=size;h->operation=op;h->reserved=0;
    return core_call(p,size);
}
void core_dispatch_c(struct ciuki_w32_exception *e,struct ciuki_w32_context *c)
{
    faults++;
    if(resume_ip){
        check(e->code==expected_status);c->eip=resume_ip;resume_ip=0;
        if(e->code==W32_EXCEPTION_STACK){c->esp=escape_esp;c->ebp=escape_ebp;result.stack=1;}
        if(e->code==0xc000008e||e->code==W32_EXCEPTION_SSE){
            c->floating.control|=0x3f;c->floating.status=0;
            if(c->flags&0x20){
                *(uint16_t*)c->extended|=0x3f;*(uint16_t*)(c->extended+2)=0;
                *(uint32_t*)(c->extended+24)=0x1f80;
            }
        }
    }
    else {
        uint32_t *chain;__asm__ volatile("movl %%fs:0,%0":"=r"(chain));
        while((uint32_t)chain!=0xffffffff){
            int (*handler)(void*,void*,void*,void*)=(void*)chain[1];
            int action=handler(e,chain,c,0);
            if(!action)break;
            chain=(uint32_t*)chain[0];
        }
        if((uint32_t)chain==0xffffffff)errors++;
    }
    c->eflags&=~0x100u;
    struct ciuki_w32_continue a;clear(&a,sizeof(a));a.context=(uint32_t)c;
    invoke(&a,sizeof(a),W32_CONTINUE);
    for(;;)__asm__ volatile("ud2");
}
static void memory_checks(void)
{
    struct ciuki_w32_vm a;clear(&a,sizeof(a));a.bytes=3*4096;a.flags=W32_MEM_RESERVE|W32_MEM_COMMIT;a.protect=W32_PAGE_EXECUTE_READWRITE;
    check(!invoke(&a,sizeof(a),W32_VM_ALLOC));uint32_t base=a.address;
    volatile uint32_t *p=(void*)base;check(!*p);*p=0xfeedbeef;
    struct ciuki_w32_query q;clear(&q,sizeof(q));q.address=base;check(!invoke(&q,sizeof(q),W32_VM_QUERY));check(q.info.state==W32_MEM_COMMIT&&q.info.region_size==3*4096);
    clear(&a,sizeof(a));a.address=base;a.bytes=4096;a.protect=W32_PAGE_READONLY;check(!invoke(&a,sizeof(a),W32_VM_PROTECT));check(a.old_protect==W32_PAGE_EXECUTE_READWRITE);
    expected_status=W32_STATUS_ACCESS_VIOLATION;
    __asm__ volatile("movl $1f,%0; movl $7,(%1); 1:":"=m"(resume_ip):"r"(p):"memory");
    clear(&a,sizeof(a));a.address=base;a.bytes=4096;a.protect=W32_PAGE_READWRITE|W32_PAGE_GUARD;check(!invoke(&a,sizeof(a),W32_VM_PROTECT));
    expected_status=W32_EXCEPTION_GUARD;
    __asm__ volatile("movl $1f,%0; movl (%1),%%eax; 1:":"=m"(resume_ip):"r"(p):"eax","memory");
    check(*p==0xfeedbeef); /* guard removed once */
    clear(&a,sizeof(a));a.address=base;a.bytes=4096;a.flags=W32_MEM_DECOMMIT;check(!invoke(&a,sizeof(a),W32_VM_FREE));
    clear(&a,sizeof(a));a.address=base;a.bytes=4096;a.flags=W32_MEM_COMMIT;a.protect=W32_PAGE_READWRITE;check(!invoke(&a,sizeof(a),W32_VM_ALLOC)&&!*p);
    clear(&a,sizeof(a));a.address=base;a.flags=W32_MEM_RELEASE;check(!invoke(&a,sizeof(a),W32_VM_FREE));result.memory=1;
}
static volatile uint32_t thread_done;
static void thread_probe(uint32_t argument)
{
    uint32_t teb;uint16_t cw;
    __asm__ volatile("movl %%fs:0x18,%0; fnstcw %1":"=r"(teb),"=m"(cw));
    check(argument==0x12345678&&teb!=result.fs&&cw==0x27f);
    const uint32_t other[4]={0x87654321,0x87654321,0x87654321,0x87654321};
    __asm__ volatile("movups %0,%%xmm0"::"m"(other):"memory");
    thread_done=1;
}
static void thread_return(void)
{
    __asm__ volatile("movl %0,%%eax; xorl %%ebx,%%ebx; int $0x80"::"i"(CIUKI_SYS_THREAD_EXIT):"eax","ebx","memory");
    for(;;)__asm__ volatile("ud2");
}
static void thread_checks(void)
{
    const uint32_t wanted[4]={0x12345678,0x12345678,0x12345678,0x12345678};uint32_t observed[4];
    __asm__ volatile("movups %0,%%xmm0"::"m"(wanted):"memory");
    struct ciuki_w32_thread t;clear(&t,sizeof(t));t.entry=(uint32_t)thread_probe;t.argument=0x12345678;t.trampoline=(uint32_t)thread_return;
    check(!invoke(&t,sizeof(t),W32_THREAD)&&t.tid);
    while(!thread_done)__asm__ volatile("movl $1,%%eax; int $0x80":::"eax","memory");
    __asm__ volatile("movups %%xmm0,%0":"=m"(observed)::"memory");
    for(unsigned i=0;i<4;i++)check(observed[i]==wanted[i]);
    result.threads=1;
}
void entry(uint32_t peb)
{
    struct ciuki_w32_dispatcher dispatcher;clear(&dispatcher,sizeof(dispatcher));dispatcher.entry=(uint32_t)core_dispatch;
    check(!invoke(&dispatcher,sizeof(dispatcher),W32_DISPATCHER));
    uint32_t teb;__asm__ volatile("movl %%fs:0x18,%0":"=r"(teb));
    check(*(uint32_t*)(teb+0x18)==teb&&*(uint32_t*)(teb+0x30)==peb);result.fs=teb;
    uint32_t image=*(uint32_t*)(peb+8),offset=*(uint32_t*)(image+60),entry_rva=*(uint32_t*)(image+offset+24+16);
    uint32_t (*exe)(void)=(void*)(image+entry_rva);check(!exe()); /* f5-02 nested SEH */
    uint32_t params=*(uint32_t*)(peb+0x10);
    const char *mode=(void*)((struct ciuki_w32_parameters*)params)->command_line;
    if(mode[0]!='m') {
    expected_status=W32_EXCEPTION_PRIVILEGED;
    __asm__ volatile("movl $1f,%0; hlt; 1:":"=m"(resume_ip)::"memory");
    expected_status=W32_EXCEPTION_BREAKPOINT;
    __asm__ volatile("movl $1f,%0; int3; 1:":"=m"(resume_ip)::"memory");
    expected_status=W32_EXCEPTION_ILLEGAL;
    __asm__ volatile("movl $1f,%0; sysenter; 1:":"=m"(resume_ip)::"memory");
    expected_status=W32_STATUS_ACCESS_VIOLATION;
    __asm__ volatile("movl $1f,%0; int $0x2e; 1:":"=m"(resume_ip)::"memory");
    expected_status=W32_EXCEPTION_DIVIDE;
    __asm__ volatile("movl $1f,%0; xorl %%ecx,%%ecx; xorl %%edx,%%edx; movl $1,%%eax; divl %%ecx; 1:":"=m"(resume_ip)::"eax","ecx","edx","memory");
    expected_status=W32_STATUS_ACCESS_VIOLATION;
    __asm__ volatile("movl $1f,%0; xorl %%eax,%%eax; movl (%%eax),%%eax; 1:":"=m"(resume_ip)::"eax","memory");
    }
    if(mode[0]!='e') memory_checks();
    uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
    if((d&0x03000000)==0x03000000){__asm__ volatile("xorps %%xmm0,%%xmm0":::"memory");result.sse=1;}
    if(mode[0]!='m') {
        expected_status=W32_EXCEPTION_SINGLE_STEP;
        __asm__ volatile("movl $1f,%0; pushfl; orl $0x100,(%%esp); popfl; nop; 1:":"=m"(resume_ip)::"memory");
        uint16_t cw,unmasked;__asm__ volatile("fnstcw %0":"=m"(cw));unmasked=cw&~4u;
        expected_status=0xc000008e;
        uint32_t zero=0;
        __asm__ volatile("movl $1f,%0; fldcw %1; fld1; fdivs %2; fwait; 1: fninit; fldcw %3":
                         "=m"(resume_ip):"m"(unmasked),"m"(zero),"m"(cw):"memory");
        if(result.sse){
            uint32_t mxcsr=0x1d80,one=0x3f800000;
            expected_status=W32_EXCEPTION_SSE;
            __asm__ volatile("movl $1f,%0; ldmxcsr %1; movss %2,%%xmm0; divss %3,%%xmm0; 1:":
                             "=m"(resume_ip):"m"(mxcsr),"m"(one),"m"(zero):"memory");
        }
    }
    if(mode[0]!='e') {
        expected_status=W32_EXCEPTION_STACK;
        __asm__ volatile("movl %%esp,%0; movl %%ebp,%1; movl $1f,%2; calll _core_stack_grow; 1:":
                         "=m"(escape_esp),"=m"(escape_ebp),"=m"(resume_ip)::"eax","ecx","edx","memory");
    }
    if(result.sse) thread_checks();
    struct ciuki_w32_counter counter;clear(&counter,sizeof(counter));check(!invoke(&counter,sizeof(counter),W32_COUNTER)&&counter.frequency_lo==1193182);
    result.errors=errors;result.faults=faults;result.magic=0x57333243;result.done=1;
    /* The controller reads loader results through an exported symbol RVA. */
    while(!release_gate)__asm__ volatile("movl $1,%%eax; int $0x80":::"eax","memory");
    __asm__ volatile("xorl %%eax,%%eax; xorl %%ebx,%%ebx; int $0x80":::"eax","ebx","memory");
}
