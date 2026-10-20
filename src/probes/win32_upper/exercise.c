/* f5-20D authored guest fixture for the w32-upper-dlls probe: every staged
 * reused Wine DLL (config/wine-upper.json, qualified) loaded in one Win32
 * process through the production loader (DllMain attach), one census entry
 * exercised per DLL through GetProcAddress, every module freed. Builtins stay
 * resident, so DllMain detach runs when the process exits. Results go to the
 * controller.h block that the kernel reads; nothing is printed. The same
 * source runs on the H1 Linux adapter (W32_UPPER_HOST) for the dry-run of the
 * suite case.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "exercise.h"

typedef uint32_t UINT;
typedef int32_t HRESULT;
typedef struct { uint32_t cbSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformID; } DLLVERSIONINFO;
typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } UUID;
typedef struct { int erfOper, erfType; BOOL fError; } ERF;
typedef struct { uint32_t lo; int32_t hi; } ULARGE;
#define CDECL
/* COM: vtable slot n of an interface pointer (IUnknown: QueryInterface 0, AddRef 1, Release 2). */
#define SLOT(p, n) ((*(void ***)(p))[n])
#define RELEASE(p) ((ULONG(W32API *)(void *))SLOT(p, 2))(p)
/* name = GetProcAddress(module, "name") typed as ret (conv *)args. */
#define PROC(name, ret, conv, args) ret (conv *name) args = (ret (conv *) args)e->proc(m, #name)
/* A step counts before it is checked: entry_error is the first failed step. */
#define STEP(cond) do { (*step)++; if (!(cond)) return 0; } while (0)

/* cabinet FDI callbacks (cdecl): a bump allocator inside the fixture. */
static uint8_t fdi_arena[16384];
static uint32_t fdi_used, fdi_allocs, fdi_frees;
static void *fdi_alloc(ULONG bytes)
{
    bytes = (bytes + 15u) & ~15u;
    if (bytes > sizeof(fdi_arena) - fdi_used)
        return 0;
    void *p = fdi_arena + fdi_used;
    fdi_used += bytes;
    fdi_allocs++;
    return p;
}
static void fdi_free(void *p) { (void)p; fdi_frees++; }

static int same(const char *a, const char *b)
{
    for (;; a++, b++) {
        if (*a != *b)
            return 0;
        if (!*a)
            return 1;
    }
}

/* One census entry per staged DLL; a staged DLL without an exercise fails
 * closed (step 0). Pure calls only: no file, window or network service, so
 * the guest and the H1 adapter run the same path. */
