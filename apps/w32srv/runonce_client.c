/* Win32 half of the desktop's RunOnce launcher. GetCommandLineA receives the
 * registry command verbatim through Ciuki's native-to-PE argv[1] seam. The
 * actual public CreateProcessA performs executable search and publication.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#include <ciuki/abi.h>
struct startup {DWORD size;char *reserved,*desktop,*title;DWORD x,y,width,height,chars_x,chars_y,fill,flags;uint16_t show,reserved_bytes;uint8_t *reserved2;HANDLE input,output,error;};
struct process_info {HANDLE process,thread;DWORD pid,tid;};
extern char *W32API GetCommandLineA(void);
extern BOOL W32API CreateProcessA(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,struct startup *,struct process_info *);
extern void W32API ExitProcess(DWORD);
#ifndef W32_HOST_MODEL
_Static_assert(sizeof(struct startup)==68,"STARTUPINFOA");
_Static_assert(sizeof(struct process_info)==16,"PROCESS_INFORMATION");
#endif
static void zero(void *p,uint32_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
#ifdef W32_RUNONCE_HOST
extern void runonce_report(int32_t);
#else
static void runonce_report(int32_t result)
{
    struct ciuki_message m;zero(&m,sizeof(m));m.length=8;
    for(unsigned i=0;i<4;i++)m.data[i]=(uint8_t)((uint32_t)result>>(8*i));
    uint32_t status;
    __asm__ volatile("pushl %%ebp; xorl %%ebp,%%ebp; int $0x80; popl %%ebp"
        : "=a"(status) : "a"(CIUKI_SYS_CHANNEL_SEND),"b"(4),"c"((uint32_t)(uintptr_t)&m),"d"(0),"S"(0),"D"(0) : "memory","cc");
    (void)status;
}
#endif
void W32API runonce_entry(void)
{
    struct startup startup;struct process_info info;zero(&startup,sizeof(startup));zero(&info,sizeof(info));startup.size=sizeof(startup);
    BOOL ok=CreateProcessA(NULL,GetCommandLineA(),NULL,NULL,0,0,NULL,"C:\\WINDOWS",&startup,&info);
    if(!ok){runonce_report(-(int32_t)GetLastError());ExitProcess(1);return;}
    CloseHandle(info.thread);runonce_report((int32_t)info.pid);
    WaitForSingleObject(info.process,0xffffffffu);CloseHandle(info.process);ExitProcess(0);
}
