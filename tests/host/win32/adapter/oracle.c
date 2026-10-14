/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Self-authored NT-equal fixture.
 * Return 0 for success, a numbered failing check otherwise. No Windows SDK,
 * CRT, proprietary payload, or copied Wine test code. */
typedef unsigned long U32;
typedef unsigned short WCHAR;
typedef unsigned char BOOLEAN;
struct unicode {
    unsigned short length, maximum;
    WCHAR *buffer;
};
struct ansi {
    unsigned short length, maximum;
    char *buffer;
};
#define IMPORT __declspec(dllimport)
IMPORT void *__stdcall RtlCreateHeap(U32, void *, U32, U32, void *, void *);
IMPORT void *__stdcall RtlDestroyHeap(void *);
IMPORT void *__stdcall RtlAllocateHeap(void *, U32, U32);
IMPORT void *__stdcall RtlReAllocateHeap(void *, U32, void *, U32);
IMPORT BOOLEAN __stdcall RtlFreeHeap(void *, U32, void *);
IMPORT U32 __stdcall RtlSizeHeap(void *, U32, const void *);
IMPORT void __stdcall RtlInitAnsiString(struct ansi *, const char *);
IMPORT long __stdcall RtlAnsiStringToUnicodeString(struct unicode *, const struct ansi *, BOOLEAN);
IMPORT void __stdcall RtlFreeUnicodeString(struct unicode *);
IMPORT BOOLEAN __stdcall RtlValidateHeap(void *, U32, const void *);
IMPORT void __stdcall RtlInitUnicodeString(struct unicode *, const WCHAR *);
IMPORT long __stdcall RtlAppendUnicodeStringToString(struct unicode *, const struct unicode *);
IMPORT long __stdcall RtlCompareUnicodeString(const struct unicode *, const struct unicode *,
                                              BOOLEAN);
IMPORT long __stdcall RtlUpcaseUnicodeString(struct unicode *, const struct unicode *, BOOLEAN);
IMPORT U32 __stdcall RtlGetFullPathName_U(const WCHAR *, U32, WCHAR *, WCHAR **);
IMPORT BOOLEAN __stdcall RtlDosPathNameToNtPathName_U(const WCHAR *, struct unicode *, WCHAR **,
                                                      void *);
