/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include <w32/ntdll.h>
NTSTATUS W32API RtlInitializeCriticalSectionAndSpinCount(RTL_CRITICAL_SECTION *c, ULONG spin) {
    if (!c)
        return STATUS_INVALID_PARAMETER;
    w32_fill(c, 0, sizeof(*c));
    c->LockCount = -1;
    c->SpinCount = spin;
    return 0;
}
NTSTATUS W32API RtlInitializeCriticalSection(RTL_CRITICAL_SECTION *c) {
    return RtlInitializeCriticalSectionAndSpinCount(c, 0);
}
NTSTATUS W32API RtlDeleteCriticalSection(RTL_CRITICAL_SECTION *c) {
    if (!c || c->OwningThread)
        return STATUS_INVALID_PARAMETER;
    /* Free only a debug block InitializeCriticalSectionEx allocated; static
     * blocks (Wine's named sections) and (void *)-1 are not owned here. */
    RTL_CRITICAL_SECTION_DEBUG *debug = c->DebugInfo;
    struct w32_teb *teb = NtCurrentTeb();
    if (debug && debug != (void *)~(uintptr_t)0 && debug->CreatorBackTraceIndex == W32_CS_DEBUG_OWNED &&
        debug->CriticalSection == c && teb && teb->ProcessEnvironmentBlock &&
        teb->ProcessEnvironmentBlock->ProcessHeap)
        RtlFreeHeap(teb->ProcessEnvironmentBlock->ProcessHeap, 0, debug);
    return RtlInitializeCriticalSection(c);
}
BOOLEAN W32API RtlTryEnterCriticalSection(RTL_CRITICAL_SECTION *c) {
    const struct w32_services *s = w32_get_services();
    if (!c || !s || !s->thread_id)
        return 0;
    ULONG tid = s->thread_id(s->opaque), expected = 0;
    if (__atomic_load_n(&c->OwningThread, __ATOMIC_RELAXED) == tid ||
        __atomic_compare_exchange_n(&c->OwningThread, &expected, tid, 0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
        __atomic_add_fetch(&c->LockCount, 1, __ATOMIC_RELAXED);
        c->RecursionCount++;
        return 1;
    }
    return 0;
}
NTSTATUS W32API RtlEnterCriticalSection(RTL_CRITICAL_SECTION *c) {
    const struct w32_services *s = w32_get_services();
    if (!c || !s || !s->thread_id)
        return STATUS_INVALID_PARAMETER;
    if (RtlTryEnterCriticalSection(c))
        return 0;
    if (!s->wait_word || !s->wake_word)
        return STATUS_NOT_IMPLEMENTED;
    __atomic_add_fetch(&c->LockCount, 1, __ATOMIC_RELAXED);
    ULONG tid = s->thread_id(s->opaque);
    for (;;) {
        ULONG owner = 0;
        if (__atomic_compare_exchange_n(&c->OwningThread, &owner, tid, 0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            break;
        s->wait_word(s->opaque, &c->OwningThread, owner);
    }
    c->RecursionCount = 1;
    return 0;
}
NTSTATUS W32API RtlLeaveCriticalSection(RTL_CRITICAL_SECTION *c) {
    const struct w32_services *s = w32_get_services();
    if (!c || !s || c->OwningThread != s->thread_id(s->opaque) || c->RecursionCount <= 0)
        return STATUS_INVALID_PARAMETER;
    __atomic_sub_fetch(&c->LockCount, 1, __ATOMIC_RELAXED);
    if (!--c->RecursionCount) {
        __atomic_store_n(&c->OwningThread, 0, __ATOMIC_RELEASE);
        if (s->wake_word)
            s->wake_word(s->opaque, &c->OwningThread);
    }
    return 0;
}
