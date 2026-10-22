/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include <w32/ntdll.h>
static ULONG wlen(const WCHAR *s) {
    ULONG n = 0;
    if (s)
        while (s[n])
            n++;
    return n;
}
void W32API RtlInitAnsiString(ANSI_STRING *d, const char *s) {
    ULONG n = w32_strlen(s);
    if (n > 65534)
        n = 65534;
    d->Buffer = (char *)s;
    d->Length = n;
    d->MaximumLength = s ? n + 1 : 0;
}
void W32API RtlInitUnicodeString(UNICODE_STRING *d, const WCHAR *s) {
    ULONG n = wlen(s);
    if (n > 32766)
        n = 32766;
    d->Buffer = (WCHAR *)s;
    d->Length = n * 2;
    d->MaximumLength = s ? n * 2 + 2 : 0;
}
/* CP1252 is the initial English/Western ACP. Locale/DBCS tables are an f5-06
 * integration input; never mislabel a byte-preserving cast as UTF-8. */
static const WCHAR cp1252[32] = {0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021,
                                 0x2c6,  0x2030, 0x160,  0x2039, 0x152,  0x8d,   0x17d,  0x8f,
                                 0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
                                 0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};
static WCHAR decode(uint8_t c) {
    return c >= 0x80 && c <= 0x9f ? cp1252[c - 0x80] : c;
}
static uint8_t encode(WCHAR c) {
    for (unsigned i = 0; i < 32; i++)
        if (cp1252[i] == c)
            return i + 0x80;
    return c <= 255 ? (uint8_t)c : '?';
}
WCHAR w32_ansi_decode(uint8_t c) { return decode(c); }
uint8_t w32_ansi_encode(WCHAR c) { return encode(c); }
/* Rtl string buffers come from the process heap, as on Windows, so that
 * RtlFreeHeap(GetProcessHeap()) and RtlFree*String are interchangeable.
 * Before the process heap exists (early loader use) core metadata is used. */
