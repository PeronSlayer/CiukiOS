/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include "linux.h"
#ifdef W32_HOST_MODEL
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#endif
#define REQUIRE(c)                                                                                 \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            host_print("FAIL line ");                                                              \
            host_number(__LINE__);                                                                 \
            host_print("\n");                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static struct w32_loader loader;
/* Randomised conformance: contents survive neighbours' traffic, reallocation
 * preserves prefixes, structure validates, large blocks return segments. */
static ULONG lcg_state = 0x1234567u;
static ULONG lcg(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return lcg_state >> 8;
}
static int heap_stress(void) {
    enum { SLOTS = 512 };
    static uint8_t *slot[SLOTS];
    static ULONG length[SLOTS];
    void *h = RtlCreateHeap(HEAP_GROWABLE, 0, 0, 0, 0, 0);
    if (!h)
        return 0;
    ULONG start = host.allocations;
    for (ULONG op = 0; op < 20000; op++) {
        ULONG i = lcg() % SLOTS, kind = lcg() % 8;
        ULONG n = kind == 7 ? 0x80000 + lcg() % 65536 : lcg() % (kind < 4 ? 64 : 4096);
        if (slot[i]) {
            for (ULONG k = 0; k < length[i]; k++)
                if (slot[i][k] != (uint8_t)(i + k))
                    return 0;
            if (RtlSizeHeap(h, 0, slot[i]) != length[i])
                return 0;
            if (kind & 1) {
                if (!RtlFreeHeap(h, 0, slot[i]))
                    return 0;
                slot[i] = 0;
                continue;
            }
            uint8_t *q = RtlReAllocateHeap(h, 0, slot[i], n);
            if (!q)
                return 0;
            for (ULONG k = 0; k < length[i] && k < n; k++)
                if (q[k] != (uint8_t)(i + k))
                    return 0;
            slot[i] = q;
        } else if (!(slot[i] = RtlAllocateHeap(h, 0, n)) || ((uintptr_t)slot[i] & 7))
            return 0;
        length[i] = n;
        for (ULONG k = 0; k < n; k++)
            slot[i][k] = (uint8_t)(i + k);
        if (!(op % 997) && !RtlValidateHeap(h, 0, 0))
            return 0;
    }
    if (!RtlValidateHeap(h, 0, 0))
        return 0;
    for (ULONG i = 0; i < SLOTS; i++)
        if (slot[i] && !RtlFreeHeap(h, 0, slot[i]))
            return 0;
        else
            slot[i] = 0;
    /* Everything free: only the initial segment remains. */
    if (!RtlValidateHeap(h, 0, 0) || host.allocations != start)
        return 0;
    return !RtlDestroyHeap(h);
}
static int heaps(void) {
    ULONG baseline = host.allocations;
    void *h = RtlCreateHeap(HEAP_GROWABLE, 0, 0, 0, 0, 0);
    REQUIRE(h);
    uint8_t *a = RtlAllocateHeap(h, HEAP_ZERO_MEMORY, 17);
    REQUIRE(a && !((uintptr_t)a & 7));
    for (ULONG i = 0; i < 17; i++)
        REQUIRE(!a[i]);
    REQUIRE(RtlSizeHeap(h, 0, a) == 17);
    a[0] = 0x72;
    uint8_t *b = RtlReAllocateHeap(h, HEAP_ZERO_MEMORY, a, 170);
    REQUIRE(b && b[0] == 0x72);
    for (ULONG i = 17; i < 170; i++)
        REQUIRE(!b[i]);
    REQUIRE(RtlValidateHeap(h, 0, b));
    REQUIRE(RtlSizeHeap(h, 0, (void *)1) == 0xffffffffu);
    struct w32_heap_entry e = {0};
    REQUIRE(!RtlWalkHeap(h, &e) && e.Flags == 2);
    ULONG busy = 0, freed = 0, walked = 0;
    while (!RtlWalkHeap(h, &e) && walked++ < 1000) {
        if (e.Flags == 1) {
            busy++;
            REQUIRE(e.DataAddress == b && e.DataSize == 170);
        } else if (!e.Flags)
            freed++;
    }
    REQUIRE(busy == 1 && freed >= 1 && walked < 1000);
    REQUIRE(RtlWalkHeap(h, &e) == (NTSTATUS)0x8000001a);
    /* Interior and misaligned pointers are not blocks. */
    REQUIRE(!RtlFreeHeap(h, 0, b + 8) && !RtlFreeHeap(h, 0, b + 1));
    REQUIRE(RtlSizeHeap(h, 0, b) == 170);
    REQUIRE(RtlFreeHeap(h, 0, 0));
    REQUIRE(RtlFreeHeap(h, 0, b));
    REQUIRE(b[0] == 0x72);
    REQUIRE(!RtlFreeHeap(h, 0, b));
    REQUIRE(!RtlReAllocateHeap(h, 0, 0, 32));
    REQUIRE(RtlValidateHeap(h, 0, 0));
    void *zero = RtlAllocateHeap(h, 0, 0);
    REQUIRE(zero && RtlSizeHeap(h, 0, zero) == 0);
    REQUIRE(RtlFreeHeap(h, 0, zero));
    REQUIRE(!RtlDestroyHeap(h));
    h = RtlCreateHeap(0, 0, 4096, 4096, 0, 0);
    REQUIRE(h);
    a = RtlAllocateHeap(h, 0, 1000);
    REQUIRE(a);
    REQUIRE(!RtlAllocateHeap(h, 0, 5000));
    REQUIRE(!RtlReAllocateHeap(h, HEAP_REALLOC_IN_PLACE_ONLY, a, 5000));
    REQUIRE(RtlSizeHeap(h, 0, a) == 1000);
    REQUIRE(!RtlDestroyHeap(h));
    h = RtlCreateHeap(HEAP_GROWABLE | HEAP_TAIL_CHECKING_ENABLED | HEAP_FREE_CHECKING_ENABLED, 0, 0,
                      0, 0, 0);
    REQUIRE(h);
    a = RtlAllocateHeap(h, 0, 16);
    REQUIRE(a && w32_u32(a) == 0xbaadf00d);
    a[16] = 0;
    REQUIRE(!RtlValidateHeap(h, 0, a));
    REQUIRE(!RtlFreeHeap(h, 0, a));
    a[16] = 0xab;
    REQUIRE(RtlFreeHeap(h, 0, a) && w32_u32(a) == 0xfeeefeee);
    REQUIRE(!RtlDestroyHeap(h));
    h = RtlCreateHeap(HEAP_GROWABLE | HEAP_NO_SERIALIZE, 0, 0, 0, 0, 0);
    REQUIRE(h);
    REQUIRE(!RtlAllocateHeap(h, 0, 0xffffffffu));
    host.fail_after = 0;
    REQUIRE(!RtlAllocateHeap(h, 0, 100000));
    host.fail_after = -1;
    REQUIRE(RtlValidateHeap(h, 0, 0));
    REQUIRE(!RtlDestroyHeap(h));
    REQUIRE(host.allocations == baseline);
    REQUIRE(heap_stress());
    REQUIRE(host.allocations == baseline);
    host_print("PASS heaps\n");
    return 0;
}
static RTL_CRITICAL_SECTION shared_cs;
#ifdef W32_HOST_MODEL
static volatile ULONG entered, release_child;
static void *contender(void *arg) {
    (void)arg;
    RtlEnterCriticalSection(&shared_cs);
    __atomic_store_n(&entered, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&release_child, __ATOMIC_ACQUIRE))
        sched_yield();
    RtlLeaveCriticalSection(&shared_cs);
    return 0;
}
#endif
static int critical(void) {
    REQUIRE(!RtlInitializeCriticalSection(&shared_cs));
    REQUIRE(shared_cs.LockCount == -1);
    REQUIRE(!RtlEnterCriticalSection(&shared_cs));
    REQUIRE(RtlTryEnterCriticalSection(&shared_cs));
    REQUIRE(shared_cs.RecursionCount == 2 && shared_cs.LockCount == 1);
    REQUIRE(!RtlLeaveCriticalSection(&shared_cs));
    REQUIRE(!RtlLeaveCriticalSection(&shared_cs));
    REQUIRE(shared_cs.LockCount == -1 && !shared_cs.OwningThread);
#ifdef W32_HOST_MODEL
    REQUIRE(!RtlEnterCriticalSection(&shared_cs));
    pthread_t t;
    REQUIRE(!pthread_create(&t, 0, contender, 0));
    while (__atomic_load_n(&shared_cs.LockCount, __ATOMIC_RELAXED) < 1)
        sched_yield();
    REQUIRE(!__atomic_load_n(&entered, __ATOMIC_ACQUIRE));
    REQUIRE(!RtlLeaveCriticalSection(&shared_cs));
    while (!__atomic_load_n(&entered, __ATOMIC_ACQUIRE))
        sched_yield();
    REQUIRE(!RtlTryEnterCriticalSection(&shared_cs));
    __atomic_store_n(&release_child, 1, __ATOMIC_RELEASE);
    REQUIRE(!pthread_join(t, 0));
    REQUIRE(shared_cs.LockCount == -1);
#endif
    REQUIRE(!RtlDeleteCriticalSection(&shared_cs));
    host_print("PASS critical\n");
    return 0;
}
static int strings(void) {
    ANSI_STRING a;
    UNICODE_STRING u;
    RtlInitAnsiString(&a, "a\x80\xe9");
    REQUIRE(!RtlAnsiStringToUnicodeString(&u, &a, 1));
    REQUIRE(u.Length == 6 && u.Buffer[1] == 0x20ac);
    ANSI_STRING back;
    REQUIRE(!RtlUnicodeStringToAnsiString(&back, &u, 1));
    REQUIRE(!w32_strcmp(back.Buffer, a.Buffer));
    UNICODE_STRING upper;
    REQUIRE(!RtlUpcaseUnicodeString(&upper, &u, 1));
    REQUIRE(upper.Buffer[0] == 'A' && upper.Buffer[2] == 0xc9);
    REQUIRE(RtlEqualUnicodeString(&upper, &u, 1));
    REQUIRE(!RtlEqualUnicodeString(&upper, &u, 0));
    RtlFreeAnsiString(&back);
    RtlFreeUnicodeString(&upper);
    RtlFreeUnicodeString(&u);
    WCHAR path[] = {'C', ':', '\\', 'f', 'o', 'o', '\\', '.', '.', '\\', 'b', 'a', 'r', 0},
          out[128], *part;
    REQUIRE(RtlGetFullPathName_U(path, sizeof(out), out, &part) == 12);
    REQUIRE(part == out + 3 && part[0] == 'b');
    REQUIRE(RtlDosPathNameToNtPathName_U(path, &u, &part, 0));
    REQUIRE(u.Buffer[0] == '\\' && u.Buffer[1] == '?' && u.Buffer[4] == 'C');
    RtlFreeUnicodeString(&u);
    WCHAR empty[1] = {0};
    RtlInitUnicodeString(&u, empty);
    REQUIRE(!u.Length && u.MaximumLength == 2);
    REQUIRE(RtlNtStatusToDosError(STATUS_DLL_NOT_FOUND) == 126);
    host_print("PASS strings\n");
    return 0;
}
static ULONG seh_calls, seh_mode, nested_seen;
static LONG W32API top_handler(struct w32_exception_pointers *p) {
    p->ContextRecord->Eax = 77;
    return -1;
}
static LONG handler(struct w32_exception *r, struct w32_seh *f, struct w32_context *c,
                    struct w32_seh **dispatch) {
    seh_calls++;
    if (seh_mode == 1)
        return ExceptionContinueExecution;
    if (seh_mode == 2)
        return 99;
    if (seh_mode == 3) {
        *dispatch = f->Prev;
        return ExceptionNestedException;
    }
    if (seh_mode == 4 && f->Prev != (void *)~(uintptr_t)0) {
        *dispatch = f->Prev;
        return ExceptionCollidedUnwind;
    }
    if (seh_mode == 5 && r->ExceptionCode == 1) {
        struct w32_exception inner = {0};
        inner.ExceptionCode = 2;
        NTSTATUS s = w32_dispatch_exception(&inner, c);
        nested_seen =
            s == STATUS_UNHANDLED_EXCEPTION && (inner.ExceptionFlags & EXCEPTION_NESTED_CALL) == 0;
    }
    return ExceptionContinueSearch;
}
static int seh(void) {
    struct w32_seh frames[3];
    struct w32_context c = {0};
    struct w32_exception r = {0};
    for (ULONG i = 0; i < 3; i++) {
        frames[i].Prev = i == 2 ? (void *)~(uintptr_t)0 : &frames[i + 1];
        frames[i].Handler = handler;
    }
    struct w32_teb *t = NtCurrentTeb();
    void *lo = t->Tib.StackLimit, *hi = t->Tib.StackBase;
    /* Include the protected-call guards on the real host C stack. */
    t->Tib.StackLimit = (void *)((uintptr_t)&frames - 65536);
    t->Tib.StackBase = (void *)((uintptr_t)&frames + 65536);
    t->Tib.ExceptionList = frames;
    seh_mode = 0;
    seh_calls = 0;
    REQUIRE(w32_dispatch_exception(&r, &c) == STATUS_UNHANDLED_EXCEPTION && seh_calls == 3);
    seh_mode = 1;
    REQUIRE(!w32_dispatch_exception(&r, &c));
    r.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    REQUIRE(w32_dispatch_exception(&r, &c) == STATUS_NONCONTINUABLE_EXCEPTION);
    r.ExceptionFlags = 0;
    seh_mode = 2;
    REQUIRE(w32_dispatch_exception(&r, &c) == STATUS_INVALID_DISPOSITION);
    seh_mode = 0;
    frames[1].Prev = frames;
    REQUIRE(w32_dispatch_exception(&r, &c) == STATUS_BAD_STACK);
    frames[1].Prev = &frames[2];
    r.ExceptionFlags = 0;
    seh_calls = 0;
    REQUIRE(!w32_unwind(&frames[2], &r, &c) && seh_calls == 2 &&
            t->Tib.ExceptionList == &frames[2]);
    REQUIRE(r.ExceptionFlags & EXCEPTION_UNWINDING);
    t->Tib.ExceptionList = frames;
    r.ExceptionFlags = 0;
    seh_mode = 4;
    REQUIRE(!w32_unwind(&frames[1], &r, &c) && t->Tib.ExceptionList == &frames[1]);
    t->Tib.ExceptionList = frames;
    seh_mode = 4;
    seh_calls = 0;
    REQUIRE(!w32_unwind(0, &r, &c) && seh_calls == 2 &&
            t->Tib.ExceptionList == (void *)~(uintptr_t)0);
    t->Tib.ExceptionList = frames;
    seh_mode = 5;
    r.ExceptionCode = 1;
    r.ExceptionFlags = 0;
    nested_seen = 0;
    REQUIRE(w32_dispatch_exception(&r, &c) == STATUS_UNHANDLED_EXCEPTION && nested_seen);
    struct w32_exception_pointers pointers = {&r, &c};
    REQUIRE(!SetUnhandledExceptionFilter(top_handler));
    REQUIRE(UnhandledExceptionFilter(&pointers) == -1 && c.Eax == 77);
    t->CoreFilterDepth = 1;
    REQUIRE(!UnhandledExceptionFilter(&pointers));
    t->CoreFilterDepth = 0;
    REQUIRE(SetUnhandledExceptionFilter(0) == top_handler);
    t->Tib.ExceptionList = (void *)~(uintptr_t)0;
    t->Tib.StackLimit = lo;
    t->Tib.StackBase = hi;
    host_print("PASS seh-state-machine\n");
    return 0;
}
static struct {
    ULONG base, reason;
    int dll;
} callback_trace[256];
static ULONG trace_count;
static int reentrant_query;
static int quiet_invoke(void *opaque, void *proc, void *module, ULONG reason, int dll) {
    (void)proc;
    (void)module;
    struct host_state *s = opaque;
    s->invocations++;
    if (trace_count < 256) {
        callback_trace[trace_count].base = (ULONG)(uintptr_t)module;
        callback_trace[trace_count].reason = reason;
        callback_trace[trace_count++].dll = dll;
    }
    /* DllMain must observe its module and recursive loader lock. */
    if (dll && reason == 1) {
        if (!RtlTryEnterCriticalSection(&loader.lock))
            return 0;
        RtlLeaveCriticalSection(&loader.lock);
        if (reentrant_query)
            for (struct w32_module *m = loader.modules; m; m = m->next)
                if (m->pe.image == module) {
                    if (!w32_ldr_module(&loader, m->name) ||
                        w32_ldr_proc(&loader, m, "AbsentInsideDllMain", 0, 0xffffffffu))
                        return 0;
                    if (!loader.module_count)
                        return 0;
                }
    }
    if (s->fail_attach == 2 && dll && reason == 1)
        for (struct w32_module *m = loader.modules; m; m = m->next)
            if (m->pe.image == module && !w32_strcmp(m->name, "consumer-dll.dll"))
                return 0;
    return !(s->fail_attach == 1 && dll && reason == 1);
}
static int load_file(const char *directory, const char *name, ULONG flags, struct w32_module **m) {
    char path[W32_PATH_MAX];
    ULONG n = w32_strlen(directory);
    w32_copy(path, directory, n);
    path[n++] = '/';
    w32_copy_string(path + n, sizeof(path) - n, name);
    return w32_ldr_load(&loader, path, flags, m);
}
static int loaders(const char *directory) {
    host.services.invoke = quiet_invoke;
    ULONG baseline = host.allocations;
    w32_ldr_init(&loader, &host.services, &host.peb);
    w32_ldr_bind(&loader);
    w32_copy_string(loader.exe_directory, sizeof(loader.exe_directory), directory);
    REQUIRE(w32_ldr_thread_attach(&loader, &host.teb));
    struct w32_module *m, *again;
    REQUIRE(load_file(directory, "consumer.exe", 0, &m));
    REQUIRE(loader.module_count == 2);
    REQUIRE(w32_ldr_load(&loader, "provider.dll", 0, &again));
    REQUIRE(again->references == 1);
    void *fn = w32_ldr_proc(&loader, again, "FixtureValue", 0, 0);
    REQUIRE(fn);
    REQUIRE(w32_ldr_proc(&loader, again, 0, 11, 0));
    REQUIRE(!w32_ldr_proc(&loader, again, 0, 8, 0));
    REQUIRE(loader.error.code == 127);
    REQUIRE(w32_ldr_free(&loader, m));
    REQUIRE(loader.module_count == 1);
    REQUIRE(w32_ldr_free(&loader, again));
    REQUIRE(loader.module_count == 0);
    trace_count = 0;
    reentrant_query = 1;
    REQUIRE(load_file(directory, "consumer-dll.dll", 0, &m));
    reentrant_query = 0;
    REQUIRE(!loader.error.code);
    ULONG consumer_base = m->pe.base, provider_base = m->dependencies[0]->pe.base;
    REQUIRE(trace_count == 2 && callback_trace[0].base == provider_base &&
            callback_trace[1].base == consumer_base);
    REQUIRE(callback_trace[0].reason == 1 && callback_trace[1].reason == 1);
    REQUIRE(w32_ldr_free(&loader, m));
    REQUIRE(trace_count == 4 && callback_trace[2].base == consumer_base &&
            callback_trace[3].base == provider_base);
    REQUIRE(callback_trace[2].reason == 0 && callback_trace[3].reason == 0);
    host.fail_attach = 2;
    trace_count = 0;
    REQUIRE(!load_file(directory, "consumer-dll.dll", 0, &m));
    host.fail_attach = 0;
    REQUIRE(loader.error.code == 1114 && !loader.module_count && trace_count == 4);
    REQUIRE(callback_trace[0].reason == 1 && callback_trace[1].reason == 1 &&
            callback_trace[2].reason == 0 && callback_trace[3].reason == 0);
    REQUIRE(callback_trace[0].base == callback_trace[3].base &&
            callback_trace[1].base == callback_trace[2].base);
    host.fail_attach = 1;
    REQUIRE(!load_file(directory, "consumer-dll.dll", 0, &m));
    REQUIRE(loader.error.code == 1114 && loader.module_count == 0);
    host.fail_attach = 0;
    ULONG invokes = host.invocations;
    REQUIRE(load_file(directory, "resource-named.exe", W32_LOAD_DATAFILE, &m));
    REQUIRE(host.invocations == invokes);
    static WCHAR hello[] = {'H', 'E', 'L', 'L', 'O', 0};
    const void *data;
    ULONG n;
    REQUIRE(w32_pe_resource(&m->pe, (struct w32_resource_key){10, 0},
                            (struct w32_resource_key){0, hello}, 1033, &data, &n));
    REQUIRE(n == 12);
    REQUIRE(!w32_pe_resource(&m->pe, (struct w32_resource_key){10, 0},
                             (struct w32_resource_key){0, hello}, 1040, &data, &n));
    REQUIRE(w32_ldr_free(&loader, m));
    REQUIRE(load_file(directory, "tls.exe", 0, &m));
    REQUIRE(m->tls_index < W32_MAX_TLS);
    uint8_t *tls = host.teb.ThreadLocalStoragePointer[m->tls_index];
    REQUIRE(tls && tls[0] == 1 && tls[3] == 4 && !tls[4]);
    struct w32_teb other = host.teb;
    REQUIRE(w32_ldr_thread_attach(&loader, &other));
    REQUIRE(other.ThreadLocalStoragePointer[m->tls_index] != tls);
    REQUIRE(((uint8_t *)other.ThreadLocalStoragePointer[m->tls_index])[0] == 1);
    w32_ldr_thread_detach(&loader, &other);
    REQUIRE(w32_ldr_free(&loader, m));
    REQUIRE(w32_ldr_thread_attach(&loader, &other));
    REQUIRE(load_file(directory, "tls-dll.dll", 0, &m));
    REQUIRE(other.ThreadLocalStoragePointer[m->tls_index] &&
            host.teb.ThreadLocalStoragePointer[m->tls_index]);
    REQUIRE(w32_ldr_free(&loader, m));
    w32_ldr_thread_detach(&loader, &other);
    REQUIRE(load_file(directory, "reloc-highlow.exe", 0, &m));
    void *collision = host.services.map(&host, 0x00400000, 0x10000, 1);
    REQUIRE(!collision);
    REQUIRE(load_file(directory, "reloc-low.exe", 0, &again));
    REQUIRE(again->pe.base != 0x400000);
    REQUIRE(w32_ldr_free(&loader, again));
    REQUIRE(!load_file(directory, "no-reloc-dll.dll", 0, &again));
    REQUIRE(loader.error.code == 193);
    REQUIRE(w32_ldr_free(&loader, m));
    for (LONG fail = 0; fail < 6; fail++) {
        host.fail_after = fail;
        int ok = load_file(directory, "consumer.exe", 0, &m);
        host.fail_after = -1;
        if (ok)
            REQUIRE(w32_ldr_free(&loader, m));
        REQUIRE(loader.module_count == 0);
    }
    w32_ldr_shutdown(&loader);
    REQUIRE(!host.mappings && host.allocations == baseline);
    host_print("PASS loader-lifecycle\n");
    return 0;
}
static int relocations(const char *directory) {
    const char *names[] = {"reloc-highlow.exe", "reloc-high.exe", "reloc-low.exe",
                           "reloc-highadj.exe"};
    const ULONG expected[] = {0x10414000, 0x3001, 0x4000, 0x3001};
    for (ULONG i = 0; i < 4; i++) {
        char path[W32_PATH_MAX];
        ULONG n = w32_strlen(directory);
        w32_copy(path, directory, n);
        path[n++] = '/';
        w32_copy_string(path + n, sizeof(path) - n, names[i]);
        struct w32_file f;
        struct w32_pe p;
        struct w32_error e = {0};
        REQUIRE(host.services.open(&host, path, &f));
        REQUIRE(w32_pe_parse(&p, f.bytes, f.size, &e));
        void *image = host.services.alloc(&host, p.size);
        REQUIRE(image);
        REQUIRE(w32_pe_copy_image(&p, image) && w32_pe_validate(&p));
        REQUIRE(w32_pe_relocate(&p, 0x10412000));
        REQUIRE((i == 0 ? w32_u32(p.image + 0x1001) : w32_u16(p.image + 0x1001)) == expected[i]);
        if (i == 0 || i == 2) {
            REQUIRE(w32_pe_relocate(&p, 0x400000));
            REQUIRE((i == 0 ? w32_u32(p.image + 0x1001) : w32_u16(p.image + 0x1001)) ==
                    (i == 0 ? 0x402000u : 0x2000u));
        }
        if (i == 3) {
            w32_put16(p.image + 0x1001, 0x2000);
            w32_put16(p.image + 0x270a, 0x8000);
            p.base = p.preferred;
            REQUIRE(w32_pe_relocate(&p, 0x1040f000) && w32_u16(p.image + 0x1001) == 0x3000);
        }
        host.services.free(&host, image, p.size);
        host.services.close(&host, &f);
    }
    host_print("PASS relocations\n");
    return 0;
}
#ifdef W32_HOST_MODEL
static jmp_buf stub_jump;
static ULONG diagnostic_count, termination_code;
static ULONG crash_count;
static void stub_diagnostic(void *o, const char *a, const char *b, const char *c) {
    (void)o;
    (void)b;
    (void)c;
    if (!w32_strcmp(a, "stub"))
        diagnostic_count++;
    else if (!w32_strcmp(a, "crash"))
        crash_count++;
}
static void stub_terminate(void *o, ULONG code) {
    (void)o;
    termination_code = code;
    longjmp(stub_jump, 1);
}
static int stubs_once(void) {
    host.services.diagnostic = stub_diagnostic;
    host.services.terminate = stub_terminate;
    static volatile ULONG once;
    for (ULONG i = 0; i < 2; i++) {
        if (!setjmp(stub_jump))
            w32_stub_hit(&once, "example.dll", "Unimplemented");
        REQUIRE(termination_code == (ULONG)STATUS_NOT_IMPLEMENTED);
    }
    /* One bounded stub record; each unhandled raise also leaves a crash record. */
    REQUIRE(diagnostic_count == 1 && once == 1 && crash_count == 2);
    host_print("PASS stub-first-hit\n");
    return 0;
}
static void *heap_contender(void *h) {
    void *p = RtlAllocateHeap(h, 0, 8);
    if (p)
        RtlFreeHeap(h, 0, p);
    return p;
}
static int heap_exceptions(void) {
    host.services.terminate = stub_terminate;
    void *h = RtlCreateHeap(HEAP_GENERATE_EXCEPTIONS, 0, 4096, 4096, 0, 0);
    REQUIRE(h);
    void *p = RtlAllocateHeap(h, 0, 32);
    REQUIRE(p);
    if (!setjmp(stub_jump)) {
        RtlReAllocateHeap(h, 0, p, 1024 * 1024);
        REQUIRE(0);
    }
    REQUIRE(termination_code == (ULONG)STATUS_NO_MEMORY && RtlSizeHeap(h, 0, p) == 32);
    pthread_t thread;
    void *result = 0;
    REQUIRE(!pthread_create(&thread, 0, heap_contender, h));
    REQUIRE(!pthread_join(thread, &result) && result);
    REQUIRE(RtlFreeHeap(h, 0, p));
    REQUIRE(!RtlDestroyHeap(h));
    host_print("PASS heap-exception-unlocks\n");
    return 0;
}
#endif
static int graphs(const char *directory) {
    host.services.invoke = quiet_invoke;
    w32_ldr_init(&loader, &host.services, &host.peb);
    w32_ldr_bind(&loader);
    w32_copy_string(loader.exe_directory, sizeof(loader.exe_directory), directory);
    struct w32_module *m;
    REQUIRE(load_file(directory, "cycle-a.dll", 0, &m));
    REQUIRE(loader.module_count == 2);
    REQUIRE(m->dependencies[0]->dependencies[0] == m);
    REQUIRE(w32_ldr_free(&loader, m) && !loader.module_count);
    REQUIRE(load_file(directory, "forward-depth-16.dll", 0, &m));
    REQUIRE(w32_ldr_proc(&loader, m, "FixtureValue", 0, 0));
    REQUIRE(loader.module_count == 17);
    REQUIRE(w32_ldr_free(&loader, m) && !loader.module_count);
    REQUIRE(load_file(directory, "forward-depth-17.dll", 0, &m));
    REQUIRE(!w32_ldr_proc(&loader, m, "FixtureValue", 0, 0));
    REQUIRE(loader.error.code == 127 && !w32_strcmp(loader.error.reason, "forwarder-depth"));
    REQUIRE(loader.module_count == 1);
    REQUIRE(w32_ldr_free(&loader, m));
    REQUIRE(load_file(directory, "fixture.dll", 0, &m));
    REQUIRE(!w32_ldr_proc(&loader, m, "FixtureValue", 0, 0));
    REQUIRE(loader.error.code == 127);
    REQUIRE(w32_ldr_free(&loader, m));
    w32_ldr_shutdown(&loader);
    REQUIRE(!host.allocations && !host.mappings);
    host_print("PASS cyclic-imports-forwarders\n");
    return 0;
}
static int search_order(const char *directory, const char *expected) {
    host.services.invoke = quiet_invoke;
    w32_ldr_init(&loader, &host.services, &host.peb);
    w32_ldr_bind(&loader);
    char dirs[5][W32_PATH_MAX];
    const char *suffix[] = {"exe", "current", "system", "windows", "path"};
    for (ULONG i = 0; i < 5; i++) {
        ULONG n = w32_strlen(directory);
        w32_copy(dirs[i], directory, n);
        dirs[i][n++] = '/';
        w32_copy_string(dirs[i] + n, W32_PATH_MAX - n, suffix[i]);
    }
    w32_copy_string(loader.exe_directory, sizeof(loader.exe_directory), dirs[0]);
    w32_copy_string(loader.current_directory, sizeof(loader.current_directory), dirs[1]);
    w32_copy_string(loader.system_directory, sizeof(loader.system_directory), dirs[2]);
    w32_copy_string(loader.windows_directory, sizeof(loader.windows_directory), dirs[3]);
    loader.path = dirs[4];
    struct w32_module *m;
    REQUIRE(w32_ldr_load(&loader, "ordinary.dll", 0, &m));
    char want[W32_PATH_MAX];
    ULONG n = w32_strlen(directory);
    w32_copy(want, directory, n);
    want[n++] = '/';
    w32_copy_string(want + n, sizeof(want) - n, expected);
    REQUIRE(!w32_strcmp(m->path, want));
    struct w32_module *same;
    REQUIRE(w32_ldr_load(&loader, "unavailable/ORDINARY.DLL", 0, &same) && same == m &&
            m->references == 2);
    REQUIRE(w32_ldr_free(&loader, same));
    REQUIRE(w32_ldr_free(&loader, m));
    static const char hash[] = "0000000000000000000000000000000000000000000000000000000000000000";
    n = w32_strlen(directory);
    w32_copy(want, directory, n);
    want[n++] = '/';
    w32_copy_string(want + n, sizeof(want) - n, "known.dll");
    struct w32_builtin builtin = {"known.dll", want, hash, 0x70010000, 0x4000, {4, 10, 0, 0}};
    loader.builtins = &builtin;
    loader.builtin_count = 1;
    REQUIRE(w32_builtin_validate(&builtin, 1, &loader.error));
    REQUIRE(w32_ldr_load(&loader, "absent/path/KNOWN.dll", 0, &m));
    REQUIRE(m->builtin && m->pe.base == 0x70010000);
    REQUIRE(w32_ldr_free(&loader, m));
    REQUIRE(loader.module_count == 1);
    struct w32_builtin bad[2] = {builtin, builtin};
    REQUIRE(!w32_builtin_validate(bad, 2, &loader.error));
    w32_ldr_shutdown(&loader);
    REQUIRE(!host.allocations && !host.mappings);
    host_print("PASS search-order-builtin\n");
    return 0;
}
static int altered_search(const char *directory) {
    host.services.invoke=quiet_invoke;
    w32_ldr_init(&loader,&host.services,&host.peb);w32_ldr_bind(&loader);
    char image[W32_PATH_MAX],expected[W32_PATH_MAX];
    ULONG n=w32_strlen(directory);
    REQUIRE(n+32<W32_PATH_MAX);
    w32_copy(image,directory,n);w32_copy_string(image+n,sizeof(image)-n,"/other/consumer.exe");
    w32_copy(loader.exe_directory,directory,n);
    w32_copy_string(loader.exe_directory+n,sizeof(loader.exe_directory)-n,"/exe");
    for(unsigned alternate=0;alternate<2;alternate++){
        struct w32_module *m;
        REQUIRE(w32_ldr_load(&loader,image,alternate?W32_LOAD_ALTERED_SEARCH:0,&m));
        struct w32_module *provider=w32_ldr_module(&loader,"provider.dll");
        w32_copy(expected,directory,n);
        w32_copy_string(expected+n,sizeof(expected)-n,alternate?"/other/provider.dll":"/exe\\provider.dll");
        REQUIRE(provider && !w32_strcmp(provider->path,expected));
        REQUIRE(!loader.altered_directory && w32_strlen(loader.exe_directory)==n+4);
        REQUIRE(w32_ldr_free(&loader,m) && !loader.module_count);
    }
    struct w32_module *m;
    REQUIRE(!w32_ldr_load(&loader,"absent/fail.dll",W32_LOAD_ALTERED_SEARCH,&m));
    REQUIRE(!loader.altered_directory);
    REQUIRE(w32_ldr_load(&loader,"provider.dll",W32_LOAD_ALTERED_SEARCH,&m));
    w32_copy(expected,directory,n);w32_copy_string(expected+n,sizeof(expected)-n,"/exe\\provider.dll");
    REQUIRE(!w32_strcmp(m->path,expected));
    REQUIRE(w32_ldr_free(&loader,m));w32_ldr_shutdown(&loader);
    REQUIRE(!host.allocations && !host.mappings);
    host_print("PASS alternate import folder, normal/basename order, failure restoration and cleanup\n");return 0;
}
static int invalid_relocation(const char *image) {
    w32_ldr_init(&loader, &host.services, &host.peb);
    void *occupied = host.services.map(&host, 0x01000000, 65536, 1);
    REQUIRE(occupied);
    struct w32_module *module;
    REQUIRE(!w32_ldr_load(&loader, image, 0, &module));
    REQUIRE(loader.error.code == 193 && !w32_strcmp(loader.error.reason, "rva-bounds"));
    host.services.unmap(&host, occupied, 65536);
    w32_ldr_shutdown(&loader);
    REQUIRE(!host.allocations && !host.mappings);
    host_print("PASS relocated-invalid-import-refused\n");
    return 0;
}
/* Ldr resource exports, RtlImage helpers and the process-start messages. */
struct resource_info {
    uintptr_t Type, Name, Language;
};
static int resource_bytes(void *base, uintptr_t type, uintptr_t name, ULONG lang,
                          const char *want) {
    struct resource_info info = {type, name, lang};
    void *entry, *data;
    ULONG n;
    if (LdrFindResource_U(base, (const struct w32_resource_info *)&info, 3, &entry) ||
        LdrAccessResource(base, entry, &data, &n) || n != 6)
        return 0;
    for (ULONG i = 0; i < 3; i++)
        if (((uint8_t *)data)[i * 2] != (uint8_t)want[i])
            return 0;
    return 1;
}
static int resources_and_start(const char *directory) {
    host.services.invoke = quiet_invoke;
    w32_ldr_init(&loader, &host.services, &host.peb);
    w32_ldr_bind(&loader);
    w32_copy_string(loader.exe_directory, sizeof(loader.exe_directory), directory);
    struct w32_module *m;
    REQUIRE(load_file(directory, "compiled-resources.exe", W32_LOAD_DATAFILE, &m));
    void *base = m->pe.image;
    static WCHAR named[] = {'N', 'A', 'M', 'E', 'D', 0}, none[] = {'N', 'O', 'N', 'E', 0};
    REQUIRE(resource_bytes(base, 10, 1, 1033, "\1\2\3"));
    REQUIRE(resource_bytes(base, 10, 1, 1040, "\7\10\11"));
    /* Tagged DATAFILE handle (NT style) and language fallback to 0x409. */
    REQUIRE(resource_bytes((uint8_t *)base + 1, 10, 1, 0x810, "\1\2\3"));
    REQUIRE(resource_bytes(base, 10, (uintptr_t)named, 0, "\4\5\6"));
    struct resource_info info = {10, (uintptr_t)none, 0};
    void *p;
    REQUIRE(LdrFindResource_U(base, (void *)&info, 3, &p) == (NTSTATUS)0xc000008b && !p);
    info.Type = 11;
    REQUIRE(LdrFindResource_U(base, (void *)&info, 3, &p) == (NTSTATUS)0xc000008a && !p);
    info.Type = 10;
    REQUIRE(!LdrFindResourceDirectory_U(base, (void *)&info, 1, &p));
    REQUIRE(w32_u16((uint8_t *)p + 12) == 1 && w32_u16((uint8_t *)p + 14) == 1);
    uint8_t *nt = RtlImageNtHeader(base);
    REQUIRE(nt && w32_u32(nt) == 0x4550);
    ULONG size;
    uint8_t *mapped = RtlImageDirectoryEntryToData(base, 1, 2, &size);
    REQUIRE(mapped && size && mapped == (uint8_t *)base + m->pe.dirs[2].rva);
    /* The same directory through the raw file, mapped as data. */
    char path[W32_PATH_MAX];
    ULONG n = w32_strlen(directory);
    w32_copy(path, directory, n);
    path[n++] = '/';
    w32_copy_string(path + n, sizeof(path) - n, "compiled-resources.exe");
    struct w32_file f;
    REQUIRE(host.services.open(&host, path, &f));
    uint8_t *raw = RtlImageDirectoryEntryToData((void *)f.bytes, 0, 2, &size);
    REQUIRE(raw && raw != mapped && raw > f.bytes && raw < f.bytes + f.size);
    for (ULONG i = 0; i < 16; i++)
        REQUIRE(raw[i] == mapped[i]);
    host.services.close(&host, &f);
    REQUIRE(w32_ldr_free(&loader, m));
    char message[2 * W32_PATH_MAX];
    w32_copy_string(path + n, sizeof(path) - n, "missing-dll.exe");
    REQUIRE(w32_ldr_start_process(&loader, path, &m) == STATUS_DLL_NOT_FOUND && !m);
    REQUIRE(w32_ldr_start_message(&loader, path, message, sizeof(message)));
    static const char dll[] = "The dynamic link library absent.dll could not be found in the "
                              "specified path ";
    for (ULONG i = 0; dll[i]; i++)
        REQUIRE(message[i] == dll[i]);
    REQUIRE(!w32_strcmp(message + sizeof(dll) - 1 + n - 1, ";.;C:\\WINDOWS\\SYSTEM;C:\\WINDOWS."));
    w32_copy_string(path + n, sizeof(path) - n, "missing-export.exe");
    REQUIRE(w32_ldr_start_process(&loader, path, &m) == STATUS_ENTRYPOINT_NOT_FOUND);
    REQUIRE(w32_ldr_start_message(&loader, path, message, sizeof(message)));
    REQUIRE(!w32_strcmp(message, "The missing-export.exe file is linked to missing export "
                                 "provider.dll:MissingValue."));
    w32_copy_string(path + n, sizeof(path) - n, "provider.dll");
    REQUIRE(w32_ldr_start_process(&loader, path, &m) == (NTSTATUS)0xc000007b);
    REQUIRE(!w32_strcmp(loader.error.reason, "expected-executable"));
    REQUIRE(!w32_ldr_start_message(&loader, path, message, sizeof(message)));
    w32_ldr_shutdown(&loader);
    REQUIRE(host.peb.ProcessHeap && !RtlDestroyHeap(host.peb.ProcessHeap));
    host.peb.ProcessHeap = 0;
    REQUIRE(!host.allocations && !host.mappings);
    host_print("PASS resources-start\n");
    return 0;
}
#ifndef W32_HOST_MODEL
/* The CiukiOS backend is fail-closed until f5-03a: named refusals, no crash. */
static int ciuki_stub(const char *directory) {
    const struct w32_services *saved = w32_get_services();
    w32_ldr_init(&loader, &w32_ciuki_services_stub, 0);
    REQUIRE(!RtlCreateHeap(HEAP_GROWABLE, 0, 0, 0, 0, 0));
    struct w32_module *m;
    REQUIRE(!load_file(directory, "console.exe", 0, &m));
    REQUIRE(loader.error.code == 126 && !w32_strcmp(loader.error.reason, "missing-dll"));
    REQUIRE(!RtlEnterCriticalSection(&loader.lock) && !RtlLeaveCriticalSection(&loader.lock));
    w32_ldr_shutdown(&loader);
    w32_set_services(saved);
    host_print("PASS ciuki-stub-fail-closed\n");
    return 0;
}
#endif
int host_main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    if (!w32_strcmp(argv[1], "heap"))
        return heaps();
    if (!w32_strcmp(argv[1], "critical"))
        return critical();
    if (!w32_strcmp(argv[1], "strings"))
        return strings();
    if (!w32_strcmp(argv[1], "seh"))
        return seh();
    if (!w32_strcmp(argv[1], "loader") && argc >= 3)
        return loaders(argv[2]);
    if (!w32_strcmp(argv[1], "relocations") && argc >= 3)
        return relocations(argv[2]);
    if (!w32_strcmp(argv[1], "graphs") && argc >= 3)
        return graphs(argv[2]);
    if (!w32_strcmp(argv[1], "invalid-reloc") && argc >= 3)
        return invalid_relocation(argv[2]);
    if (!w32_strcmp(argv[1], "resources") && argc >= 3)
        return resources_and_start(argv[2]);
    if (!w32_strcmp(argv[1], "search") && argc >= 4)
        return search_order(argv[2], argv[3]);
    if (!w32_strcmp(argv[1], "altered-search") && argc >= 3)
        return altered_search(argv[2]);
#ifndef W32_HOST_MODEL
    if (!w32_strcmp(argv[1], "ciuki-stub") && argc >= 3)
        return ciuki_stub(argv[2]);
#endif
#ifdef W32_HOST_MODEL
    if (!w32_strcmp(argv[1], "stubs"))
        return stubs_once();
    if (!w32_strcmp(argv[1], "heap-exceptions"))
        return heap_exceptions();
#endif
    return 2;
}
