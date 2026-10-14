/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#ifndef CIUKI_W32_LDR_H
#define CIUKI_W32_LDR_H
#include "ntdll.h"
#define W32_MAX_SECTIONS 96
#define W32_MAX_MODULES 256
#define W32_MAX_TLS 64
#define W32_PATH_MAX 1024
struct w32_error {
    ULONG code;
    const char *reason;
    char module[W32_PATH_MAX], symbol[256];
};
struct w32_section {
    ULONG rva, span, raw, offset, flags;
};
struct w32_directory {
    ULONG rva, size;
};
struct w32_pe {
    const uint8_t *file;
    ULONG file_size, preferred, base, size, headers, entry, section_alignment;
    uint16_t flags, subsystem, section_count;
    struct w32_section sections[W32_MAX_SECTIONS];
    struct w32_directory dirs[16];
    uint8_t *image;
    struct w32_error *error;
};
int w32_pe_parse(struct w32_pe *, const void *, ULONG, struct w32_error *);
int w32_pe_copy_image(struct w32_pe *, void *);
int w32_pe_validate(struct w32_pe *);
int w32_pe_relocate(struct w32_pe *, ULONG);
void *w32_pe_span(struct w32_pe *, ULONG, ULONG);
const char *w32_pe_string(struct w32_pe *, ULONG);
int w32_pe_protect(struct w32_pe *, const struct w32_services *);
struct w32_resource_key {
    ULONG id;
    const WCHAR *name;
};
int w32_pe_resource(struct w32_pe *, struct w32_resource_key, struct w32_resource_key, ULONG,
                    const void **, ULONG *);
int w32_pe_resource_lookup(struct w32_pe *, const struct w32_resource_key *, ULONG, void **);
enum { W32_LOAD_DATAFILE = 2, W32_LOAD_EXECUTABLE = 0x10000 };
struct w32_builtin {
    const char *name, *path, *sha256;
    ULONG base, size;
    uint16_t version[4];
};
struct w32_ldr_entry {
    LIST_ENTRY InLoadOrderLinks, InMemoryOrderLinks, InInitializationOrderLinks;
    void *DllBase, *EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName, BaseDllName;
    ULONG Flags;
    uint16_t LoadCount, TlsIndex;
};
#ifndef W32_HOST_MODEL
/* LDR_DATA_TABLE_ENTRY prefix read by kernel32 and the reused DLLs. */
_Static_assert(offsetof(struct w32_ldr_entry, DllBase) == 0x18, "LDR DllBase");
_Static_assert(offsetof(struct w32_ldr_entry, SizeOfImage) == 0x20, "LDR SizeOfImage");
_Static_assert(offsetof(struct w32_ldr_entry, FullDllName) == 0x24, "LDR FullDllName");
_Static_assert(offsetof(struct w32_ldr_entry, BaseDllName) == 0x2c, "LDR BaseDllName");
_Static_assert(offsetof(struct w32_ldr_entry, LoadCount) == 0x38, "LDR LoadCount");
_Static_assert(offsetof(struct w32_ldr_entry, TlsIndex) == 0x3a, "LDR TlsIndex");
#endif
struct w32_module {
    struct w32_module *next;
    struct w32_pe pe;
    struct w32_ldr_entry entry;
    WCHAR wide_path[W32_PATH_MAX], wide_name[256];
    char path[W32_PATH_MAX], name[256];
    ULONG references, busy, load_flags, load_order, initialization_order, tls_index;
    ULONG tls_template, tls_size, tls_zero, tls_alignment;
    struct w32_module *dependencies[W32_MAX_MODULES];
    ULONG dependency_count;
    uint8_t state, builtin, disable_threads, mark;
};
struct w32_ldr_thread {
    struct w32_ldr_thread *next;
    struct w32_teb *teb;
    void *tls[W32_MAX_TLS];
    ULONG sizes[W32_MAX_TLS];
};
struct w32_loader {
    const struct w32_services *services;
    RTL_CRITICAL_SECTION lock;
    struct w32_module *modules, *main;
    struct w32_ldr_thread *threads;
    struct w32_peb *peb;
    struct w32_peb_ldr lists;
    const struct w32_builtin *builtins;
    ULONG builtin_count, module_count, sequence;
    char exe_directory[W32_PATH_MAX], current_directory[W32_PATH_MAX];
    char system_directory[W32_PATH_MAX], windows_directory[W32_PATH_MAX];
    const char *path;
    struct w32_error error;
    uint8_t tls_used[W32_MAX_TLS];
    ULONG transaction_depth, collecting;
    uint8_t process_start, terminating;
};
int w32_builtin_validate(const struct w32_builtin *, ULONG, struct w32_error *);
void w32_ldr_init(struct w32_loader *, const struct w32_services *, struct w32_peb *);
int w32_ldr_load(struct w32_loader *, const char *, ULONG, struct w32_module **);
int w32_ldr_free(struct w32_loader *, struct w32_module *);
void w32_ldr_shutdown(struct w32_loader *);
struct w32_module *w32_ldr_module(struct w32_loader *, const char *);
void *w32_ldr_proc(struct w32_loader *, struct w32_module *, const char *, ULONG, ULONG);
int w32_ldr_thread_attach(struct w32_loader *, struct w32_teb *);
void w32_ldr_thread_detach(struct w32_loader *, struct w32_teb *);
void w32_ldr_bind(struct w32_loader *);
int w32_ldr_start_message(const struct w32_loader *, const char *, char *, ULONG);
NTSTATUS w32_ldr_start_process(struct w32_loader *, const char *, struct w32_module **);
struct w32_resource_info;
NTSTATUS W32API LdrFindResource_U(void *, const struct w32_resource_info *, ULONG, void **);
NTSTATUS W32API LdrFindResourceDirectory_U(void *, const struct w32_resource_info *, ULONG,
                                           void **);
NTSTATUS W32API LdrAccessResource(void *, const void *, void **, ULONG *);
void *W32API RtlImageNtHeader(void *);
void *W32API RtlImageDirectoryEntryToData(void *, BOOLEAN, uint16_t, ULONG *);
NTSTATUS W32API LdrLoadDll(const WCHAR *, ULONG *, UNICODE_STRING *, void **);
NTSTATUS W32API LdrUnloadDll(void *);
NTSTATUS W32API LdrGetDllHandle(const WCHAR *, ULONG *, UNICODE_STRING *, void **);
NTSTATUS W32API LdrGetProcedureAddress(void *, ANSI_STRING *, ULONG, void **);
NTSTATUS W32API LdrDisableThreadCalloutsForDll(void *);
NTSTATUS W32API LdrLockLoaderLock(ULONG, ULONG *, ULONG *);
NTSTATUS W32API LdrUnlockLoaderLock(ULONG, ULONG);
#endif