static void *process_heap(void) {
    struct w32_teb *t = NtCurrentTeb();
    return t && t->ProcessEnvironmentBlock ? t->ProcessEnvironmentBlock->ProcessHeap : 0;
}
static void *allocate(ULONG n) {
    void *heap = process_heap();
    if (heap)
        return RtlAllocateHeap(heap, 0, n);
    const struct w32_services *s = w32_get_services();
    return s && s->alloc ? s->alloc(s->opaque, n) : 0;
}
NTSTATUS W32API RtlAnsiStringToUnicodeString(UNICODE_STRING *d, const ANSI_STRING *s,
                                             BOOLEAN alloc) {
    if (!d || !s || (s->Length && !s->Buffer) || s->Length > 32766)
        return STATUS_INVALID_PARAMETER;
    ULONG bytes = s->Length * 2;
    if (alloc) {
        d->Buffer = allocate(bytes + 2);
        if (!d->Buffer)
            return STATUS_NO_MEMORY;
        d->MaximumLength = bytes + 2;
    }
    if (d->MaximumLength < bytes + 2 || !d->Buffer)
        return STATUS_BUFFER_TOO_SMALL;
    for (ULONG i = 0; i < s->Length; i++)
        d->Buffer[i] = decode((uint8_t)s->Buffer[i]);
    d->Buffer[s->Length] = 0;
    d->Length = bytes;
    return 0;
}
NTSTATUS W32API RtlUnicodeStringToAnsiString(ANSI_STRING *d, const UNICODE_STRING *s,
                                             BOOLEAN alloc) {
    if (!d || !s || s->Length & 1 || (s->Length && !s->Buffer))
        return STATUS_INVALID_PARAMETER;
    ULONG bytes = s->Length / 2;
    if (alloc) {
        d->Buffer = allocate(bytes + 1);
        if (!d->Buffer)
            return STATUS_NO_MEMORY;
        d->MaximumLength = bytes + 1;
    }
    if (d->MaximumLength < bytes + 1 || !d->Buffer)
        return STATUS_BUFFER_TOO_SMALL;
    for (ULONG i = 0; i < bytes; i++)
        d->Buffer[i] = encode(s->Buffer[i]);
    d->Buffer[bytes] = 0;
    d->Length = bytes;
    return 0;
}
void W32API RtlFreeAnsiString(ANSI_STRING *s) {
    const struct w32_services *v = w32_get_services();
    void *heap = process_heap();
    if (s->Buffer && !(heap && RtlFreeHeap(heap, 0, s->Buffer)) && v && v->free)
        v->free(v->opaque, s->Buffer, s->MaximumLength);
    w32_fill(s, 0, sizeof(*s));
}
void W32API RtlFreeUnicodeString(UNICODE_STRING *s) {
    RtlFreeAnsiString((ANSI_STRING *)s);
}
WCHAR W32API RtlUpcaseUnicodeChar(WCHAR c) {
    if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7))
        return c - 32;
    if (c == 0xff)
        return 0x178;
    if (c == 0x153 || c == 0x161 || c == 0x17e)
        return c - 1;
    return c;
}
LONG W32API RtlCompareUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b,
                                    BOOLEAN ignore) {
    ULONG n = (a->Length < b->Length ? a->Length : b->Length) / 2;
    for (ULONG i = 0; i < n; i++) {
        WCHAR x = a->Buffer[i], y = b->Buffer[i];
        if (ignore) {
            x = RtlUpcaseUnicodeChar(x);
            y = RtlUpcaseUnicodeChar(y);
        }
        if (x != y)
            return (LONG)x - y;
    }
    return (LONG)a->Length - b->Length;
}
BOOLEAN W32API RtlEqualUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b,
                                     BOOLEAN ignore) {
    return a->Length == b->Length && !RtlCompareUnicodeString(a, b, ignore);
}
NTSTATUS W32API RtlAppendUnicodeStringToString(UNICODE_STRING *d, const UNICODE_STRING *s) {
    if (!d || !s || (d->Length | s->Length) & 1 || d->Length > d->MaximumLength ||
        s->Length > d->MaximumLength - d->Length)
        return STATUS_BUFFER_TOO_SMALL;
    w32_copy((uint8_t *)d->Buffer + d->Length, s->Buffer, s->Length);
    d->Length += s->Length;
    if (d->MaximumLength - d->Length >= 2)
        d->Buffer[d->Length / 2] = 0;
    return 0;
}
NTSTATUS W32API RtlUpcaseUnicodeString(UNICODE_STRING *d, const UNICODE_STRING *s, BOOLEAN alloc) {
    if (!d || !s || s->Length & 1)
        return STATUS_INVALID_PARAMETER;
    if (alloc) {
        d->Buffer = allocate(s->Length ? s->Length : 2);
        if (!d->Buffer)
            return STATUS_NO_MEMORY;
        d->MaximumLength = s->Length ? s->Length : 2;
    }
    if (d->MaximumLength < s->Length)
        return STATUS_BUFFER_TOO_SMALL;
    for (ULONG i = 0; i < s->Length / 2; i++)
        d->Buffer[i] = RtlUpcaseUnicodeChar(s->Buffer[i]);
    d->Length = s->Length;
    return 0;
}
ULONG W32API RtlGetFullPathName_U(const WCHAR *name, ULONG bytes, WCHAR *out, WCHAR **part) {
    WCHAR tmp[1024];
    ULONG n = wlen(name), len = 0, root = 0;
    if (part)
        *part = 0;
    if (!n || n > 900)
        return 0;
    struct w32_process_parameters *p = NtCurrentTeb()->ProcessEnvironmentBlock->ProcessParameters;
    const WCHAR *cwd = p ? p->CurrentDirectory.DosPath.Buffer : 0;
    if (name[0] == '\\' && name[1] == '\\') {
        tmp[len++] = '\\';
        tmp[len++] = '\\';
        root = 2;
        name += 2;
    } else if (n >= 3 && name[1] == ':' && (name[2] == '\\' || name[2] == '/')) {
        tmp[len++] = RtlUpcaseUnicodeChar(name[0]);
        tmp[len++] = ':';
        tmp[len++] = '\\';
        root = 3;
        name += 3;
    } else {
        if (n >= 2 && name[1] == ':') {
            /* Per-drive cwd tables belong to kernel32; preserve the current
             * drive, and use the root for another drive. */
            if (!cwd || RtlUpcaseUnicodeChar(cwd[0]) != RtlUpcaseUnicodeChar(name[0])) {
                tmp[len++] = RtlUpcaseUnicodeChar(name[0]);
                tmp[len++] = ':';
                tmp[len++] = '\\';
                root = 3;
            }
            name += 2;
        }
        if (!len) {
            if (cwd) {
                ULONG c = wlen(cwd);
                if (c > 900)
                    return 0;
                for (ULONG i = 0; i < c; i++)
                    tmp[len++] = cwd[i];
            } else {
                tmp[len++] = 'C';
                tmp[len++] = ':';
                tmp[len++] = '\\';
            }
            root = 3;
        }
        if (*name == '\\' || *name == '/') {
            len = root;
            name++;
        } else if (tmp[len - 1] != '\\')
            tmp[len++] = '\\';
    }
    while (*name) {
        while (*name == '\\' || *name == '/')
            name++;
        const WCHAR *s = name;
        while (*name && *name != '\\' && *name != '/')
            name++;
        ULONG count = name - s;
        if (!count)
            break;
        if (count == 1 && *s == '.')
            continue;
        if (count == 2 && s[0] == '.' && s[1] == '.') {
            if (len > root) {
                if (tmp[len - 1] == '\\')
                    len--;
                while (len > root && tmp[len - 1] != '\\')
                    len--;
            }
            continue;
        }
        if (len && tmp[len - 1] != '\\')
            tmp[len++] = '\\';
        if (len + count >= 1023)
            return 0;
        for (ULONG i = 0; i < count; i++)
            tmp[len++] = s[i];
    }
    if (len > root && tmp[len - 1] == '\\')
        len--;
    tmp[len] = 0;
    if (bytes < (len + 1) * 2 || !out)
        return (len + 1) * 2;
    w32_copy(out, tmp, (len + 1) * 2);
    if (part && len > root) {
        ULONG i = len;
        while (i && out[i - 1] != '\\')
            i--;
        *part = out + i;
    }
    return len * 2;
}
BOOLEAN W32API RtlDosPathNameToNtPathName_U(const WCHAR *name, UNICODE_STRING *out, WCHAR **part,
                                            void *relative) {
    WCHAR full[1024], *file = 0;
    if (relative || !out)
        return 0;
    ULONG n = RtlGetFullPathName_U(name, sizeof(full), full, &file);
    if (!n || n >= sizeof(full))
        return 0;
    const WCHAR prefix[] = {'\\', '?', '?', '\\'};
    ULONG skip = full[0] == '\\' && full[1] == '\\' ? 2 : 0, extra = skip ? 8 : 4;
    out->MaximumLength = n - skip * 2 + extra * 2 + 2;
    out->Buffer = allocate(out->MaximumLength);
    if (!out->Buffer)
        return 0;
    w32_copy(out->Buffer, prefix, 8);
    if (skip) {
        out->Buffer[4] = 'U';
        out->Buffer[5] = 'N';
        out->Buffer[6] = 'C';
        out->Buffer[7] = '\\';
    }
    w32_copy(out->Buffer + extra, full + skip, n + 2 - skip * 2);
    out->Length = out->MaximumLength - 2;
    if (part)
        *part = file ? out->Buffer + extra + (file - full) - skip : 0;
    return 1;
}
/* f5-20A: string services reached by the reused Wine 10.0 DLLs. The ANSI
 * code page is CP1252 (single byte), so sizes are one byte per character. */
