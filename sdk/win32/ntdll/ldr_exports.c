/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include <w32/ldr.h>
static struct w32_loader *loader;
void w32_ldr_bind(struct w32_loader *l) {
    loader = l;
}
static struct w32_module *handle(void *base) {
    /* NT tags LOAD_LIBRARY_AS_DATAFILE handles with bit 0; accept both. */
    base = (void *)((uintptr_t)base & ~(uintptr_t)3);
    if (loader)
        for (struct w32_module *m = loader->modules; m; m = m->next)
            if (m->pe.image == base)
                return m;
    return 0;
}
static NTSTATUS result(void) {
    switch (loader->error.code) {
    case 8:
        return STATUS_NO_MEMORY;
    case 126:
        return STATUS_DLL_NOT_FOUND;
    case 127:
        return STATUS_ENTRYPOINT_NOT_FOUND;
    case 1114:
        return STATUS_DLL_INIT_FAILED;
    case 193:
        return (NTSTATUS)0xc000007b;
    case 120:
        return STATUS_NOT_IMPLEMENTED;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}
static int narrow(char *out, const UNICODE_STRING *s) {
    if (!s || s->Length & 1 || s->Length / 2 >= W32_PATH_MAX || (!s->Buffer && s->Length))
        return 0;
    return w32_utf16_path(s->Buffer,s->Length/2,out,W32_PATH_MAX);
}
NTSTATUS W32API LdrLoadDll(const WCHAR *path, ULONG *flags, UNICODE_STRING *name, void **base) {
    (void)path;
    char text[W32_PATH_MAX];
    if (base)
        *base = 0;
    if (!loader || !base || !narrow(text, name))
        return STATUS_INVALID_PARAMETER;
    /* SearchPath is an optional semicolon list for this call, restored while
     * still holding the recursive lock. ASCII/ACP paths are the first target. */
    char search[W32_PATH_MAX];
    const char *saved = loader->path;
    RtlEnterCriticalSection(&loader->lock);
    if (path) {
        UNICODE_STRING p;
        RtlInitUnicodeString(&p, path);
        if (!narrow(search, &p)) {
            RtlLeaveCriticalSection(&loader->lock);
            return STATUS_INVALID_PARAMETER;
        }
        loader->path = search;
    }
    struct w32_module *m;
    int ok = w32_ldr_load(loader, text, flags ? *flags : 0, &m);
    loader->path = saved;
    if (ok)
        *base = m->pe.image;
    NTSTATUS status = ok ? 0 : result();
    RtlLeaveCriticalSection(&loader->lock);
    return status;
}
NTSTATUS W32API LdrUnloadDll(void *base) {
    struct w32_module *m = handle(base);
    if (!m)
        return STATUS_DLL_NOT_FOUND;
    return w32_ldr_free(loader, m) ? 0 : STATUS_INVALID_PARAMETER;
}
NTSTATUS W32API LdrGetDllHandle(const WCHAR *path, ULONG *flags, UNICODE_STRING *name,
                                void **base) {
    (void)path;
    (void)flags;
    char text[W32_PATH_MAX];
    if (base)
        *base = 0;
    if (!loader || !base || (name && !narrow(text, name)))
        return STATUS_INVALID_PARAMETER;
    RtlEnterCriticalSection(&loader->lock);
    struct w32_module *m = w32_ldr_module(loader, name ? text : 0);
    if (m)
        *base = m->pe.image;
    RtlLeaveCriticalSection(&loader->lock);
    return m ? 0 : STATUS_DLL_NOT_FOUND;
}
NTSTATUS W32API LdrGetProcedureAddress(void *base, ANSI_STRING *name, ULONG ordinal,
                                       void **address) {
    if (address)
        *address = 0;
    struct w32_module *m = handle(base);
    if (!m || !address)
        return STATUS_INVALID_PARAMETER;
    char text[256];
    const char *symbol = 0;
    if (name) {
        if (name->Length >= sizeof(text) || (!name->Buffer && name->Length))
            return STATUS_INVALID_PARAMETER;
        w32_copy(text, name->Buffer, name->Length);
        text[name->Length] = 0;
        symbol = text;
    }
    *address = w32_ldr_proc(loader, m, symbol, ordinal, 0xffffffffu);
    return *address ? 0 : result();
}
NTSTATUS W32API LdrDisableThreadCalloutsForDll(void *base) {
    struct w32_module *m = handle(base);
    if (!m)
        return STATUS_DLL_NOT_FOUND;
    if (m->tls_index != 0xffffffffu)
        return STATUS_INVALID_PARAMETER;
    m->disable_threads = 1;
    return 0;
}
NTSTATUS W32API LdrLockLoaderLock(ULONG flags, ULONG *disposition, ULONG *cookie) {
    if (!loader || !cookie || flags & ~3u)
        return STATUS_INVALID_PARAMETER;
    int acquired = (flags & 2) ? RtlTryEnterCriticalSection(&loader->lock)
                               : !RtlEnterCriticalSection(&loader->lock);
    if (disposition)
        *disposition = acquired ? 1 : 2;
    *cookie = acquired ? loader->services->thread_id(loader->services->opaque) : 0;
    return 0;
}
NTSTATUS W32API LdrUnlockLoaderLock(ULONG flags, ULONG cookie) {
    if (!loader || flags & ~1u || !cookie ||
        cookie != loader->services->thread_id(loader->services->opaque))
        return STATUS_INVALID_PARAMETER;
    return RtlLeaveCriticalSection(&loader->lock);
}
/* Resources. Type and Name are integer ids below 0x10000 or pointers to
 * NUL-terminated WCHAR names (compared exactly; kernel32 upcases first). */
struct w32_resource_info {
    uintptr_t Type, Name, Language; /* ULONG_PTR */
};
static struct w32_resource_key key(uintptr_t v) {
    struct w32_resource_key k = {0, 0};
    if (v >> 16)
        k.name = (const WCHAR *)(uintptr_t)v;
    else
        k.id = (ULONG)v;
    return k;
}
#define LANG_PRIMARY(l) ((l) & 0x3ff)
/* Language fallback of the era: requested, primary/neutral, neutral, the
 * user default (US English until f5-06 supplies the locale), any. */
static NTSTATUS find(void *base, const struct w32_resource_info *info, ULONG level, void **out) {
    if (out)
        *out = 0;
    if (!loader || !info || !out || !level || level > 3)
        return STATUS_INVALID_PARAMETER;
    RtlEnterCriticalSection(&loader->lock);
    struct w32_module *m = handle(base);
    NTSTATUS status = (NTSTATUS)0xc000008a; /* STATUS_RESOURCE_TYPE_NOT_FOUND */
    if (!m)
        status = STATUS_INVALID_PARAMETER;
    else if (m->pe.dirs[2].size) {
        struct w32_resource_key keys[3] = {key(info->Type), key(info->Name), {0, 0}};
        if (level < 3) {
            if (w32_pe_resource_lookup(&m->pe, keys, level, out))
                status = 0;
            else if (level == 2 && w32_pe_resource_lookup(&m->pe, keys, 1, out))
                status = (NTSTATUS)0xc000008b; /* STATUS_RESOURCE_NAME_NOT_FOUND */
            *out = status ? 0 : *out;
        } else {
            ULONG lang = info->Language & 0xffff;
            const ULONG order[] = {lang, LANG_PRIMARY(lang), 0, 0x409, 9, 0xffffffffu};
            status = (NTSTATUS)0xc000008b;
            if (!w32_pe_resource_lookup(&m->pe, keys, 1, out))
                status = (NTSTATUS)0xc000008a;
            else if (w32_pe_resource_lookup(&m->pe, keys, 2, out)) {
                status = (NTSTATUS)0xc0000204; /* STATUS_RESOURCE_LANG_NOT_FOUND */
                for (ULONG i = 0; i < 6 && status; i++) {
                    if (i && order[i] == order[i - 1])
                        continue;
                    keys[2].id = order[i];
                    if (w32_pe_resource_lookup(&m->pe, keys, 3, out))
                        status = 0;
                }
            }
            if (status)
                *out = 0;
        }
    }
    RtlLeaveCriticalSection(&loader->lock);
    return status;
}
NTSTATUS W32API LdrFindResource_U(void *base, const struct w32_resource_info *info, ULONG level,
                                  void **entry) {
    return find(base, info, level, entry);
}
NTSTATUS W32API LdrFindResourceDirectory_U(void *base, const struct w32_resource_info *info,
                                           ULONG level, void **directory) {
    if (level >= 3)
        return STATUS_INVALID_PARAMETER;
    return find(base, info, level, directory);
}
NTSTATUS W32API LdrAccessResource(void *base, const void *entry, void **address, ULONG *size) {
    if (!loader || !entry || !address)
        return STATUS_INVALID_PARAMETER;
    RtlEnterCriticalSection(&loader->lock);
    struct w32_module *m = handle(base);
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    if (m) {
        uintptr_t e = (uintptr_t)entry, lo = (uintptr_t)m->pe.image + m->pe.dirs[2].rva;
        if (e >= lo && m->pe.dirs[2].size >= 16 && e - lo <= m->pe.dirs[2].size - 16) {
            ULONG rva = w32_u32(entry), n = w32_u32((const uint8_t *)entry + 4);
            void *data = w32_pe_span(&m->pe, rva, n);
            if (data) {
                *address = data;
                if (size)
                    *size = n;
                status = 0;
            }
        }
    }
    RtlLeaveCriticalSection(&loader->lock);
    return status;
}
/* Image helpers used by the reused DLLs and kernel32. Loaded modules are
 * bounds-checked; other mappings get the Windows header checks only. */
void *W32API RtlImageNtHeader(void *base) {
    if (!base)
        return 0;
    struct w32_module *m = loader ? handle(base) : 0;
    const uint8_t *b = base;
    if (b[0] != 'M' || b[1] != 'Z')
        return 0;
    ULONG off = w32_u32(b + 60);
    if (off >= 0x10000000 || (m && (m->pe.headers < 248 || off > m->pe.headers - 248)))
        return 0;
    return w32_u32(b + off) == 0x4550 ? (void *)(b + off) : 0;
}
void *W32API RtlImageDirectoryEntryToData(void *base, BOOLEAN image, uint16_t index, ULONG *size) {
    uint8_t *nt = RtlImageNtHeader((void *)((uintptr_t)base & ~(uintptr_t)3));
    if (size)
        *size = 0;
    if (!nt || w32_u16(nt + 24) != 0x10b || index >= w32_u32(nt + 24 + 92))
        return 0;
    ULONG rva = w32_u32(nt + 24 + 96 + index * 8), n = w32_u32(nt + 24 + 100 + index * 8);
    if (!rva)
        return 0;
    uint8_t *b = (uint8_t *)((uintptr_t)base & ~(uintptr_t)3);
    if (!image && index != 4) {
        /* A file mapped as data: translate the RVA through the sections. */
        ULONG count = w32_u16(nt + 6), table = 24 + w32_u16(nt + 20), found = 0;
        for (ULONG i = 0; i < count && !found; i++) {
            const uint8_t *t = nt + table + i * 40;
            ULONG va = w32_u32(t + 12), raw = w32_u32(t + 16), span = w32_u32(t + 8);
            if (span < raw)
                span = raw;
            if (rva >= va && rva - va < span && rva - va < raw) {
                rva = rva - va + w32_u32(t + 20);
                found = 1;
            }
        }
        if (!found && rva >= w32_u32(nt + 24 + 60))
            return 0;
    }
    if (size)
        *size = n;
    return b + rva;
}

int W32API CiukiThreadAttach(void){return loader&&w32_ldr_thread_attach(loader,NtCurrentTeb());}
void W32API CiukiThreadDetach(void){if(loader)w32_ldr_thread_detach(loader,NtCurrentTeb());}
void W32API CiukiProcessDetach(void){if(loader)w32_ldr_shutdown(loader);}