static int exercise(const char *name, void *m, const struct w32_upper_env *e, uint32_t *step)
{
    *step = 0;
    if (same(name, "advpack.dll")) {
        PROC(NeedRebootInit, DWORD, W32API, (void));
        PROC(NeedReboot, BOOL, W32API, (DWORD));
        STEP(NeedRebootInit && NeedReboot);
        STEP(!NeedReboot(NeedRebootInit()));
        return 1;
    }
    if (same(name, "cabinet.dll")) {
        PROC(FDICreate, void *, CDECL, (void *, void *, void *, void *, void *, void *, void *, int, ERF *));
        PROC(FDIDestroy, BOOL, CDECL, (void *));
        STEP(FDICreate && FDIDestroy);
        ERF erf = {0, 0, 0};
        fdi_used = fdi_allocs = fdi_frees = 0;
        void *fdi = FDICreate((void *)fdi_alloc, (void *)fdi_free, 0, 0, 0, 0, 0, -1 /* cpuUNKNOWN */, &erf);
        STEP(fdi && !erf.fError && fdi_allocs == 1);
        STEP(FDIDestroy(fdi) && fdi_frees == 1);
        return 1;
    }
    if (same(name, "combase.dll")) {
        PROC(CoTaskMemAlloc, void *, W32API, (ULONG));
        PROC(CoTaskMemFree, void, W32API, (void *));
        STEP(CoTaskMemAlloc && CoTaskMemFree);
        uint8_t *p = CoTaskMemAlloc(32);
        STEP(p);
        p[0] = 0x57, p[31] = 0x4f;
        CoTaskMemFree(p);
        return 1;
    }
    if (same(name, "comctl32.dll")) {
        PROC(InitCommonControls, void, W32API, (void));
        PROC(ImageList_Create, void *, W32API, (int, int, UINT, int, int));
        PROC(ImageList_GetImageCount, int, W32API, (void *));
        PROC(ImageList_Destroy, BOOL, W32API, (void *));
        STEP(InitCommonControls && ImageList_Create && ImageList_GetImageCount && ImageList_Destroy);
        InitCommonControls();
        void *list = ImageList_Create(16, 16, 0x18 /* ILC_COLOR24 */, 1, 1);
        STEP(list);
        STEP(ImageList_GetImageCount(list) == 0);
        STEP(ImageList_Destroy(list));
        return 1;
    }
    if (same(name, "comdlg32.dll")) {
        PROC(CommDlgExtendedError, DWORD, W32API, (void));
        STEP(CommDlgExtendedError);
        STEP(CommDlgExtendedError() == 0);
        return 1;
    }
    if (same(name, "coml2.dll")) {
        PROC(CreateILockBytesOnHGlobal, HRESULT, W32API, (void *, BOOL, void **));
        PROC(GetHGlobalFromILockBytes, HRESULT, W32API, (void *, void **));
        STEP(CreateILockBytesOnHGlobal && GetHGlobalFromILockBytes);
        void *bytes = 0, *global = 0;
        STEP(CreateILockBytesOnHGlobal(0, 1, &bytes) == 0 && bytes);
        STEP(GetHGlobalFromILockBytes(bytes, &global) == 0 && global);
        STEP(RELEASE(bytes) == 0);
        return 1;
    }
    if (same(name, "msvcrt.dll")) {
        PROC(malloc, void *, CDECL, (uint32_t));
        PROC(free, void, CDECL, (void *));
        PROC(strlen, uint32_t, CDECL, (const char *));
        PROC(_itoa, char *, CDECL, (int, char *, int));
        STEP(malloc && free && strlen && _itoa);
        char *p = malloc(64);
        STEP(p);
        STEP(strlen("Ciuki") == 5);
        STEP(_itoa(1998, p, 10) == p && same(p, "1998"));
        free(p);
        return 1;
    }
    if (same(name, "ole32.dll")) {
        /* A compound file in memory: ILockBytes -> IStorage -> IStream. No
         * apartment, window or server is involved. */
        PROC(CreateILockBytesOnHGlobal, HRESULT, W32API, (void *, BOOL, void **));
        PROC(StgCreateDocfileOnILockBytes, HRESULT, W32API, (void *, DWORD, DWORD, void **));
        STEP(CreateILockBytesOnHGlobal && StgCreateDocfileOnILockBytes);
        static const WCHAR stream_name[] = {'C', 'I', 'U', 'K', 'I', 0};
        void *bytes = 0, *storage = 0, *stream = 0;
        STEP(CreateILockBytesOnHGlobal(0, 1, &bytes) == 0 && bytes);
        /* STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE */
        STEP(StgCreateDocfileOnILockBytes(bytes, 0x1012, 0, &storage) == 0 && storage);
        /* IStorage::CreateStream is slot 3: STGM_READWRITE | STGM_SHARE_EXCLUSIVE. */
        STEP(((HRESULT(W32API *)(void *, const WCHAR *, DWORD, DWORD, DWORD, void **))SLOT(storage, 3))(
                 storage, stream_name, 0x12, 0, 0, &stream) == 0 && stream);
        /* IStream::Write is slot 4. */
        ULONG written = 0;
        STEP(((HRESULT(W32API *)(void *, const void *, ULONG, ULONG *))SLOT(stream, 4))(
                 stream, "Ciuki", 5, &written) == 0 && written == 5);
        STEP(RELEASE(stream) == 0);
        STEP(RELEASE(storage) == 0);
        STEP(RELEASE(bytes) == 0);
        return 1;
    }
    if (same(name, "oleaut32.dll")) {
        PROC(SysAllocString, WCHAR *, W32API, (const WCHAR *));
        PROC(SysStringLen, UINT, W32API, (const WCHAR *));
        PROC(SysFreeString, void, W32API, (WCHAR *));
        STEP(SysAllocString && SysStringLen && SysFreeString);
        static const WCHAR ciuki[] = {'C', 'i', 'u', 'k', 'i', 0};
        WCHAR *b = SysAllocString(ciuki);
        STEP(b && b != ciuki);
        STEP(SysStringLen(b) == 5 && b[4] == 'i' && !b[5]);
        SysFreeString(b);
        return 1;
    }
    if (same(name, "riched20.dll")) {
        PROC(REExtendedRegisterClass, int32_t, W32API, (void));
        STEP(REExtendedRegisterClass);
        /* 1: REListBox20W registered, 2: REComboBox20W registered. */
        STEP((REExtendedRegisterClass() & 3) == 3);
        return 1;
    }
    if (same(name, "riched32.dll")) {
        PROC(DllGetVersion, HRESULT, W32API, (DLLVERSIONINFO *));
        STEP(DllGetVersion);
        DLLVERSIONINFO v = {sizeof(v), 0, 0, 0, 0};
        STEP(DllGetVersion(&v) == 0 && v.dwMajorVersion >= 4);
        return 1;
    }
    if (same(name, "rpcrt4.dll")) {
        PROC(UuidCreate, LONG, W32API, (UUID *));
        STEP(UuidCreate);
        UUID u = {0, 0, 0, {0}};
        LONG status = UuidCreate(&u);
        STEP(status == 0 /* RPC_S_OK */ || status == 1824 /* RPC_S_UUID_LOCAL_ONLY */);
        STEP((u.Data3 & 0xf000) == 0x4000 && (u.Data4[0] & 0xc0) == 0x80);
        return 1;
    }
    if (same(name, "setupapi.dll")) {
        /* Patch 0003: every SetupDi export fails with ERROR_CALL_NOT_IMPLEMENTED. */
        PROC(SetupDiGetClassDevsA, void *, W32API, (const void *, const char *, void *, DWORD));
        STEP(SetupDiGetClassDevsA);
        void *set = SetupDiGetClassDevsA(0, 0, 0, 0);
        STEP(set == (void *)(uintptr_t)0xffffffffu && e->last_error() == 120);
        return 1;
    }
    if (same(name, "shcore.dll")) {
        PROC(SHCreateMemStream, void *, W32API, (const void *, UINT));
        PROC(IStream_Size, HRESULT, W32API, (void *, ULARGE *));
        STEP(SHCreateMemStream && IStream_Size);
        void *stream = SHCreateMemStream("Ciuki", 5);
        STEP(stream);
        ULARGE size = {0, 0};
        STEP(IStream_Size(stream, &size) == 0 && size.lo == 5 && !size.hi);
        STEP(RELEASE(stream) == 0);
        return 1;
    }
    if (same(name, "shell32.dll")) {
        /* Forwarded to SHCORE by the reused SHELL32: the loader resolves it. */
        PROC(CommandLineToArgvW, WCHAR **, W32API, (const WCHAR *, int *));
        STEP(CommandLineToArgvW);
        static const WCHAR line[] = {'c', 'i', 'u', 'k', 'i', ' ', 'w', 'o', 'o', 'f', 0};
        int count = 0;
        WCHAR **argv = CommandLineToArgvW(line, &count);
        STEP(argv && count == 2 && argv[1] && argv[1][0] == 'w' && argv[1][3] == 'f' && !argv[1][4]);
        STEP(e->local_free(argv) == 0);
        return 1;
    }
    if (same(name, "shlwapi.dll")) {
        /* Compiled in from kernelbase by patch 0008. */
        PROC(StrStrIA, char *, W32API, (const char *, const char *));
        STEP(StrStrIA);
        const char *text = "Classic computing";
        STEP(StrStrIA(text, "COMPUTING") == text + 8);
        STEP(!StrStrIA(text, "woof"));
        return 1;
    }
    return 0;
}

