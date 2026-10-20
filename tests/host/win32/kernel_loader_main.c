/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "adapter/linux.h"
#include <w32/store.h>
#include <w32/kernel32.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../../src/probes/win32_dialogs/controller.h"
#include "../../../src/probes/win32_upper/controller.h"
#include "../../../src/probes/win32_kernel32/controller.h"
extern void w32_kernel_model_init(const char *, uint32_t *, uint32_t *);
extern void w32_kernel_model_finish(void);

extern void w32_kernel_model_release_word(uint32_t);
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "runtime FAIL line %d: %s\n", __LINE__, #c);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static LONG handler(struct w32_exception *e, struct w32_seh *frame, struct w32_context *c,
                    struct w32_seh **dispatch) {
    (void)e;
    (void)frame;
    (void)c;
    (void)dispatch;
    return ExceptionContinueExecution;
}
static LONG filter(struct w32_exception_pointers *p) {
    return p->ExceptionRecord->ExceptionCode == 0xdead ? -1 : 0;
}
extern int w32_kernel32_runtime(void);
extern int w32_kernel32_fixed_alloc(void);
extern void w32_kernel_model_spawn_test(void);
extern void w32_kernel_model_spawn_edges(void);
static int runtime(void) {
    RTL_CRITICAL_SECTION *cs = host.services.alloc(0, sizeof(*cs));
    CHECK(cs);
    CHECK(!RtlInitializeCriticalSection(cs));
    cs->OwningThread = 99;
    w32_kernel_model_release_word((ULONG)(uintptr_t)&cs->OwningThread);
    CHECK(!RtlEnterCriticalSection(cs) && cs->RecursionCount == 1);
    CHECK(!RtlEnterCriticalSection(cs) && cs->RecursionCount == 2);
    CHECK(!RtlLeaveCriticalSection(cs) && !RtlLeaveCriticalSection(cs) && !cs->OwningThread);
    host.services.free(0, cs, sizeof(*cs));
    void *heap = RtlCreateHeap(HEAP_GROWABLE | HEAP_FREE_CHECKING_ENABLED, 0, 65536, 4096, 0, 0);
    CHECK(heap);
    unsigned char *block = RtlAllocateHeap(heap, HEAP_ZERO_MEMORY, 31);
    CHECK(block);
    for (unsigned i = 0; i < 31; i++)
        CHECK(!block[i]);
    block[0] = 0x5a;
    block = RtlReAllocateHeap(heap, HEAP_ZERO_MEMORY, block, 70000);
    CHECK(block && block[0] == 0x5a && RtlSizeHeap(heap, 0, block) == 70000);
    CHECK(RtlFreeHeap(heap, 0, block));
    CHECK(!RtlDestroyHeap(heap));
    struct w32_seh frame = {host.teb.Tib.ExceptionList, handler};
    host.teb.Tib.ExceptionList = &frame;
    host.teb.Tib.StackLimit = &frame;
    host.teb.Tib.StackBase = &frame + 1;
    struct w32_exception exception = {.ExceptionCode = 0xdead};
    struct w32_context context = {0};
    CHECK(!w32_dispatch_user_exception(&exception, &context));
    host.teb.Tib.ExceptionList = frame.Prev;
    SetUnhandledExceptionFilter(filter);
    CHECK(!w32_dispatch_user_exception(&exception, &context));
    SetUnhandledExceptionFilter(0);
    puts("PASS kernel heap critical waits SEH filter");
    return 0;
}
static int startup(void) {
    struct w32_loader loader;
    w32_ldr_init(&loader, &host.services, &host.peb);
    struct w32_module *main;
    CHECK(!w32_ciuki_start(&loader, &host.peb, "/compiled-tls.exe", "\"two  spaces\" /x", "C:\\",
                           "PATH=C:\\TOOLS\0TEMP=C:\\TEMP\0\0", &main));
    CHECK(main && loader.main == main && host.peb.ImageBaseAddress == main->pe.image);
    CHECK(host.teb.ThreadLocalStoragePointer && main->tls_index < 64);
    unsigned char *tls = host.teb.ThreadLocalStoragePointer[main->tls_index];
    CHECK(tls && tls[0] == 1 && tls[3] == 4 && tls[4] == 0 && tls[11] == 0);
    const char *command = "\"two  spaces\" /x";
    for (unsigned i = 0; command[i]; i++)
        CHECK(host.peb.ProcessParameters->CommandLine.Buffer[i] == (unsigned char)command[i]);
    CHECK(loader.path && !w32_strcmp(loader.path, "C:\\TOOLS"));
    struct w32_module *copy;
    CHECK(w32_ldr_load(&loader, "/compiled-tls.exe", W32_LOAD_DATAFILE, &copy));
    CHECK(copy->pe.image != main->pe.image && copy->tls_index == 0xffffffffu);
    CHECK(w32_ldr_free(&loader, copy));
    struct w32_module *relocated;
    CHECK(w32_ldr_load(&loader,"/reloc-highlow.exe",0,&relocated));
    CHECK(relocated->pe.base!=relocated->pe.preferred);
    CHECK(w32_u32(relocated->pe.image+0x1001)==0x402000u+relocated->pe.base-relocated->pe.preferred);
    CHECK(w32_ldr_free(&loader,relocated));
    puts("PASS kernel forced relocation COW");
    w32_ldr_shutdown(&loader);
    RtlDestroyHeap(host.peb.ProcessHeap);
    host.peb.ProcessHeap = 0;
    puts("PASS kernel startup parameters TLS DATAFILE");
    return 0;
}
extern HANDLE W32API GetModuleHandleA(const char *);
static int dialogs_startup(const char *path, uint32_t address) {
    /* Actual fixture imports, production loader, kernel32 and VM services.
     * PE callbacks are observed, never executed by this native H0 model. */
    struct w32_loader loader;
    w32_ldr_init(&loader, &host.services, &host.peb);
    struct w32_file manifest;
    CHECK(w32_ciuki_store(&loader, &manifest));
    struct w32_module *main;
    CHECK(!w32_ciuki_start(&loader, &host.peb, path, "", "C:\\", "\0\0", &main));
    CHECK(GetModuleHandleA(0) == main->pe.image);
    struct ciuki_w32_memory_info q;
    CHECK(VirtualQuery((void *)0x6fff0000, &q, sizeof(q)) == sizeof(q));
    printf("DIALOGS_LAYOUT old_result=6fff0000 allocation=%08x state=%08x bytes=%u parameters=%08x builtins=%08x main=%08x+%08x\n",
           q.allocation_base, q.state, q.region_size, K32_PTR(host.peb.ProcessParameters),
           K32_PTR(loader.builtins), main->pe.base, main->pe.size);
    CHECK(q.allocation_base == K32_PTR(loader.builtins));
    SetLastError(0);
    void *p = VirtualAlloc((void *)0x6fff0000, 4096, 0x3000, 4);
    printf("DIALOGS_ALLOC old_result=%p error=%u\n", p, GetLastError());
    CHECK(!p && GetLastError() == 487);
    p = (void *)(uintptr_t)address;
    CHECK(VirtualQuery(p, &q, sizeof(q)) == sizeof(q) && q.state == 0x10000);
    CHECK(VirtualAlloc(p, 4096, 0x3000, 4) == p);
    struct w32_dialogs_result *r = p;
    CHECK(!r->magic && !r->done && !r->release);
    r->magic = W32_DIALOGS_MAGIC;
    r->version = W32_DIALOGS_VERSION;
    CHECK(VirtualQuery(p, &q, sizeof(q)) == sizeof(q) && q.state == 0x1000 &&
          q.allocation_base == address && q.region_size == 4096 && q.protect == 4);
    CHECK(VirtualFree(p, 0, 0x8000));
    w32_ldr_shutdown(&loader);
    if (host.peb.ProcessHeap) RtlDestroyHeap(host.peb.ProcessHeap);
    host.peb.ProcessHeap = 0;
    puts("PASS dialogs startup old result collision=487 new result free commit RW zero GetModuleHandleA");
    return 0;
}
static int attach_failure(void) {
    struct w32_loader loader;
    w32_ldr_init(&loader,&host.services,&host.peb);w32_ldr_bind(&loader);
    CHECK(w32_ldr_thread_attach(&loader,&host.teb));host.fail_attach=1;
    struct w32_module *main;
    CHECK(w32_ldr_start_process(&loader,"/consumer.exe",&main)==STATUS_DLL_INIT_FAILED);
    CHECK(loader.error.code==1114&&!w32_strcmp(loader.error.reason,"dllmain-attach-failed"));
    CHECK(!loader.module_count&&!main);
    w32_ldr_shutdown(&loader);RtlDestroyHeap(host.peb.ProcessHeap);host.peb.ProcessHeap=0;
    puts("PASS kernel attach failure start status rollback");return 0;
}
static int store_test(void) {
    struct w32_loader loader;
    w32_ldr_init(&loader,&host.services,&host.peb);
    struct w32_file manifest;
    CHECK(w32_ciuki_store(&loader,&manifest));
    CHECK(loader.builtin_count==43); /* + f5-15A ciukishell platform exports */
    struct w32_module *main;
    CHECK(!w32_ciuki_start(&loader,&host.peb,"/builtin-import.exe","","C:\\","\0\0",&main));
    struct w32_module *core=w32_ldr_module(&loader,"ntdll.dll");
    struct w32_module *kernel32=w32_ldr_module(&loader,"kernel32.dll");
    CHECK(core&&kernel32&&core->builtin&&kernel32->builtin);
    struct w32_module *alias;
    CHECK(w32_ldr_load(&loader,"C:\\untrusted\\kernel32.dll",0,&alias)&&alias==kernel32);
    CHECK(w32_ldr_free(&loader,alias)&&w32_ldr_module(&loader,"kernel32.dll")==kernel32);
    CHECK(w32_ldr_proc(&loader,core,"__wine_dbg_header",0,0xffffffffu));
    CHECK(w32_ldr_proc(&loader,core,"__wine_dbg_output",0,0xffffffffu));
    CHECK(w32_ldr_proc(&loader,core,"__wine_dbg_strdup",0,0xffffffffu));
    struct w32_module *shell,*platform;
    CHECK(w32_ldr_load(&loader,"shell32.dll",0,&shell));
    void *folder=w32_ldr_proc(&loader,shell,"SHGetSpecialFolderPathA",0,0xffffffffu);
    platform=w32_ldr_module(&loader,"ciukishell.dll");
    CHECK(folder && platform && platform->builtin);
    CHECK(folder==w32_ldr_proc(&loader,platform,"SHGetSpecialFolderPathA",0,0xffffffffu));
    CHECK(w32_ldr_proc(&loader,shell,"DllGetClassObject",0,0xffffffffu)==
          w32_ldr_proc(&loader,platform,"DllGetClassObject",0,0xffffffffu));
    extern int w32_builtin_version(const char *,uint16_t *);
    uint16_t version[4];CHECK(w32_builtin_version("C:\\WINDOWS\\SYSTEM\\gdi32.dll",version));
    int matched=0;for(unsigned i=0;i<loader.builtin_count;i++)if(!w32_strcmp(loader.builtins[i].name,"gdi32.dll")){
        for(unsigned j=0;j<4;j++)CHECK(version[j]==loader.builtins[i].version[j]);matched=1;
    }CHECK(matched && !w32_builtin_version("unknown-title.dll",version));
    CHECK(w32_ldr_free(&loader,shell));
    w32_ldr_shutdown(&loader);RtlDestroyHeap(host.peb.ProcessHeap);host.peb.ProcessHeap=0;
    puts("PASS kernel builtin store imports helpers pinning");return 0;
}
static int upper_store_test(const char *name) {
    struct w32_loader loader;
    w32_ldr_init(&loader,&host.services,&host.peb);
    w32_ldr_bind(&loader);
    struct w32_file manifest;
    CHECK(w32_ciuki_store(&loader,&manifest));
    CHECK(w32_ldr_thread_attach(&loader,&host.teb));
    ULONG before=host.invocations;
    if(name) {
        struct w32_module *module;
        CHECK(w32_ldr_load(&loader,name,0,&module));
        CHECK(module->builtin);
        CHECK(w32_ldr_proc(&loader,module,"DirectXSetupA",0,0xffffffffu));
        CHECK(w32_ldr_proc(&loader,module,"DirectXSetupW",0,0xffffffffu));
        CHECK(w32_ldr_proc(&loader,module,"DirectXSetupGetVersion",0,0xffffffffu));
        CHECK(host.invocations-before==1);
    }
    w32_ldr_shutdown(&loader);
    CHECK(host.invocations-before==(name?2u:0u));
    printf("PASS kernel DirectSetup store H0 %s bind/callback observation only\n",name?name:"baseline");
    return 0;
}
/* f5-20A: every core export the reused Wine DLLs import (a list written
 * by the host test) resolves through the production loader and store,
 * forwarders included; loader reference, resource and NT services. */
