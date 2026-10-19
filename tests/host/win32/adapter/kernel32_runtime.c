/* H1 i386 Linux: executes the production kernel32 stack switch and APC
 * dispatcher; only copied kernel records are adapted. No guest boot evidence.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "linux.h"
#include <w32/kernel32.h>
#define CHECK(c) do {if (!(c)) {host_print("FAIL i386 kernel32 line ");host_number(__LINE__);host_print("\n");host_exit(1);}} while (0)
extern void *W32API ConvertThreadToFiber(void *);
extern void *W32API CreateFiber(DWORD,void(W32API *)(void *),void *);
extern void W32API SwitchToFiber(void *);
extern void W32API DeleteFiber(void *);
extern BOOL W32API ConvertFiberToThread(void);
extern void *W32API GetFiberData(void);
extern DWORD W32API SleepEx(DWORD,BOOL);
extern DWORD W32API QueueUserAPC(void(W32API *)(uintptr_t),HANDLE,uintptr_t);
static void *root,*fiber;
static unsigned switches,callback_count;
static uint16_t control(void){uint16_t v;__asm__ volatile("fnstcw %0":"=m"(v));return v;}
static void set_control(uint16_t v){__asm__ volatile("fldcw %0"::"m"(v));}
static void W32API worker(void *data) {
  volatile DWORD local=0xfeed1234;
  CHECK(data==(void *)0xc1a0&&GetFiberData()==data&&control()==0x027f);
  struct w32_seh frame={NtCurrentTeb()->Tib.ExceptionList,0};NtCurrentTeb()->Tib.ExceptionList=&frame;
  void *base=NtCurrentTeb()->Tib.StackBase;
  for (unsigned i=0;i<32;i++) {
    switches++;set_control(0x0f7f);SwitchToFiber(root);
    CHECK(local==0xfeed1234&&control()==0x0f7f&&NtCurrentTeb()->Tib.ExceptionList==&frame&&NtCurrentTeb()->Tib.StackBase==base);
  }
  SwitchToFiber(root);CHECK(0);
}
struct queued {DWORD routine,arg,low,high;};
static struct queued queue[8];static unsigned count;
static DWORD dispatcher;
static struct w32_context return_context;
static volatile unsigned returning;
int32_t w32_ciuki_call(void *record,uint32_t size) {
  struct ciuki_w32_header *h=record;CHECK(size==h->size);
  if (h->operation==W32_VM_ALLOC) {
    struct ciuki_w32_vm *a=record;void *p=host.services.alloc(&host,a->bytes);
    if (!p)return STATUS_NO_MEMORY;a->address=K32_PTR(p);return 0;
  }
  if (h->operation==W32_VM_PROTECT) {
    struct ciuki_w32_vm *a=record;CHECK(host.services.protect(&host,(void *)(uintptr_t)a->address,a->bytes,a->protect==1 ? 0 : 3));a->old_protect=4;return 0;
  }
  if (h->operation==W32_VM_FREE) {
    /* The production allocator already tracks these exact fiber stack sizes. */
    struct ciuki_w32_vm *a=record;host.services.free(&host,(void *)(uintptr_t)a->address,5*4096);return 0;
  }
  if (h->operation==W32_SYSTEM) {CHECK(((struct ciuki_w32_system_args *)h)->action==W32_S_YIELD);return 0;}
  if (h->operation==W32_APC) {
    struct ciuki_w32_apc_args *a=record;
    if(a->action==W32_APC_REGISTER){dispatcher=a->routine;return 0;}
    if(a->action==W32_APC_QUEUE){CHECK(count<8);queue[count++]=(struct queued){a->routine,a->argument,0,0};a->result=1;return 0;}
    CHECK(a->action==W32_APC_RETURN&&returning);returning=0;host_resume(&return_context);
  }
  CHECK(h->operation==W32_WAIT);
  struct ciuki_w32_wait_args *a=record;
  a->result=258;
  if(a->alertable&&count){
    struct queued q=queue[0];for(unsigned i=1;i<count;i++)queue[i-1]=queue[i];count--;
    returning=1;RtlCaptureContext(&return_context);
    if(returning)((void(W32API *)(void *,DWORD,DWORD,DWORD))(uintptr_t)dispatcher)((void *)(uintptr_t)q.routine,q.arg,q.low,q.high);
    a->result=192;
  }
  return 0;
}
static void W32API apc(uintptr_t value){CHECK(value==++callback_count);}
static void W32API timer(void *arg,DWORD low,DWORD high){CHECK(arg==(void *)3&&low==0x1234&&high==0x5678);callback_count++;}
int host_main(int argc,char **argv) {
  (void)argc;(void)argv;
  RTL_CRITICAL_SECTION cs;CHECK(!RtlInitializeCriticalSection(&cs));host.peb.FastPebLock=&cs;
  root=ConvertThreadToFiber((void *)0xabba);CHECK(root);
  struct w32_seh *exception=host.teb.Tib.ExceptionList;void *base=host.teb.Tib.StackBase;
  fiber=CreateFiber(16384,worker,(void *)0xc1a0);CHECK(fiber);
  set_control(0x0b7f);
  for(unsigned i=0;i<32;i++){
    SwitchToFiber(fiber);CHECK(switches==i+1&&control()==0x0b7f&&GetFiberData()==(void *)0xabba);
    CHECK(host.teb.Tib.ExceptionList==exception&&host.teb.Tib.StackBase==base);
  }
  DeleteFiber(fiber);CHECK(ConvertFiberToThread());
  CHECK(QueueUserAPC(apc,(HANDLE)(intptr_t)-2,1)&&QueueUserAPC(apc,(HANDLE)(intptr_t)-2,2));
  CHECK(!SleepEx(0,0)&&!callback_count&&count==2);
  CHECK(SleepEx(1,1)==192&&callback_count==1);
  CHECK(SleepEx(1,1)==192&&callback_count==2);
  queue[count++]=(struct queued){K32_PTR(timer),3,0x1234,0x5678};
  CHECK(SleepEx(1,1)==192&&callback_count==3&&!count);
  CHECK(!host.allocations&&!host.mappings);
  host_print("PASS i386 kernel32 fibers stack SEH x87 APC alertable FIFO one/three argument dispatch\n");return 0;
}
