/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Original implementation. */
#include <w32/ntdll.h>
static const struct w32_services *services;
void w32_set_services(const struct w32_services *s) {
    services = s;
}
const struct w32_services *w32_get_services(void) {
    return services;
}
void *w32_copy(void *d, const void *s, uint32_t n) {
    uint8_t *p = d;
    const uint8_t *q = s;
    if ((uintptr_t)p > (uintptr_t)q && (uintptr_t)p - (uintptr_t)q < n)
        while (n) {
            --n;
            p[n] = q[n];
        }
    else
        for (uint32_t i = 0; i < n; i++)
            p[i] = q[i];
    return d;
}
void *w32_fill(void *d, uint8_t v, uint32_t n) {
    uint8_t *p = d;
    for (uint32_t i = 0; i < n; i++)
        p[i] = v;
    return d;
}
uint32_t w32_strlen(const char *s) {
    uint32_t n = 0;
    if (s)
        while (s[n])
            n++;
    return n;
}
int w32_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}
static unsigned lower(unsigned c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
int w32_stricmp(const char *a, const char *b) {
    while (*a && lower((uint8_t)*a) == lower((uint8_t)*b)) {
        a++;
        b++;
    }
    return (int)lower((uint8_t)*a) - (int)lower((uint8_t)*b);
}
int w32_copy_string(char *d, uint32_t n, const char *s) {
    uint32_t len = w32_strlen(s);
    if (len >= n)
        return 0;
    w32_copy(d, s, len + 1);
    return 1;
}
uint16_t w32_u16(const void *v) {
    const uint8_t *p = v;
    return p[0] | (uint16_t)p[1] << 8;
}
uint32_t w32_u32(const void *v) {
    const uint8_t *p = v;
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
void w32_put16(void *v, uint16_t n) {
    uint8_t *p = v;
    p[0] = n;
    p[1] = n >> 8;
}
void w32_put32(void *v, uint32_t n) {
    uint8_t *p = v;
    for (unsigned i = 0; i < 4; i++)
        p[i] = n >> (8 * i);
}
void w32_diag(const char *event, const char *module, const char *symbol) {
    if (services && services->diagnostic)
        services->diagnostic(services->opaque, event, module, symbol);
}
W32_NORETURN void w32_terminate(uint32_t code) {
    if (services && services->terminate)
        services->terminate(services->opaque, code);
    __builtin_trap();
}
void w32_stub_hit(volatile ULONG *once, const char *dll, const char *name) {
    if (!__atomic_exchange_n(once, 1, __ATOMIC_RELAXED))
        w32_diag("stub", dll, name);
    NtCurrentTeb()->LastErrorValue=120;
    RtlRaiseStatus(STATUS_NOT_IMPLEMENTED);
}
/* Compiler-emitted freestanding aggregate copies have these cdecl names. */
void *memcpy(void *d, const void *s, size_t n) {
    return w32_copy(d, s, n);
}
void *memmove(void *d, const void *s, size_t n) {
    return w32_copy(d, s, n);
}
void *memset(void *d, int c, size_t n) {
    return w32_fill(d, (uint8_t)c, n);
}
ULONG W32API RtlNtStatusToDosError(NTSTATUS s) {
    switch (s) {
    case STATUS_SUCCESS:
        return 0;
    case (NTSTATUS)0xc0000001: return 31;
    case (NTSTATUS)0xc0000005: return 998;
    case (NTSTATUS)0xc0000018: return 487;
    case (NTSTATUS)0xc0000022: return 5;
    case (NTSTATUS)0xc0000034: return 2;
    case (NTSTATUS)0xc000007b: return 193;
    case (NTSTATUS)0x80000011: return 170;
    case (NTSTATUS)0x8000001a: return 259;
    case STATUS_NO_MEMORY:
        return 8;
    case STATUS_INVALID_PARAMETER:
        return 87;
    case STATUS_BUFFER_TOO_SMALL:
        return 122;
    case STATUS_DLL_NOT_FOUND:
        return 126;
    case STATUS_ENTRYPOINT_NOT_FOUND:
        return 127;
    case STATUS_DLL_INIT_FAILED:
        return 1114;
    case STATUS_NOT_IMPLEMENTED:
        return 120;
    default:
        return 317;
    }
}
