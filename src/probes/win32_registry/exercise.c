/* Real registry RPC, corrupt-slot fallback and kernel-driven service crashes. LGPL-2.1-or-later */
#include <w32/kernel32.h>
#include "../win32_core/fixture.h"
extern LONG W32API RegCreateKeyExA(uint32_t,const char *,uint32_t,char *,uint32_t,uint32_t,void *,uint32_t *,uint32_t *);
extern LONG W32API RegSetValueExA(uint32_t,const char *,uint32_t,uint32_t,const uint8_t *,uint32_t);
extern LONG W32API RegQueryValueExA(uint32_t,const char *,void *,uint32_t *,uint8_t *,uint32_t *);
extern LONG W32API RegDeleteKeyA(uint32_t,const char *);
extern LONG W32API RegCloseKey(uint32_t);
extern LONG W32API RegOpenKeyExA(uint32_t,const char *,uint32_t,uint32_t,uint32_t *);
extern LONG W32API RegFlushKey(uint32_t);
extern HANDLE W32API CreateThread(SECURITY_ATTRIBUTES *,DWORD,DWORD(W32API *)(void *),void *,DWORD,DWORD *);
static const char path[]="Software\\CiukiOS\\F521B";
static uint32_t word(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int corrupt_latest(void)
{
    uint64_t sequence[2]={0,0};
    for(unsigned slot=0;slot<2;slot++) {
        char file[]="C:\\WINDOWS\\USER.CRG.0";file[sizeof(file)-2]=(char)('0'+slot);
        HANDLE h=CreateFileA(file,0x80000000u,3,0,3,0x80,0);uint8_t header[40];DWORD count=0;
        if(h==K32_INVALID)return 0;
        int ok=ReadFile(h,header,sizeof(header),&count,0);CloseHandle(h);
        if(!ok || count!=40 || word(header)!=0x31475243u)return 0;
        sequence[slot]=word(header+12)|((uint64_t)word(header+16)<<32);
    }
    if(!sequence[0] || !sequence[1] || sequence[0]==sequence[1])return 0;
    char file[]="C:\\WINDOWS\\USER.CRG.0";
    file[sizeof(file)-2]=sequence[1]>sequence[0] ? '1' : '0';
    HANDLE h=CreateFileA(file,0x40000000u,3,0,3,0x80,0);DWORD count=0;
    if(h==K32_INVALID)return 0;
    int ok=WriteFile(h,"!",1,&count,0) && count==1 && FlushFileBuffers(h);
    if(!CloseHandle(h))ok=0;
    return ok;
}
static int restart(unsigned sequence)
{
    result->request=sequence;
    for(unsigned i=0;i<10000 && result->acknowledged!=sequence;i++)Sleep(1);
    return result->acknowledged==sequence && !result->control_error;
}
static uint32_t reopen(unsigned row,uint32_t expected)
{
    uint32_t key=0;
    for(unsigned i=0;i<10000;i++) {
        if(!RegOpenKeyExA(0x80000001u,path,0,0xf003f,&key))break;
        Sleep(1);
    }
    CHECK(row,key);
    uint32_t value=0,bytes=4,type=0;
    if(key)CHECK(row,!RegQueryValueExA(key,"recovery",0,&type,(uint8_t *)&value,&bytes) &&
        type==4 && bytes==4 && value==expected);
    return key;
}
static DWORD W32API writer(void *argument)
{
    unsigned id=(unsigned)(uintptr_t)argument;
    const char *name=id ? "writer1" : "writer0";
    uint32_t key=0,disposition=0;
    LONG error=RegCreateKeyExA(0x80000001u,path,0,0,0,0xf003f,0,&key,&disposition);
    CHECK(2,!error && key);
    if(error || !key)return 1;
    for(uint32_t value=1;value<=8;value++) {
        CHECK(2,!RegSetValueExA(key,name,0,4,(uint8_t *)&value,4));
        uint32_t got=0,size=4,type=0;
        CHECK(2,!RegQueryValueExA(key,name,0,&type,(uint8_t *)&got,&size) && size==4 && type==4 && got==value);
        Sleep(1);
    }
    CHECK(2,!RegFlushKey(key));CHECK(2,!RegCloseKey(key));return 0;
}
void W32API entry(void)
{
    fixture_begin(W32_FIXTURE_REGISTRY);
    HANDLE a=CreateThread(0,0,writer,0,0,0),b=CreateThread(0,0,writer,(void *)1,0,0);
    CHECK(2,a && b);
    if(a){CHECK(2,WaitForSingleObject(a,15000)==0);CHECK(2,CloseHandle(a));}
    if(b){CHECK(2,WaitForSingleObject(b,15000)==0);CHECK(2,CloseHandle(b));}
    result->row[2].value[0]=!result->row[2].errors;
    uint32_t key=0,disposition=0,value=1;
    CHECK(0,!RegCreateKeyExA(0x80000001u,path,0,0,0,0xf003f,0,&key,&disposition));
    if(key) {
        CHECK(0,!RegSetValueExA(key,"recovery",0,4,(uint8_t *)&value,4));CHECK(0,!RegFlushKey(key));
        value=2;
        CHECK(0,!RegSetValueExA(key,"recovery",0,4,(uint8_t *)&value,4));CHECK(0,!RegFlushKey(key));
        CHECK(0,!RegCloseKey(key));
        CHECK(0,corrupt_latest());
        CHECK(0,restart(1));
        key=reopen(0,1);result->row[0].value[0]=!result->row[0].errors;
        if(key) {
            value=3;CHECK(1,!RegSetValueExA(key,"recovery",0,4,(uint8_t *)&value,4));
            CHECK(1,!RegFlushKey(key));CHECK(1,!RegCloseKey(key));
            CHECK(1,restart(2));key=reopen(1,3);
            if(key)CHECK(1,!RegCloseKey(key));
            result->row[1].value[0]=!result->row[1].errors;
        }
    }
    CHECK(1,!RegDeleteKeyA(0x80000001u,path));
    CHECK(1,!RegFlushKey(0x80000001u));
    fixture_finish();
}
