/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#ifndef CIUKI_W32_NTDLL_H
#define CIUKI_W32_NTDLL_H
#include "services.h"
struct w32_exception {
    ULONG ExceptionCode, ExceptionFlags;
    struct w32_exception *ExceptionRecord;
    void *ExceptionAddress;
    ULONG NumberParameters, ExceptionInformation[15];
};
struct w32_float_save {
    ULONG ControlWord, StatusWord, TagWord, ErrorOffset, ErrorSelector;
    ULONG DataOffset, DataSelector;
    uint8_t RegisterArea[80];
    ULONG Cr0NpxState;
};
struct w32_context {
    ULONG ContextFlags, Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    struct w32_float_save FloatSave;
    ULONG SegGs, SegFs, SegEs, SegDs;
    ULONG Edi, Esi, Ebx, Edx, Ecx, Eax, Ebp, Eip, SegCs, EFlags, Esp, SegSs;
    uint8_t ExtendedRegisters[512];
};
struct w32_seh;
typedef LONG (*w32_handler)(struct w32_exception *, struct w32_seh *, struct w32_context *,
                            struct w32_seh **);
struct w32_seh {
    struct w32_seh *Prev;
    w32_handler Handler;
};
enum {
    ExceptionContinueExecution,
    ExceptionContinueSearch,
    ExceptionNestedException,
    ExceptionCollidedUnwind
};
enum {
    EXCEPTION_NONCONTINUABLE = 1,
    EXCEPTION_UNWINDING = 2,
    EXCEPTION_EXIT_UNWIND = 4,
    EXCEPTION_STACK_INVALID = 8,
    EXCEPTION_NESTED_CALL = 16,
    EXCEPTION_TARGET_UNWIND = 32,
    EXCEPTION_COLLIDED_UNWIND = 64
};
struct w32_tib {
    struct w32_seh *ExceptionList;
    void *StackBase, *StackLimit, *SubSystemTib, *FiberData, *ArbitraryUserPointer;
    struct w32_tib *Self;
};
struct w32_process_parameters {
    ULONG MaximumLength, Length, Flags, DebugFlags;
    void *ConsoleHandle;
    ULONG ConsoleFlags;
    void *StandardInput, *StandardOutput, *StandardError;
    struct {
        UNICODE_STRING DosPath;
        void *Handle;
    } CurrentDirectory;
    UNICODE_STRING DllPath, ImagePathName, CommandLine;
    WCHAR *Environment;
    ULONG StartingX, StartingY, CountX, CountY, CountCharsX, CountCharsY;
    ULONG FillAttribute, WindowFlags, ShowWindowFlags;
    UNICODE_STRING WindowTitle, DesktopInfo, ShellInfo, RuntimeData;
};
struct w32_peb_ldr {
    ULONG Length;
    BOOLEAN Initialized;
    uint8_t padding[3];
    void *SsHandle;
    LIST_ENTRY InLoadOrderModuleList, InMemoryOrderModuleList, InInitializationOrderModuleList;
};
struct w32_peb {
    BOOLEAN InheritedAddressSpace, ReadImageFileExecOptions, BeingDebugged, padding;
    void *Mutant, *ImageBaseAddress;
    struct w32_peb_ldr *Ldr;
    struct w32_process_parameters *ProcessParameters;
    void *SubSystemData, *ProcessHeap;
    RTL_CRITICAL_SECTION *FastPebLock;
};
struct w32_teb {
    struct w32_tib Tib;
    void *EnvironmentPointer;
    struct {
        void *UniqueProcess, *UniqueThread;
    } ClientId;
    void *ActiveRpcHandle, **ThreadLocalStoragePointer;
    struct w32_peb *ProcessEnvironmentBlock;
    ULONG LastErrorValue, CountOfOwnedCriticalSections;
    uint8_t reserved[0xe10 - 0x3c];
    void *TlsSlots[64];
    /* Private core extension, outside the frozen Windows-compatible prefix. */
    ULONG CoreFilterDepth;
};
#ifndef W32_HOST_MODEL
_Static_assert(sizeof(void *) == 4 && sizeof(UNICODE_STRING) == 8, "i386 ABI");
_Static_assert(offsetof(struct w32_teb, ProcessEnvironmentBlock) == 0x30, "TEB.PEB");
_Static_assert(offsetof(struct w32_teb, TlsSlots) == 0xe10, "TEB.TlsSlots");
_Static_assert(offsetof(struct w32_peb, ProcessParameters) == 0x10, "PEB parameters");
_Static_assert(offsetof(struct w32_peb, ProcessHeap) == 0x18, "PEB heap");
_Static_assert(offsetof(struct w32_process_parameters, StandardOutput) == 0x1c, "process stdout");
_Static_assert(offsetof(struct w32_process_parameters, Environment) == 0x48, "process environment");
_Static_assert(sizeof(struct w32_teb) <= 4096, "TEB descriptor limit");
_Static_assert(offsetof(struct w32_teb, Tib.Self) == 0x18, "TEB.Self");
_Static_assert(offsetof(struct w32_teb, ClientId) == 0x20, "TEB.ClientId");
_Static_assert(offsetof(struct w32_teb, ThreadLocalStoragePointer) == 0x2c, "TEB.Tls");
_Static_assert(offsetof(struct w32_teb, LastErrorValue) == 0x34, "TEB.LastErrorValue");
_Static_assert(offsetof(struct w32_peb, BeingDebugged) == 2, "PEB.BeingDebugged");
_Static_assert(offsetof(struct w32_peb, ImageBaseAddress) == 8, "PEB.ImageBaseAddress");
_Static_assert(offsetof(struct w32_peb, Ldr) == 0x0c, "PEB.Ldr");
_Static_assert(offsetof(struct w32_peb, FastPebLock) == 0x1c, "PEB.FastPebLock");
_Static_assert(offsetof(struct w32_peb_ldr, InLoadOrderModuleList) == 0x0c, "PEB_LDR_DATA");
_Static_assert(offsetof(struct w32_peb_ldr, InInitializationOrderModuleList) == 0x1c,
               "PEB_LDR_DATA init order");
