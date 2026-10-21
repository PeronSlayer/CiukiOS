/* Eight isolated unhandled exceptions and bounded crash text checks. LGPL-2.1-or-later */
#include <w32/shell.h>
#include "../win32_core/fixture.h"
extern char *W32API GetCommandLineA(void);
static const uint32_t codes[]={0xc0000005u,0xc0000094u,0xc0000096u,0x80000003u,
    0xc000001du,0xc00000fdu,0xc000008eu,0xc0000005u};
static void fault(unsigned id)
{
    switch(id) {
    case 0:__asm__ volatile("movl $0x123,%%eax;movl %%eax,(%%eax)":::"eax","memory");break;
    case 1:__asm__ volatile("movl $1,%%eax;xorl %%edx,%%edx;xorl %%ecx,%%ecx;divl %%ecx":::"eax","ecx","edx","cc");break;
    case 2:__asm__ volatile("cli");break;
    case 3:__asm__ volatile("int3");break;
    case 4:__asm__ volatile("ud2");break;
    /* Touch each successive page: this exercises guard growth and reserve
     * exhaustion instead of jumping over the guard into an unrelated AV. */
    case 5:__asm__ volatile("1:subl $4096,%%esp;movl $0,(%%esp);jmp 1b":::"memory");break;
    case 6: {
        uint16_t control=0x037b; /* Unmask x87 zero-divide only. */
        __asm__ volatile("fninit;fldcw %0;fld1;fldz;.byte 0xde,0xf9;fwait"::"m"(control):"memory");break;
    }
    case 7:__asm__ volatile("movl $0x123,%%esp;movl $0,0x123":::"memory");break;
    }
    ExitProcess(2);
}
static const char *find(const char *s,const char *part)
{
    for(;*s;s++) {unsigned i=0;while(part[i] && s[i]==part[i])i++;if(!part[i])return s+i;}
    return 0;
}
static uint32_t number(const char *p,unsigned base)
{
    uint32_t n=0;if(!p)return 0;
    for(;;p++) {
        unsigned d=*p>='0' && *p<='9' ? (unsigned)(*p-'0') : *p>='a' && *p<='f' ? (unsigned)(*p-'a'+10) : base;
        if(d>=base)return n;n=n*base+d;
    }
}
static unsigned stack_words(const char *p)
{
    if(!p)return 0;
    unsigned words=0;
    while(*p==' ') {
        p++;unsigned n=0;
        while((*p>='0' && *p<='9') || (*p>='a' && *p<='f') || *p=='?'){n++;p++;}
        if(n!=8)return 0;words++;
    }
    return *p=='\n' ? words : 0;
}
static uint32_t pids[8];
static unsigned scan(unsigned *classes,unsigned *words)
{
    uint32_t seen=0,kind=0;unsigned files=0,minimum=8;
    for(unsigned i=0;i<8;i++) {
        char path[]="C:\\WINDOWS\\CRASH\\CRASH0.TXT";path[sizeof(path)-6]=(char)('0'+i);
        HANDLE file=CreateFileA(path,0x80000000u,3,0,3,0x80,0);
        if(file==K32_INVALID)continue;
        char text[2048]={0};DWORD bytes=0;
        int ok=ReadFile(file,text,sizeof(text)-1,&bytes,0);CloseHandle(file);
        if(!ok || !bytes || bytes>=sizeof(text)-1)continue;
        unsigned pid=number(find(text,"PID: "),10),code=number(find(text,"Exception: "),16);
        unsigned n=stack_words(find(text,"Stack:"));
        if(n!=8)continue; /* The production disk worker may still be writing. */
        for(unsigned j=0;j<8;j++)if(pid==pids[j] && code==codes[j] && !(seen&(1u<<j))) {
            seen|=1u<<j;kind|=1u<<(j==7 ? 0 : j);files++;
            if(n<minimum)minimum=n;
        }
    }
    *classes=0;for(unsigned i=0;i<7;i++)*classes+=(kind>>i)&1u;
    *words=files ? minimum : 0;return files;
}
void W32API entry(void)
{
    /* The child exits through the unhandled exception path; it never writes
     * a passing result block. The parent only counts matching crash files. */
    const char *argument=find(GetCommandLineA(),"/fault=");
    if(argument && *argument>='0' && *argument<='7') {
        fault((unsigned)(*argument-'0'));
    }
    fixture_begin(W32_FIXTURE_CRASH);
    for(unsigned i=0;i<8;i++) {
        char command[]="C:\\System\\Win32\\Fixtures\\crash-exercise.exe /fault=0";
        command[sizeof(command)-2]=(char)('0'+i);
        struct shell_startup startup={.size=sizeof(startup)};struct shell_process child={0};
        int made=CreateProcessA("C:\\System\\Win32\\Fixtures\\crash-exercise.exe",command,0,0,0,0,0,0,&startup,&child);
        CHECK(0,made && child.pid);
        if(!made)continue;
        pids[i]=child.pid;CHECK(0,WaitForSingleObject(child.process,10000)==0);
        CHECK(0,CloseHandle(child.thread));CHECK(0,CloseHandle(child.process));
        /* The desktop writes asynchronously. Observe this PID's file before
         * starting another crash, avoiding a rotation race. */
        unsigned classes=0,words=0,files=0;
        for(unsigned wait=0;wait<5000;wait++) {
            files=scan(&classes,&words);if(files==i+1)break;Sleep(1);
        }
        CHECK(0,files==i+1 && words==8);
        result->row[0].value[0]=classes;result->row[0].value[1]=files;result->row[0].value[2]=words;
    }
    fixture_finish();
}
