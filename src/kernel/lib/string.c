/* Freestanding string routines (clang may emit calls to these).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp == sp || n == 0)
        return d;
    if (dp < sp) {
        while (n--)
            *dp++ = *sp++;
    } else {
        dp += n;
        sp += n;
        while (n--)
            *--dp = *--sp;
    }
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *dp = d;
    while (n--)
        *dp++ = (unsigned char)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x < *y ? -1 : 1;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        if (!*a)
            return 0;
    }
    return 0;
}

uint32_t fnv1a32(const void *data, size_t len, uint32_t seed)
{
    const unsigned char *p = data;
    uint32_t h = seed ? seed : 2166136261u;
    while (len--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}