_Static_assert(offsetof(struct w32_process_parameters, CurrentDirectory) == 0x24,
               "process cwd");
_Static_assert(offsetof(struct w32_process_parameters, ImagePathName) == 0x38, "image path");
_Static_assert(offsetof(struct w32_process_parameters, CommandLine) == 0x40, "command line");
#endif
_Static_assert(offsetof(struct w32_context, Eip) == 184 && sizeof(struct w32_context) == 716,
               "CONTEXT");
static inline struct w32_teb *NtCurrentTeb(void) {
#ifdef W32_HOST_MODEL
    extern _Thread_local struct w32_teb *w32_model_teb;
    return w32_model_teb;
#else
    struct w32_teb *teb;
    __asm__ volatile("movl %%fs:0x18,%0" : "=r"(teb));
    return teb;
#endif
}
NTSTATUS W32API RtlInitializeCriticalSection(RTL_CRITICAL_SECTION *);
NTSTATUS W32API RtlInitializeCriticalSectionAndSpinCount(RTL_CRITICAL_SECTION *, ULONG);
NTSTATUS W32API RtlDeleteCriticalSection(RTL_CRITICAL_SECTION *);
NTSTATUS W32API RtlEnterCriticalSection(RTL_CRITICAL_SECTION *);
NTSTATUS W32API RtlLeaveCriticalSection(RTL_CRITICAL_SECTION *);
BOOLEAN W32API RtlTryEnterCriticalSection(RTL_CRITICAL_SECTION *);
enum {
    HEAP_NO_SERIALIZE = 1,
    HEAP_GENERATE_EXCEPTIONS = 4,
    HEAP_ZERO_MEMORY = 8,
    HEAP_REALLOC_IN_PLACE_ONLY = 16,
    HEAP_TAIL_CHECKING_ENABLED = 32,
    HEAP_FREE_CHECKING_ENABLED = 64,
    HEAP_GROWABLE = 2
};
void *W32API RtlCreateHeap(ULONG, void *, uint32_t, uint32_t, void *, void *);
void *W32API RtlDestroyHeap(void *);
void *W32API RtlAllocateHeap(void *, ULONG, uint32_t);
void *W32API RtlReAllocateHeap(void *, ULONG, void *, uint32_t);
BOOLEAN W32API RtlFreeHeap(void *, ULONG, void *);
uint32_t W32API RtlSizeHeap(void *, ULONG, const void *);
BOOLEAN W32API RtlValidateHeap(void *, ULONG, const void *);
BOOLEAN W32API RtlLockHeap(void *);
BOOLEAN W32API RtlUnlockHeap(void *);
struct w32_heap_entry {
    void *DataAddress;
    ULONG DataSize;
    uint8_t OverheadBytes, SegmentIndex;
    uint16_t Flags;
    union {
        struct {
            ULONG Settable, Tag;
            void *AllocatorBackTrace;
            ULONG Reserved[2];
        } Block;
        struct {
            ULONG CommittedSize, UnCommittedSize;
            void *FirstEntry, *LastEntry;
        } Segment;
    } u;
};
NTSTATUS W32API RtlWalkHeap(void *, struct w32_heap_entry *);
void W32API RtlInitAnsiString(ANSI_STRING *, const char *);
void W32API RtlInitUnicodeString(UNICODE_STRING *, const WCHAR *);
NTSTATUS W32API RtlAnsiStringToUnicodeString(UNICODE_STRING *, const ANSI_STRING *, BOOLEAN);
NTSTATUS W32API RtlUnicodeStringToAnsiString(ANSI_STRING *, const UNICODE_STRING *, BOOLEAN);
void W32API RtlFreeAnsiString(ANSI_STRING *);
void W32API RtlFreeUnicodeString(UNICODE_STRING *);
LONG W32API RtlCompareUnicodeString(const UNICODE_STRING *, const UNICODE_STRING *, BOOLEAN);
BOOLEAN W32API RtlEqualUnicodeString(const UNICODE_STRING *, const UNICODE_STRING *, BOOLEAN);
NTSTATUS W32API RtlAppendUnicodeStringToString(UNICODE_STRING *, const UNICODE_STRING *);
NTSTATUS W32API RtlUpcaseUnicodeString(UNICODE_STRING *, const UNICODE_STRING *, BOOLEAN);
WCHAR W32API RtlUpcaseUnicodeChar(WCHAR);
ULONG W32API RtlGetFullPathName_U(const WCHAR *, ULONG, WCHAR *, WCHAR **);
BOOLEAN W32API RtlDosPathNameToNtPathName_U(const WCHAR *, UNICODE_STRING *, WCHAR **, void *);
ULONG W32API RtlNtStatusToDosError(NTSTATUS);
BOOLEAN W32API RtlDispatchException(struct w32_exception *, struct w32_context *);
NTSTATUS w32_dispatch_exception(struct w32_exception *, struct w32_context *);
NTSTATUS w32_dispatch_user_exception(struct w32_exception *, struct w32_context *);
NTSTATUS w32_unwind(struct w32_seh *, struct w32_exception *, struct w32_context *);
struct w32_exception_pointers {
    struct w32_exception *ExceptionRecord;
    struct w32_context *ContextRecord;
};
typedef LONG(W32API *w32_top_filter)(struct w32_exception_pointers *);
w32_top_filter W32API SetUnhandledExceptionFilter(w32_top_filter);
LONG W32API UnhandledExceptionFilter(struct w32_exception_pointers *);
void W32API RtlCaptureContext(struct w32_context *);
void W32API RtlRaiseException(struct w32_exception *);
void W32API RaiseException(ULONG, ULONG, ULONG, const ULONG *);
W32_NORETURN void W32API RtlRaiseStatus(NTSTATUS);
void W32API RtlUnwind(void *, void *, struct w32_exception *, void *);
struct w32_teb *W32API w32_current_teb(void);
void W32API KiUserExceptionDispatcher(struct w32_exception *, struct w32_context *);
void w32_stub_hit(volatile ULONG *, const char *, const char *);
#endif
