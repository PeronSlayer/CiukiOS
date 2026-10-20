/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. See sdk/win32/COPYING.LIB. */
#ifndef CIUKI_W32_BASE_H
#define CIUKI_W32_BASE_H
#include <stddef.h>
#include <stdint.h>
#if !defined(__i386__) && !defined(_M_IX86) && !defined(W32_HOST_MODEL)
#error The Win32 core requires an i386 compiler
#endif
#ifdef W32_HOST_MODEL
#define W32API
#else
#define W32API __attribute__((stdcall))
#endif
#define W32_NORETURN __attribute__((noreturn))
typedef uint16_t WCHAR;
typedef int32_t NTSTATUS;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint8_t BOOLEAN;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xc0000002)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_NO_MEMORY ((NTSTATUS)0xc0000017)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xc0000023)
#define STATUS_NONCONTINUABLE_EXCEPTION ((NTSTATUS)0xc0000025)
#define STATUS_INVALID_DISPOSITION ((NTSTATUS)0xc0000026)
#define STATUS_UNWIND ((NTSTATUS)0xc0000027)
#define STATUS_BAD_STACK ((NTSTATUS)0xc0000028)
#define STATUS_INVALID_UNWIND_TARGET ((NTSTATUS)0xc0000029)
#define STATUS_DLL_NOT_FOUND ((NTSTATUS)0xc0000135)
#define STATUS_ENTRYPOINT_NOT_FOUND ((NTSTATUS)0xc0000139)
#define STATUS_DLL_INIT_FAILED ((NTSTATUS)0xc0000142)
#define STATUS_UNHANDLED_EXCEPTION ((NTSTATUS)0xc0000144)
typedef struct {
    uint16_t Length, MaximumLength;
    char *Buffer;
} ANSI_STRING;
typedef struct {
    uint16_t Length, MaximumLength;
    WCHAR *Buffer;
} UNICODE_STRING;
typedef struct w32_list {
    struct w32_list *Flink, *Blink;
} LIST_ENTRY;
typedef struct {
    void *DebugInfo;
    volatile LONG LockCount;
    LONG RecursionCount;
    volatile ULONG OwningThread;
    void *LockSemaphore;
    ULONG SpinCount;
} RTL_CRITICAL_SECTION;
/* NT RTL_CRITICAL_SECTION_DEBUG, 32-bit layout (Spare = Flags and the high
 * back-trace words; Wine stores a section name in Spare[0]). CiukiOS creates
 * one only for InitializeCriticalSectionEx with
 * RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO and marks it W32_CS_DEBUG_OWNED,
 * so RtlDeleteCriticalSection frees exactly the blocks it allocated. */
typedef struct {
    uint16_t Type, CreatorBackTraceIndex;
    RTL_CRITICAL_SECTION *CriticalSection;
    LIST_ENTRY ProcessLocksList;
    ULONG EntryCount, ContentionCount;
    uintptr_t Spare[2];
} RTL_CRITICAL_SECTION_DEBUG;
#define W32_CS_DEBUG_OWNED 0xc105u
void *w32_copy(void *, const void *, uint32_t);
void *w32_fill(void *, uint8_t, uint32_t);
uint32_t w32_strlen(const char *);
int w32_strcmp(const char *, const char *);
int w32_stricmp(const char *, const char *);
int w32_copy_string(char *, uint32_t, const char *);
uint16_t w32_u16(const void *);
uint32_t w32_u32(const void *);
void w32_put16(void *, uint16_t);
void w32_put32(void *, uint32_t);
#endif