BOOLEAN W32API RtlCreateUnicodeStringFromAsciiz(UNICODE_STRING *d, const char *s) {
    if (!d || !s)
        return 0;
    ANSI_STRING a;
    RtlInitAnsiString(&a, s);
    if (w32_strlen(s) != a.Length)
        return 0;
    return !RtlAnsiStringToUnicodeString(d, &a, 1);
}
NTSTATUS W32API RtlUnicodeToMultiByteN(char *d, ULONG cap, ULONG *done, const WCHAR *s,
                                       ULONG bytes) {
    ULONG n = bytes / 2;
    if (n > cap)
        n = cap;
    if (n && (!d || !s))
        return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < n; i++)
        d[i] = (char)encode(s[i]);
    if (done)
        *done = n;
    return 0;
}
NTSTATUS W32API RtlUnicodeToMultiByteSize(ULONG *size, const WCHAR *s, ULONG bytes) {
    (void)s;
    if (!size)
        return STATUS_INVALID_PARAMETER;
    *size = bytes / 2;
    return 0;
}
static int hex(WCHAR c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                          : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
/* Exactly "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}", hex digits of either
 * case; the GUID is published only after the whole string parsed. */
NTSTATUS W32API RtlGUIDFromString(const UNICODE_STRING *s, void *guid) {
    if (!s || !guid || s->Length != 76 || !s->Buffer)
        return STATUS_INVALID_PARAMETER;
    const WCHAR *p = s->Buffer;
    if (p[0] != '{' || p[37] != '}' || p[9] != '-' || p[14] != '-' || p[19] != '-' || p[24] != '-')
        return STATUS_INVALID_PARAMETER;
    static const uint8_t at[16] = {1, 3, 5, 7, 10, 12, 15, 17, 20, 22, 25, 27, 29, 31, 33, 35};
    uint8_t raw[16], out[16];
    for (unsigned i = 0; i < 16; i++) {
        int h = hex(p[at[i]]), l = hex(p[at[i] + 1]);
        if (h < 0 || l < 0)
            return STATUS_INVALID_PARAMETER;
        raw[i] = (uint8_t)(h << 4 | l);
    }
    /* Data1, Data2 and Data3 are little-endian integers; Data4 is bytes. */
    static const uint8_t order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (unsigned i = 0; i < 16; i++)
        out[i] = raw[order[i]];
    w32_copy(guid, out, 16);
    return 0;
}
/* IS_TEXT_UNICODE_* tests as documented for RtlIsTextUnicode: each passed
 * test sets its flag; reverse or not-Unicode evidence wins, then not-ASCII
 * evidence, then Unicode evidence. */
BOOLEAN W32API RtlIsTextUnicode(const void *buffer, int length, int *flags) {
    const uint8_t *b = buffer;
    if (!buffer || length < 2) {
        if (flags)
            *flags = 0;
        return 0;
    }
    ULONG tests = flags ? (ULONG)*flags : ~0u, out = 0, n = (ULONG)length / 2;
    ULONG zero_high = 0, zero_low = 0, ascii = 1, reversed = 1;
    if (length & 1)
        out |= 0x200;
    for (ULONG i = 0; i < n; i++) {
        WCHAR c = (WCHAR)(b[i * 2] | b[i * 2 + 1] << 8);
        uint8_t low = (uint8_t)c, high = (uint8_t)(c >> 8);
        if (!i && c == 0xfeff)
            out |= 0x8;
        if (!i && c == 0xfffe)
            out |= 0x80;
        if (c == 0xffff || (i && (c == 0xfffe || c == 0xfeff)) || c == 0x0a0d)
            out |= 0x100;
        if (c == 0x0d || c == 0x0a || c == 0x09 || c == 0x20 || c == 0x3000)
            out |= 0x4;
        if (c == 0x0d00 || c == 0x0a00 || c == 0x0900 || c == 0x2000 || c == 0x0030)
            out |= 0x40;
        zero_high += !high && low;
        zero_low += !low && high;
        if (high || !low)
            ascii = 0;
        if (low || !high)
            reversed = 0;
    }
    for (int i = 0; i < length; i++)
        if (!b[i]) {
            out |= 0x1000;
            break;
        }
    if (ascii && !(out & 0x8))
        out |= 0x1;
    if (reversed && !(out & 0x80))
        out |= 0x10;
    if (zero_high * 2 > n && zero_low * 4 < n)
        out |= 0x2;
    if (zero_low * 2 > n && zero_high * 4 < n)
        out |= 0x20;
    out &= tests;
    if (flags)
        *flags = (int)out;
    if (out & 0x0ff0)
        return 0;
    return (out & 0xf00f) != 0;
}