static int wide_equal(const WCHAR *a, const char *b) {
    while (*b && *a == (unsigned char)*b)
        a++, b++;
    return !*a && !*b;
}
/* Further NT-equal heap and Rtl behaviour (checks 20-49). */
static int more(void) {
    void *h = RtlCreateHeap(2, 0, 0, 0, 0, 0);
    if (!h)
        return 20;
    if (!RtlFreeHeap(h, 0, 0))
        return 21;
    void *zero = RtlAllocateHeap(h, 0, 0);
    if (!zero || RtlSizeHeap(h, 0, zero) != 0 || !RtlFreeHeap(h, 0, zero))
        return 22;
    static unsigned char *blocks[200];
    for (U32 i = 0; i < 200; i++) {
        U32 n = 1 + (i * 37) % 300;
        blocks[i] = RtlAllocateHeap(h, 0, n);
        if (!blocks[i] || (U32)blocks[i] & 7)
            return 23;
        for (U32 k = 0; k < n; k++)
            blocks[i][k] = (unsigned char)(i + k);
    }
    for (U32 i = 0; i < 200; i += 2)
        if (!RtlFreeHeap(h, 0, blocks[i]))
            return 24;
    for (U32 i = 1; i < 200; i += 2) {
        U32 n = 1 + (i * 37) % 300;
        unsigned char *q = RtlReAllocateHeap(h, 8, blocks[i], n + 500);
        if (!q)
            return 25;
        for (U32 k = 0; k < n; k++)
            if (q[k] != (unsigned char)(i + k))
                return 26;
        for (U32 k = n; k < n + 500; k++)
            if (q[k])
                return 27;
        blocks[i] = q;
    }
    if (!RtlValidateHeap(h, 0, 0))
        return 28;
    unsigned char *a = blocks[1];
    a[0] = 0x3c;
    if (RtlReAllocateHeap(h, 16, a, 8 << 20) || a[0] != 0x3c)
        return 29; /* HEAP_REALLOC_IN_PLACE_ONLY cannot grow by 8 MiB */
    unsigned char *big = RtlAllocateHeap(h, 8, 1 << 20);
    if (!big || big[12345] || RtlSizeHeap(h, 0, big) != 1 << 20 || !RtlFreeHeap(h, 0, big))
        return 30;
    for (U32 i = 1; i < 200; i += 2)
        if (!RtlFreeHeap(h, 0, blocks[i]))
            return 31;
    if (RtlDestroyHeap(h))
        return 32;
    static WCHAR left[32] = {'C', 'i', 'u', 'k', 'i', 0};
    static const WCHAR right_text[] = {'O', 'S', 0}, upper_text[] = {'C', 'I', 'U', 'K', 'I', 'O', 'S', 0};
    struct unicode l, r, up, cmp;
    RtlInitUnicodeString(&l, left);
    l.maximum = sizeof(left);
    RtlInitUnicodeString(&r, right_text);
    if (l.length != 10 || r.maximum != 6)
        return 33;
    if (RtlAppendUnicodeStringToString(&l, &r) || l.length != 14 || !wide_equal(left, "CiukiOS"))
        return 34;
    RtlInitUnicodeString(&cmp, upper_text);
    if (!RtlCompareUnicodeString(&l, &cmp, 0) || RtlCompareUnicodeString(&l, &cmp, 1))
        return 35;
    if (RtlUpcaseUnicodeString(&up, &l, 1) || up.length != 14 || RtlCompareUnicodeString(&up, &cmp, 0))
        return 36;
    RtlFreeUnicodeString(&up);
    static const WCHAR path[] = {'C', ':', '\\', 'g', 'a', 'm', 'e', '\\', '.', '.', '\\',
                                 's', 'a', 'v', 'e', '\\', '.', '\\', 'a', '.', 'd', 'a', 't', 0};
    WCHAR full[64], *part = 0;
    U32 bytes = RtlGetFullPathName_U(path, sizeof(full), full, &part);
    if (bytes != 26 || !wide_equal(full, "C:\\save\\a.dat") || part != full + 8)
        return 37;
    if (RtlGetFullPathName_U(path, 4, full, &part) != 28)
        return 38; /* too small: required size including the terminator */
    struct unicode nt;
    if (!RtlDosPathNameToNtPathName_U(path, &nt, &part, 0) ||
        !wide_equal(nt.buffer, "\\??\\C:\\save\\a.dat") || nt.length != 34 ||
        part != nt.buffer + 12)
        return 39;
    RtlFreeUnicodeString(&nt);
    return 0;
}
int entry(void) {
    void *h = RtlCreateHeap(2, 0, 0, 0, 0, 0);
    if (!h)
        return 1;
    unsigned char *p = RtlAllocateHeap(h, 8, 17);
    if (!p || (U32)p & 7)
        return 2;
    for (U32 i = 0; i < 17; i++)
        if (p[i])
            return 3;
    if (RtlSizeHeap(h, 0, p) != 17)
        return 4;
    p[0] = 0x5a;
    p = RtlReAllocateHeap(h, 8, p, 100);
    if (!p || p[0] != 0x5a)
        return 5;
    for (U32 i = 17; i < 100; i++)
        if (p[i])
            return 6;
    if (!RtlFreeHeap(h, 0, p))
        return 7;
    if (RtlDestroyHeap(h))
        return 8;
    struct ansi a;
    struct unicode u;
    RtlInitAnsiString(&a, "Ciuki");
    if (RtlAnsiStringToUnicodeString(&u, &a, 1))
        return 9;
    if (u.length != 10 || u.buffer[0] != 'C' || u.buffer[4] != 'i' || u.buffer[5])
        return 10;
    RtlFreeUnicodeString(&u);
    return more();
}