extern NTSTATUS W32API LdrAddRefDll(ULONG, void *);
extern NTSTATUS W32API NtClose(void *);
extern NTSTATUS W32API NtQuerySystemTime(int64_t *);
extern NTSTATUS W32API NtAllocateVirtualMemory(void *, void **, ULONG, ULONG *, ULONG, ULONG);
extern HANDLE W32API FindResourceW(HANDLE, const WCHAR *, const WCHAR *);
extern HANDLE W32API FindResourceExW(HANDLE, const WCHAR *, const WCHAR *, uint16_t);
extern HANDLE W32API FindResourceA(HANDLE, const char *, const char *);
typedef BOOL(W32API *name_proc)(HANDLE, const void *, void *, intptr_t);
extern BOOL W32API EnumResourceNamesW(HANDLE, const WCHAR *, name_proc, intptr_t);
extern BOOL W32API EnumResourceNamesA(HANDLE, const char *, name_proc, intptr_t);
extern HANDLE W32API CreateEventA(SECURITY_ATTRIBUTES *, BOOL, BOOL, const char *);
static BOOL W32API count_names(HANDLE h, const void *type, void *name, intptr_t p) {
    (void)h;
    CHECK((uintptr_t)type == 16 && (uintptr_t)name == 1);
    (*(unsigned *)p)++;
    return 1;
}
/* f5-20B: the user32/gdi32 decisions of the reused Wine DLLs and the
 * amendment's kernel32 Unicode functions resolve through the production
 * loader and builtin store. */
