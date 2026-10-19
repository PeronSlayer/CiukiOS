/* Production parser, matching, versions, heaps, typed failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <w32/shims.h>
#include <w32/kernel32.h>
_Thread_local struct w32_teb *w32_model_teb;
static unsigned stub_records;
static const char *db,*overrides;
static struct ciuki_w32_compat configured;
DWORD W32API GetLastError(void){return w32_model_teb->LastErrorValue;}
void W32API SetLastError(DWORD n){w32_model_teb->LastErrorValue=n;}
int k32_status(int32_t status){return !status;}
int32_t w32_ciuki_call(void *record,uint32_t bytes){
    (void)bytes;struct ciuki_w32_header *h=record;
    if(h->operation==W32_IDENTITY){struct ciuki_w32_identity *a=record;a->major=4;a->minor=10;a->build=2222;a->platform=1;a->get_version=0x88ae0a04;return 0;}
    if(h->operation==W32_SYSTEM){struct ciuki_w32_system_args *a=record;a->low=131072;a->high=65536;a->extra=100;return 0;}
    return -1;
}
extern DWORD W32API GetVersion(void);
extern BOOL W32API GetVersionExA(OSVERSIONINFOA *);
extern BOOL W32API VerifyVersionInfoA(OSVERSIONINFOEXA *,DWORD,uint64_t);
extern uint64_t W32API VerSetConditionMask(uint64_t,DWORD,uint8_t);
extern void W32API GlobalMemoryStatus(MEMORYSTATUS *);
extern BOOL W32API GlobalMemoryStatusEx(MEMORYSTATUSEX *);
extern unsigned W32API RegOpenKeyExW(unsigned,unsigned,unsigned,unsigned,unsigned);
extern unsigned W32API VerQueryValueW(unsigned,unsigned,unsigned,unsigned);
extern unsigned W32API LZOpenFileW(unsigned,unsigned,unsigned);
extern unsigned wide_real_calls;
static void *allocate(void *o,uint32_t n){(void)o;return calloc(1,n);}
static void release(void *o,void *p,uint32_t n){(void)o;(void)n;free(p);}
static uint32_t tid(void *o){(void)o;return 1;}
static void diagnostic(void *o,const char *event,const char *module,const char *detail){(void)o;(void)module;(void)detail;if(!strcmp(event,"stub"))stub_records++;}
static int open_db(void *o,const char *path,struct w32_file *f){(void)o;
    if(strcmp(path,"C:\\System\\Win32\\SHIMS.INI"))return 0;
    *f=(struct w32_file){(const void *)db,(uint32_t)strlen(db),0};return 1;
}
static void close_db(void *o,struct w32_file *f){(void)o;(void)f;}
static int fingerprint(void *o,struct w32_shim_identity *id){(void)o;id->file_size=1024;id->timestamp=7;id->image_size=8192;memset(id->sha256,'a',64);id->sha256[64]=0;return 1;}
static int read_override(void *o,void *text,uint32_t cap,uint32_t *bytes){(void)o;*bytes=(uint32_t)strlen(overrides);if(*bytes>cap)return -1;memcpy(text,overrides,*bytes);return 1;}
static int configure(void *o,const struct ciuki_w32_compat *c,const struct w32_shim_identity *id){(void)o;(void)id;configured=*c;return 1;}
void W32API RtlRaiseStatus(NTSTATUS status){(void)status;abort();}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"shim line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void){
    struct w32_teb teb={0};w32_model_teb=&teb;
    const struct w32_services services={.alloc=allocate,.free=release,.thread_id=tid,.diagnostic=diagnostic};w32_set_services(&services);
    const char *expected[]={"version","ddraw-555","ddraw-scale","ddraw-windowed","heap-zero","heap-pad","memstatus-cap","io-pit","io-gameport","dll-native","fpu-cw","w-stubs"};
    for(unsigned i=0;i<12;i++){CHECK(!strcmp(expected[i],w32_shim_names[i]));CHECK(w32_shim_versions[i]==1);}
    const char *all="version@1(xp),ddraw-555@1,ddraw-scale@1(3),ddraw-windowed@1,heap-zero@1,heap-pad@1(16),memstatus-cap@1(64),io-pit@1,io-gameport@1,dll-native@1(game.dll|sound.dll),fpu-cw@1(0x027f),w-stubs@1(GetVersionExW|CreateFileW)";
    CHECK(w32_shim_list(all,&w32_shims)&&w32_shims.mask==4095);
    CHECK(w32_shim_native("GAME.DLL")&&!w32_shim_native("kernel32.dll")&&!w32_shim_native("game.dll.extra"));
    CHECK(w32_shim_w_stub("CreateFileW")&&!w32_shim_w_stub("CreateFileA"));
    char rendered[1024];CHECK(w32_shim_format(&w32_shims,rendered,sizeof(rendered)));struct ciuki_w32_compat roundtrip;CHECK(w32_shim_list(rendered,&roundtrip)&&!memcmp(&roundtrip,&w32_shims,sizeof(roundtrip)));
    CHECK(!w32_shim_format(&w32_shims,rendered,4));
    const char *bad[]={"version@2(xp)","version@0(xp)","heap-zero@1,heap-zero@1","heap-pad@1(65537)","heap-pad@1(4294967296)","memstatus-cap@1(0)","memstatus-cap@1(4097)","fpu-cw@1(0x017f)","fpu-cw@1(0xffff)","dll-native@1(kernel32.dll)","dll-native@1(../game.dll)","w-stubs@1(GetVersionExA)","unknown@1","ddraw-scale@1(0)","ddraw-scale@1(mode:9999x480)","heap-zero@1(x)","heap-zero@1,"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)CHECK(!w32_shim_list(bad[i],&roundtrip));
    CHECK(w32_shim_list("ddraw-scale@1(mode:640x480)",&roundtrip)&&roundtrip.width==640&&roundtrip.height==480);
    CHECK(w32_shim_list("version@1(95)",&roundtrip));
    struct w32_shim_identity id={.exe="C:\\Games\\Game.EXE",.timestamp=7,.image_size=8192,.file_size=1024};memset(id.sha256,'a',64);id.sha256[64]=0;
    const char *database="[game.exe]\nfile-size=1024\nshims=heap-zero@1\n[game.exe]\nTimeDateStamp=7\nSizeOfImage=8192\nshims=version@1(me)\n[game.exe]\nsha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\nshims=heap-pad@1(32)\n";
    struct w32_shim_match m;CHECK(w32_shim_parse(database,(uint32_t)strlen(database),&id,&m)&&m.rank==4&&m.conflicts==2&&m.settings.heap_pad==32);
    id.sha256[0]='b';CHECK(w32_shim_parse(database,(uint32_t)strlen(database),&id,&m)&&m.rank==3&&m.settings.version==2);
    id.timestamp=8;CHECK(w32_shim_parse(database,(uint32_t)strlen(database),&id,&m)&&m.rank==1&&m.settings.mask==W32_SHIM_BIT(W32_SH_HEAP_ZERO));
    const char *override="[game.exe]\nfile-size=1024\nshims=\n";struct w32_shim_match over;CHECK(w32_shim_parse(override,(uint32_t)strlen(override),&id,&over)&&over.rank&&!over.settings.mask);
    struct w32_services boot=services;boot.open=open_db;boot.close=close_db;boot.fingerprint=fingerprint;boot.compat_database=read_override;boot.configure=configure;
    struct w32_loader loader={.services=&boot};db=database;overrides=override;
    CHECK(w32_shim_start(&loader,"C:\\Games\\Game.EXE")&&!configured.mask);
    overrides="";CHECK(w32_shim_start(&loader,"Game.EXE")&&configured.heap_pad==32);
    const char *invalid[]={"[x]\nshims=heap-zero@1\n","[x]\nfile-size=1\nfile-size=1\nshims=\n","[x]\nfile-size=1\nshims=bad@1\n","[../x]\nfile-size=1\nshims=\n","[x]\nfile-size=-1\nshims=\n","[x]\nunknown=1\nshims=\n"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++)CHECK(!w32_shim_parse(invalid[i],(uint32_t)strlen(invalid[i]),&id,&m));
    CHECK(!w32_shim_parse("",65537,&id,&m));
    char longline[1025];memset(longline,'x',sizeof(longline));CHECK(!w32_shim_parse(longline,sizeof(longline),&id,&m));
    w32_shims.mask|=W32_SHIM_BIT(W32_SH_VERSION);
    for(unsigned v=1;v<=5;v++){w32_shims.version=v;struct ciuki_w32_identity a={0};w32_shim_version(&a);CHECK(a.major>=4&&a.major<=5&&a.platform==(v<=2?1u:2u));}
    w32_shims.version=5;OSVERSIONINFOEXA ver={.base.size=sizeof(ver)};
    CHECK(GetVersion()==((2600u<<16)|0x105));CHECK(GetVersionExA(&ver.base)&&ver.base.major==5&&ver.base.minor==1&&ver.product==1);
    uint64_t condition=VerSetConditionMask(VerSetConditionMask(0,2,3),1,3);
    ver.base.major=4;ver.base.minor=90;CHECK(VerifyVersionInfoA(&ver,3,condition));
    ver.base.major=6;ver.base.minor=0;CHECK(!VerifyVersionInfoA(&ver,3,condition)&&GetLastError()==1150);
    ver.base.major=5;ver.base.minor=0;condition=VerSetConditionMask(VerSetConditionMask(0,2,2),1,2);CHECK(VerifyVersionInfoA(&ver,3,condition));
    ver.base.minor=1;CHECK(!VerifyVersionInfoA(&ver,3,condition));
    CHECK(w32_shim_list("memstatus-cap@1(64)",&w32_shims));MEMORYSTATUS mem={0};GlobalMemoryStatus(&mem);
    CHECK(mem.total_phys==64*1048576&&mem.avail_phys<=mem.total_phys&&mem.total_pagefile==mem.total_phys);
    MEMORYSTATUSEX mx={.length=sizeof(mx)};CHECK(GlobalMemoryStatusEx(&mx)&&mx.total_phys==mem.total_phys&&mx.avail_phys<=mx.total_phys);
    CHECK(w32_shim_list("heap-zero@1,heap-pad@1(16)",&w32_shims));
    void *heap=RtlCreateHeap(HEAP_GROWABLE|HEAP_TAIL_CHECKING_ENABLED,0,0,0,0,0);CHECK(heap);
    unsigned char *p=RtlAllocateHeap(heap,0,8);CHECK(p&&RtlSizeHeap(heap,0,p)==8);for(unsigned i=0;i<8;i++)CHECK(!p[i]);
    memset(p,0x5a,24);CHECK(RtlValidateHeap(heap,0,p));CHECK(RtlFreeHeap(heap,0,p));
    p=RtlAllocateHeap(heap,0,8);for(unsigned i=0;i<8;i++)CHECK(!p[i]);
    p[0]=0x33;p=RtlReAllocateHeap(heap,0,p,32);CHECK(p&&p[0]==0x33);for(unsigned i=8;i<32;i++)CHECK(!p[i]);CHECK(RtlFreeHeap(heap,0,p));CHECK(!RtlDestroyHeap(heap));
    volatile uint32_t once[7]={0};const uint32_t values[]={0,0x80004001,8,120,UINT32_MAX,0,0x7fffffff};
    for(unsigned i=0;i<7;i++)for(unsigned j=0;j<2;j++){CHECK(w32_shim_stub(once+i,"fixture.dll","Missing",i)==values[i]);CHECK(teb.LastErrorValue==(i==5?5u:120u));}
    CHECK(stub_records==7);
    CHECK(w32_shim_list("w-stubs@1(RegOpenKeyExW|VerQueryValueW|LZOpenFileW)",&w32_shims));
    for(unsigned i=0;i<2;i++){
        CHECK(RegOpenKeyExW(UINT32_MAX,UINT32_MAX,0,0,UINT32_MAX)==120&&GetLastError()==120);
        CHECK(!VerQueryValueW(UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX)&&GetLastError()==120);
        CHECK(LZOpenFileW(UINT32_MAX,UINT32_MAX,0)==UINT32_MAX&&GetLastError()==120);
    }
    CHECK(stub_records==10&&!wide_real_calls);
    CHECK(w32_shim_list("",&w32_shims));
    CHECK(RegOpenKeyExW(0,0,0,0,0)==17&&VerQueryValueW(0,0,0,0)==17&&LZOpenFileW(0,0,0)==17&&wide_real_calls==3);
    puts("PASS all 12 shim versions, bounds, matching, precedence, conflict, hash, heap behavior and typed failures");return 0;
}
