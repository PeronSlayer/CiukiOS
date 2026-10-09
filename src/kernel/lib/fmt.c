/* Minimal bounded formatter: %s %c %d %u %x %X %08x %p %llu %llx %%.
 * Width and '0' flag supported for integers. Always NUL-terminates.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>

struct out {
    char *buf;
    size_t size, len;
};

static void put(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void put_num(struct out *o, uint64_t v, unsigned base, int width, char pad, bool upper, bool neg)
{
    char tmp[24];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v && n < (int)sizeof(tmp));
    if (neg)
        tmp[n++] = '-';
    while (n < width) {
        put(o, pad);
        width--;
    }
    while (n)
        put(o, tmp[--n]);
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0, lng = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') {
            lng++;
            fmt++;
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int l = (int)strlen(s);
            while (l < width) { put(&o, ' '); width--; }
            while (*s)
                put(&o, *s++);
            break;
        }
        case 'c':
            put(&o, (char)va_arg(ap, int));
            break;
        case 'd': {
            int64_t v = lng >= 2 ? va_arg(ap, int64_t) : va_arg(ap, int);
            put_num(&o, v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, 10, width, pad, false, v < 0);
            break;
        }
        case 'u': {
            uint64_t v = lng >= 2 ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            put_num(&o, v, 10, width, pad, false, false);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t v = lng >= 2 ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            put_num(&o, v, 16, width, pad, *fmt == 'X', false);
            break;
        }
        case 'p':
            put_num(&o, (uintptr_t)va_arg(ap, void *), 16, 8, '0', false, false);
            break;
        case '%':
            put(&o, '%');
            break;
        default:
            put(&o, '%');
            if (*fmt)
                put(&o, *fmt);
            else
                fmt--;
            break;
        }
    }
    if (size)
        o.buf[o.len < size ? o.len : size - 1] = 0;
    return (int)o.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