static int upper_names(const char *images) {
    struct w32_loader loader;
    w32_ldr_init(&loader,&host.services,&host.peb);w32_ldr_bind(&loader);
    struct w32_file manifest;
    CHECK(w32_ciuki_store(&loader,&manifest));
    CHECK(w32_ldr_thread_attach(&loader,&host.teb));
    char path[600];snprintf(path,sizeof(path),"%s/System/Win32/upper-names.txt",images);
    FILE *list=fopen(path,"r");CHECK(list);
    char dll[64],name[128];unsigned count=0;
    while(fscanf(list,"%63s %127s",dll,name)==2){
        struct w32_module *m;
        CHECK(w32_ldr_load(&loader,dll,0,&m)&&m->builtin);
        if(!w32_ldr_proc(&loader,m,name,0,0xffffffffu)){fprintf(stderr,"unresolved %s!%s\n",dll,name);exit(1);}
        count++;
    }
    fclose(list);
    printf("PASS kernel f5-20B upper user32 gdi32 kernel32 exports=%u\n",count);
    return 0;
}
static int upper_core(const char *images) {
    struct w32_loader loader;
    w32_ldr_init(&loader,&host.services,&host.peb);w32_ldr_bind(&loader);
    struct w32_file manifest;
    CHECK(w32_ciuki_store(&loader,&manifest));
    CHECK(w32_ldr_thread_attach(&loader,&host.teb));
    char path[600];snprintf(path,sizeof(path),"%s/System/Win32/upper-symbols.txt",images);
    FILE *list=fopen(path,"r");CHECK(list);
    char dll[64],name[128];unsigned count=0;
    while(fscanf(list,"%63s %127s",dll,name)==2){
        struct w32_module *m;
        CHECK(w32_ldr_load(&loader,dll,0,&m)&&m->builtin);
        if(!w32_ldr_proc(&loader,m,name,0,0xffffffffu)){fprintf(stderr,"unresolved %s!%s\n",dll,name);exit(1);}
        count++;
    }
    fclose(list);
    struct w32_module *k=w32_ldr_module(&loader,"kernel32.dll"),*lz=w32_ldr_module(&loader,"lz32.dll");
    CHECK(k&&lz&&lz->builtin);
    void *base=k->pe.image;
    CHECK(!LdrAddRefDll(0,base)&&!LdrAddRefDll(1,base)&&k->entry.LoadCount==65535);
    CHECK(LdrAddRefDll(2,base)==STATUS_INVALID_PARAMETER&&LdrAddRefDll(0,(void *)0x1000)==STATUS_DLL_NOT_FOUND);
    struct w32_module *plain;
    CHECK(w32_ldr_load(&loader,"/exports.dll",0,&plain)&&!plain->builtin);
    ULONG references=plain->references;
    CHECK(!LdrAddRefDll(0,plain->pe.image)&&plain->references==references+1);
    CHECK(!LdrAddRefDll(1,plain->pe.image)&&w32_ldr_free(&loader,plain)&&w32_ldr_free(&loader,plain));
    CHECK(w32_ldr_module(&loader,"exports.dll")==plain&&plain->entry.LoadCount==65535);
    const WCHAR one[]={'#','1',0},sixteen[]={'#','1','6',0};
    HANDLE version=FindResourceW(base,(const WCHAR *)(uintptr_t)1,(const WCHAR *)(uintptr_t)16);
    CHECK(version&&FindResourceW(base,one,sixteen)==version&&FindResourceA(base,"#1","#16")==version);
    CHECK(FindResourceExW(base,(const WCHAR *)(uintptr_t)16,(const WCHAR *)(uintptr_t)1,0)==version);
    CHECK(!FindResourceW(base,(const WCHAR *)(uintptr_t)2,(const WCHAR *)(uintptr_t)16)&&GetLastError()==1814);
    const WCHAR bad[]={'#','x',0};
    CHECK(!FindResourceW(base,bad,sixteen)&&GetLastError()==1814);
    unsigned names=0;
    CHECK(EnumResourceNamesW(base,(const WCHAR *)(uintptr_t)16,count_names,(intptr_t)&names)&&names==1);
    CHECK(EnumResourceNamesA(base,(const char *)(uintptr_t)16,count_names,(intptr_t)&names)&&names==2);
    CHECK(!EnumResourceNamesW(base,(const WCHAR *)(uintptr_t)99,count_names,(intptr_t)&names)&&GetLastError()==1813);
    CHECK(!EnumResourceNamesW(base,(const WCHAR *)(uintptr_t)16,0,0)&&GetLastError()==87);
    int64_t now=0;CHECK(!NtQuerySystemTime(&now)&&now>0&&NtQuerySystemTime(0));
    void *region=0;ULONG bytes=5000;
    CHECK(!NtAllocateVirtualMemory((void *)(intptr_t)-1,&region,0,&bytes,0x3000,4)&&region&&bytes==8192);
    bytes=1;CHECK(NtAllocateVirtualMemory((void *)(intptr_t)-2,&region,0,&bytes,0x3000,4)==(NTSTATUS)0xc0000008);
    bytes=0;CHECK(NtAllocateVirtualMemory((void *)(intptr_t)-1,&region,0,&bytes,0x3000,4)==(NTSTATUS)0xc00000f2);
    HANDLE event=CreateEventA(0,0,0,0);
    CHECK(event&&!NtClose(event)&&NtClose(event)==(NTSTATUS)0xc0000008);
    w32_ldr_shutdown(&loader);RtlDestroyHeap(host.peb.ProcessHeap);host.peb.ProcessHeap=0;
    printf("PASS kernel f5-20A upper core exports=%u loader references resources NT services\n",count);
    return 0;
}
extern HANDLE W32API LoadLibraryA(const char *);
extern DWORD W32API GetModuleFileNameA(HANDLE,char *,DWORD);
extern BOOL W32API FreeLibrary(HANDLE);
static int unicode_paths(void) {
    struct w32_loader loader;w32_ldr_init(&loader,&host.services,&host.peb);w32_ldr_bind(&loader);
    CHECK(w32_ldr_thread_attach(&loader,&host.teb));
    const char *path="C:\\caf\xe9\\Euro\x80.dll";
    HANDLE h=LoadLibraryA(path);CHECK(h);
    char out[260];CHECK(GetModuleFileNameA(h,out,sizeof(out))==w32_strlen(path)&&!w32_strcmp(out,path));
    struct w32_module *m=w32_ldr_module(&loader,"Euro\xe2\x82\xac.dll");CHECK(m);
    CHECK(m->entry.FullDllName.Buffer[6]==0xe9&&m->entry.BaseDllName.Buffer[4]==0x20ac);
    CHECK(FreeLibrary(h));
    char long_path[260];w32_copy_string(long_path,sizeof(long_path),"C:\\caf\xe9\\");
    for(unsigned i=0;i<200;i++)long_path[8+i]=(char)0xe9;
    w32_copy_string(long_path+208,sizeof(long_path)-208,".dll");
    h=LoadLibraryA(long_path);CHECK(h);
    CHECK(GetModuleFileNameA(h,out,sizeof(out))==w32_strlen(long_path)&&!w32_strcmp(out,long_path));
    CHECK(FreeLibrary(h));w32_ldr_shutdown(&loader);RtlDestroyHeap(host.peb.ProcessHeap);host.peb.ProcessHeap=0;
    puts("PASS kernel32 ACP1252 UTF8 module path round trip");return 0;
}
int main(int argc, char **argv) {
    if (argc < 4)
        return 2;
    host_init();
    uint32_t pid, tid;
    w32_kernel_model_init(argv[1], &pid, &tid);
    int (*observer)(void *, void *, void *, uint32_t, int) = host.services.invoke;
    host.services = w32_ciuki_services;
    host.services.opaque = &host;
    host.services.invoke = observer;
    host.teb.ClientId.UniqueProcess = (void *)(uintptr_t)pid;
    host.teb.ClientId.UniqueThread = (void *)(uintptr_t)tid;
    w32_set_services(&host.services);
    if(!w32_strcmp(argv[2],"diagnostics")){
        for(unsigned i=0;i<70;i++)host.services.diagnostic(0,"stub","fixture.dll","FixtureMissing");
        host.services.diagnostic(0,"load","fixture.dll","missing-export");
        w32_kernel_model_finish();return 0;
    }
    if(!w32_strcmp(argv[2],"spawn-edges")){w32_kernel_model_spawn_edges();w32_kernel_model_finish();return 0;}
    if(!w32_strcmp(argv[2],"spawn")){w32_kernel_model_spawn_test();w32_kernel_model_finish();return 0;}
    if(!w32_strcmp(argv[2],"upper-store-baseline")||!w32_strcmp(argv[2],"upper-store-dsetup")||!w32_strcmp(argv[2],"upper-store-dsetup32")){
        const char *name=!w32_strcmp(argv[2],"upper-store-baseline")?0:
                         !w32_strcmp(argv[2],"upper-store-dsetup")?"dsetup.dll":"dsetup32.dll";
        int result=upper_store_test(name);w32_kernel_model_finish();return result;
    }
    int result = !w32_strcmp(argv[2], "kernel32") ? w32_kernel32_runtime()
                 : !w32_strcmp(argv[2], "fixed-alloc") ? w32_kernel32_fixed_alloc()
                 : !w32_strcmp(argv[2], "dialogs-startup") ? dialogs_startup(W32_DIALOGS_PATH, W32_DIALOGS_RESULT_VA)
                 : !w32_strcmp(argv[2], "upper-startup") ? dialogs_startup(W32_UPPER_PATH, W32_UPPER_RESULT_VA)
                 : !w32_strcmp(argv[2], "kernel32-startup") ? dialogs_startup(W32_KERNEL32_PATH, W32_KERNEL32_RESULT_VA)
                 : !w32_strcmp(argv[2], "unicode-paths") ? unicode_paths()
                 : !w32_strcmp(argv[2], "runtime")   ? runtime()
                 : !w32_strcmp(argv[2], "startup") ? startup()
                 : !w32_strcmp(argv[2], "store") ? store_test()
                 : !w32_strcmp(argv[2], "upper-core") ? upper_core(argv[1])
                 : !w32_strcmp(argv[2], "upper-names") ? upper_names(argv[1])
                 : !w32_strcmp(argv[2], "attach-fail") ? attach_failure()
                                                   : host_main(argc - 1, argv + 1);
    w32_kernel_model_finish();
    return result;
}