static void copy_name(char *out, const char *name)
{
    unsigned i = 0;
    for (; i < 15 && name[i]; i++)
        out[i] = name[i];
    for (; i < 16; i++)
        out[i] = 0;
}

void w32_upper_run(const struct w32_upper_env *e, const struct w32_upper_entry *table, uint32_t count,
                   struct w32_upper_result *r)
{
    void *modules[W32_UPPER_MAX];
    if (count > W32_UPPER_MAX)
        count = W32_UPPER_MAX;
    r->dlls = count;
#define FIRST(i, kind) do { if (!r->first_error) r->first_error = 10u * ((i) + 1u) + (kind); } while (0)
    for (uint32_t i = 0; i < count; i++) {
        struct w32_upper_dll *d = &r->dll[i];
        copy_name(d->name, table[i].name);
        d->base = table[i].base;
        modules[i] = e->load(table[i].name);
        if (!modules[i]) {
            d->load_error = e->last_error();
            r->load_errors++;
            FIRST(i, W32_UPPER_ERR_LOAD);
            continue;
        }
        d->loaded_base = (uint32_t)(uintptr_t)modules[i];
        r->loaded++;
        if (d->loaded_base != d->base) {
            r->base_errors++;
            FIRST(i, W32_UPPER_ERR_BASE);
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        struct w32_upper_dll *d = &r->dll[i];
        if (!modules[i])
            continue;
        uint32_t step = 0;
        d->entry = exercise(table[i].name, modules[i], e, &step) ? 1u : 0u;
        d->entry_error = d->entry ? 0 : step;
        if (d->entry)
            r->entries++;
        else {
            r->entry_errors++;
            FIRST(i, W32_UPPER_ERR_ENTRY);
        }
    }
    /* FreeLibrary balances every LoadLibraryA. Builtin modules (the whole
     * store, the staged Wine DLLs included) are never unmapped by the loader:
     * they stay resident and run DllMain(DLL_PROCESS_DETACH) when the process
     * exits, which the kernel observes as the fixture's exit status. */
    for (uint32_t i = count; i-- > 0;) {
        struct w32_upper_dll *d = &r->dll[i];
        if (!modules[i])
            continue;
        if (e->free(modules[i])) {
            d->freed = 1;
            r->freed++;
        } else {
            r->free_errors++;
            FIRST(i, W32_UPPER_ERR_FREE);
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        struct w32_upper_dll *d = &r->dll[i];
        d->resident = e->handle(table[i].name) ? 1u : 0u;
        r->resident += d->resident;
    }
    r->errors = r->load_errors + r->base_errors + r->entry_errors + r->free_errors + (count ? 0u : 1u);
#undef FIRST
}

#ifndef W32_UPPER_HOST
/* Guest entry: no CRT. The kernel reads the block, then sets release. */
#include "upper_table.h"
void *W32API LoadLibraryA(const char *);
void *W32API GetProcAddress(void *, const char *);
BOOL W32API FreeLibrary(void *);
void *W32API GetModuleHandleA(const char *);
void *W32API LocalFree(void *);
W32_NORETURN void W32API ExitProcess(DWORD);
void W32API entry(void)
{
    struct w32_upper_result *r = VirtualAlloc((void *)(uintptr_t)W32_UPPER_RESULT_VA, 4096, 0x3000u, 4u);
    if (!r)
        ExitProcess(2);
    r->magic = W32_UPPER_MAGIC;
    r->version = W32_UPPER_VERSION;
    const struct w32_upper_env env = {LoadLibraryA, GetProcAddress, FreeLibrary, GetModuleHandleA,
                                      GetLastError, LocalFree};
    w32_upper_run(&env, W32_UPPER_TABLE, W32_UPPER_TABLE_COUNT, r);
    __atomic_store_n(&r->done, 1u, __ATOMIC_RELEASE);
    for (UINT i = 0; i < 3000 && !__atomic_load_n(&r->release, __ATOMIC_ACQUIRE); i++)
        Sleep(10);
    ExitProcess(r->errors ? 1 : 0);
}
#endif
