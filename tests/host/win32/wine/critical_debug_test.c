/* SPDX-License-Identifier: LGPL-2.1-or-later
 * f5-20C (H1 finding): RtlDeleteCriticalSection frees exactly the debug
 * blocks InitializeCriticalSectionEx(RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO)
 * allocated, never a static Wine RTL_CRITICAL_SECTION_DEBUG or (void *)-1.
 * Production ntdll heap and critical-section code, ASan/UBSan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <w32/ntdll.h>
#include <w32/shims.h>
_Thread_local struct w32_teb *w32_model_teb;
struct ciuki_w32_compat w32_shims; /* no shim: plain heap */
static void *allocate(void *o, uint32_t n) { (void)o; return calloc(1, n); }
static void release(void *o, void *p, uint32_t n) { (void)o; (void)n; free(p); }
static uint32_t tid(void *o) { (void)o; return 7; }
void W32API RtlRaiseStatus(NTSTATUS status) { (void)status; abort(); }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "critical line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void) {
    struct w32_teb teb = {0};
    struct w32_peb peb = {0};
    teb.ProcessEnvironmentBlock = &peb;
    w32_model_teb = &teb;
    const struct w32_services services = {.alloc = allocate, .free = release, .thread_id = tid};
    w32_set_services(&services);
    void *heap = RtlCreateHeap(2, 0, 0, 0, 0, 0);
    CHECK(heap);
    peb.ProcessHeap = heap;
    /* Owned block: freed, section reset. */
    RTL_CRITICAL_SECTION owned;
    CHECK(!RtlInitializeCriticalSection(&owned));
    RTL_CRITICAL_SECTION_DEBUG *debug = RtlAllocateHeap(heap, 8, sizeof(*debug));
    CHECK(debug && (sizeof(void *) != 4 || sizeof(*debug) == 32));
    debug->CreatorBackTraceIndex = W32_CS_DEBUG_OWNED;
    debug->CriticalSection = &owned;
    debug->Spare[0] = (uintptr_t)"critical_debug_test.c: owned";
    owned.DebugInfo = debug;
    CHECK(RtlValidateHeap(heap, 0, debug));
    CHECK(!RtlDeleteCriticalSection(&owned) && !owned.DebugInfo && owned.LockCount == -1);
    CHECK(!RtlValidateHeap(heap, 0, debug) && RtlValidateHeap(heap, 0, 0));
    /* Wine's static named debug block: never freed. */
    RTL_CRITICAL_SECTION named;
    static RTL_CRITICAL_SECTION_DEBUG named_debug;
    CHECK(!RtlInitializeCriticalSection(&named));
    named_debug.CriticalSection = &named;
    named_debug.Spare[0] = (uintptr_t)"critical_debug_test.c: named";
    named.DebugInfo = &named_debug;
    CHECK(!RtlDeleteCriticalSection(&named) && !named.DebugInfo && named_debug.Spare[0]);
    /* A marked block of another section and (void *)-1 are not freed. */
    RTL_CRITICAL_SECTION other;
    debug = RtlAllocateHeap(heap, 8, sizeof(*debug));
    CHECK(debug);
    debug->CreatorBackTraceIndex = W32_CS_DEBUG_OWNED;
    debug->CriticalSection = &owned;
    CHECK(!RtlInitializeCriticalSection(&other));
    other.DebugInfo = debug;
    CHECK(!RtlDeleteCriticalSection(&other) && RtlValidateHeap(heap, 0, debug));
    CHECK(RtlFreeHeap(heap, 0, debug));
    other.DebugInfo = (void *)~(uintptr_t)0;
    CHECK(!RtlDeleteCriticalSection(&other) && !other.DebugInfo);
    /* An owned section still held is refused and keeps its block. */
    debug = RtlAllocateHeap(heap, 8, sizeof(*debug));
    debug->CreatorBackTraceIndex = W32_CS_DEBUG_OWNED;
    debug->CriticalSection = &owned;
    owned.DebugInfo = debug;
    owned.OwningThread = 7;
    CHECK(RtlDeleteCriticalSection(&owned) == STATUS_INVALID_PARAMETER && RtlValidateHeap(heap, 0, debug));
    owned.OwningThread = 0;
    CHECK(!RtlDeleteCriticalSection(&owned) && !RtlValidateHeap(heap, 0, debug));
    CHECK(RtlValidateHeap(heap, 0, 0) && !RtlDestroyHeap(heap));
    puts("PASS critical section debug blocks: owned freed, static and foreign kept");
    return 0;
}
