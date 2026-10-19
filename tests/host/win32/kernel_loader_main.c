/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "adapter/linux.h"
#include <w32/store.h>
#include <stdio.h>
#include <stdlib.h>
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
extern void w32_kernel_model_spawn_test(void);
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
    CHECK(loader.builtin_count==28);
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
    w32_ldr_shutdown(&loader);RtlDestroyHeap(host.peb.ProcessHeap);host.peb.ProcessHeap=0;
    puts("PASS kernel builtin store imports helpers pinning");return 0;
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
    if(!w32_strcmp(argv[2],"spawn")){w32_kernel_model_spawn_test();w32_kernel_model_finish();return 0;}
    int result = !w32_strcmp(argv[2], "kernel32") ? w32_kernel32_runtime()
                 : !w32_strcmp(argv[2], "runtime")   ? runtime()
                 : !w32_strcmp(argv[2], "startup") ? startup()
                 : !w32_strcmp(argv[2], "store") ? store_test()
                 : !w32_strcmp(argv[2], "attach-fail") ? attach_failure()
                                                   : host_main(argc - 1, argv + 1);
    w32_kernel_model_finish();
    return result;
}
