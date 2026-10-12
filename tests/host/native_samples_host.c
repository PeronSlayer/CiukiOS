/* SPDX-License-Identifier: MIT
 * Linux i386 reference adapter for SDK-ABI objects compiled by TCC. The real
 * newlib, CRT and pthread libraries are retained. This is host evidence only. */
#include <ciuki/runtime.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
static uint32_t linux_call(uint32_t n,uint32_t b,uint32_t c,uint32_t d)
{
    uint32_t r; __asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(b),"c"(c),"d"(d):"memory"); return r;
}
static unsigned char heap[8*1024*1024] __attribute__((aligned(4096)));
static uint32_t heap_end;
static union { struct ciuki_tcb tcb; unsigned char page[4096]; } tls;
uint32_t __wrap_ciuki_syscall(uint32_t n,uint32_t b,uint32_t c,uint32_t d,uint32_t s,uint32_t i,uint32_t p)
{
    (void)s; (void)i; (void)p;
    switch(n) {
    case CIUKI_SYS_EXIT: linux_call(1,b,0,0); __builtin_trap();
    case CIUKI_SYS_WRITE: return linux_call(4,b,c,d);
    case CIUKI_SYS_READ: return 0;
    case CIUKI_SYS_BRK:
        if (!heap_end) heap_end=CU_PTR(heap);
        if (b) { if (b<CU_PTR(heap) || b>CU_PTR(heap)+sizeof(heap)) return -ENOMEM; heap_end=b; }
        return heap_end;
    case CIUKI_SYS_FSTAT:
        memset((void *)(uintptr_t)c,0,sizeof(struct stat));
        ((struct stat *)(uintptr_t)c)->st_mode=S_IFCHR|0666; return 0;
    case CIUKI_SYS_FCNTL: return 0;
    case CIUKI_SYS_CLOSE: return 0;
    case CIUKI_SYS_GETPID: return 7;
    case CIUKI_SYS_WAKE_WORD: return 0;
    case CIUKI_SYS_WAIT_WORD: return -EAGAIN;
    default: return -ENOSYS;
    }
}
extern void __ciuki_start(uint32_t *);
void samples_host_start(uint32_t *linux_stack)
{
    (void)linux_stack;
    /* Linux's set_thread_area establishes the same GS layout the CiukiOS
       kernel supplies. It is confined to this reference process. */
    struct { uint32_t entry,base,limit,flags; } descriptor={0xffffffffu,CU_PTR(&tls),4095,0x41};
    if (linux_call(243,CU_PTR(&descriptor),0,0)) linux_call(1,124,0,0);
    uint16_t selector=(uint16_t)((descriptor.entry<<3)|3);
    __asm__ volatile("movw %0,%%gs"::"rm"(selector));
    tls.tcb.self=CU_PTR(&tls); tls.tcb.tid=1;
    static const char name[]="sample-host";
    uint32_t stack[]={1,CU_PTR(name),0,0,AT_CIUKI_TLS,CU_PTR(&tls),AT_NULL,0};
    __ciuki_start(stack); linux_call(1,125,0,0); __builtin_trap();
}
