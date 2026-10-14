/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors.
 *
 * H1 test shim only: the six module APIs the f5-02 builtin-import fixture
 * imports, written directly over the PE ntdll Ldr* exports and the PEB loader
 * list. It is NOT the f5-06 kernel32 (no last-error, no Unicode/ACP policy,
 * no path canonicalisation) and is never part of a builtin store or image. */
typedef unsigned long U32;
typedef unsigned short WCHAR;
typedef long NTSTATUS;
struct unicode {
    unsigned short length, maximum;
    WCHAR *buffer;
};
struct ansi {
    unsigned short length, maximum;
    const char *buffer;
};
struct list {
    struct list *next, *prev;
};
#define IMPORT __declspec(dllimport)
IMPORT NTSTATUS __stdcall LdrLoadDll(const WCHAR *, U32 *, struct unicode *, void **);
IMPORT NTSTATUS __stdcall LdrUnloadDll(void *);
IMPORT NTSTATUS __stdcall LdrGetDllHandle(const WCHAR *, U32 *, struct unicode *, void **);
IMPORT NTSTATUS __stdcall LdrGetProcedureAddress(void *, struct ansi *, U32, void **);
IMPORT void *__stdcall NtCurrentTeb(void);
static int widen(struct unicode *u, WCHAR *buffer, U32 n, const char *s) {
    U32 i = 0;
    for (; s[i]; i++) {
        if (i + 1 >= n)
            return 0;
        buffer[i] = (unsigned char)s[i];
    }
    buffer[i] = 0;
    u->buffer = buffer;
    u->length = (unsigned short)(i * 2);
    u->maximum = (unsigned short)(i * 2 + 2);
    return 1;
}
void *__stdcall LoadLibraryExA(const char *name, void *file, U32 flags) {
    WCHAR buffer[260];
    struct unicode u;
    void *base = 0;
    U32 ldr_flags = flags & 2; /* LOAD_LIBRARY_AS_DATAFILE */
    if (file || !name || !widen(&u, buffer, 260, name))
        return 0;
    return LdrLoadDll(0, &ldr_flags, &u, &base) ? 0 : base;
}
void *__stdcall LoadLibraryA(const char *name) {
    return LoadLibraryExA(name, 0, 0);
}
void *__stdcall GetModuleHandleA(const char *name) {
    WCHAR buffer[260];
    struct unicode u;
    void *base = 0;
    if (name && !widen(&u, buffer, 260, name))
        return 0;
    return LdrGetDllHandle(0, 0, name ? &u : 0, &base) ? 0 : base;
}
void *__stdcall GetProcAddress(void *module, const char *name) {
    struct ansi a = {0, 0, 0};
    U32 ordinal = 0;
    void *address = 0;
    if ((U32)name >> 16) {
        while (name[a.length])
            a.length++;
        a.maximum = a.length + 1;
        a.buffer = name;
    } else
        ordinal = (U32)name;
    return LdrGetProcedureAddress(module, ordinal ? 0 : &a, ordinal, &address) ? 0 : address;
}
int __stdcall FreeLibrary(void *module) {
    return !LdrUnloadDll(module);
}
/* Walk PEB->Ldr->InLoadOrderModuleList, as kernel32 does, which pins the
 * LDR_DATA_TABLE_ENTRY layout (DllBase 0x18, FullDllName 0x24). */
U32 __stdcall GetModuleFileNameA(void *module, char *out, U32 size) {
    unsigned char *peb = *(unsigned char **)((unsigned char *)NtCurrentTeb() + 0x30);
    unsigned char *ldr = *(unsigned char **)(peb + 0x0c);
    if (!module)
        module = *(void **)(peb + 0x08);
    struct list *head = (struct list *)(ldr + 0x0c);
    for (struct list *e = head->next; e != head; e = e->next) {
        unsigned char *entry = (unsigned char *)e;
        if (*(void **)(entry + 0x18) != module)
            continue;
        struct unicode *full = (struct unicode *)(entry + 0x24);
        U32 n = full->length / 2, i = 0;
        if (!size)
            return 0;
        for (; i < n && i + 1 < size; i++)
            out[i] = (char)full->buffer[i];
        out[i] = 0;
        return i;
    }
    return 0;
}
