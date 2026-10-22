/* f5-15C: NTDLL's CRT formatters _vsnprintf and _vsnwprintf, which the reused
 * Wine shlwapi.dll resolves from ntdll (wnsprintf). Semantics follow Wine
 * 10.0 dlls/ntdll/printf.c and printf.h (LGPL-2.1-or-later), rewritten here:
 * - the output object {buffer, length, used}: a piece that does not fit is
 *   copied up to the remaining space, `used` becomes the length and the call
 *   returns -1 at once; a NULL buffer only counts and never overflows;
 * - the terminator is written only when used < length, so an exact fit
 *   returns the length without a NUL (the documented CRT behaviour);
 * - flags + space - 0 #, width and precision (digits or *), size prefixes
 *   h l w ll I64 j (64-bit) I z t (pointer size) I32 L F (ignored);
 * - conversions c C s S d i o u x X p n %; no floating point (an unknown
 *   conversion character is copied as itself); positional `$` returns -1;
 * - a NULL string prints "(null)"; %p is upper-case hex, zero-padded to two
 *   digits per pointer byte; narrow/wide strings convert through the ANSI
 *   code page (RtlUnicodeToMultiByteN in Wine; CP1252 here, strings.c).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/ntdll.h>
#include <stdarg.h>

struct pf_out {
    void *buffer;
    ULONG length, used;
    int wide;
};
struct pf_spec {
    int left, zero, alternate, sign, width, precision, size, wide_arg;
};
enum { PF_DEFAULT, PF_SHORT, PF_LONG, PF_INT64, PF_NATIVE };

static int put(struct pf_out *o, unsigned unit) {
    if (o->buffer) {
        if (o->used >= o->length) {
            o->used = o->length;
            return -1;
        }
        if (o->wide)
            ((WCHAR *)o->buffer)[o->used] = (WCHAR)unit;
        else
            ((char *)o->buffer)[o->used] = (char)unit;
    }
    o->used++;
    return 0;
}
static int put_narrow(struct pf_out *o, uint8_t c) {
    return put(o, o->wide ? (unsigned)w32_ansi_decode(c) : c);
}
static int put_wide(struct pf_out *o, WCHAR c) {
    return put(o, o->wide ? (unsigned)c : (unsigned)w32_ansi_encode(c));
}
static int pad(struct pf_out *o, int narrow, int count) {
    if (count <= 0)
        return 0;
    if (!o->buffer) {
        o->used += (ULONG)count;
        return 0;
    }
    while (count-- > 0)
        if (put_narrow(o, (uint8_t)narrow))
            return -1;
    return 0;
}
/* 64-bit by small-divisor division in 32-bit steps: the core links no
 * compiler runtime for _aullrem. Returns the remainder. */
static unsigned divide(uint64_t *v, unsigned base) {
    uint32_t hi = (uint32_t)(*v >> 32), lo = (uint32_t)*v;
    uint32_t qh = hi / base, r = hi % base;
    uint32_t mid = (r << 16) | (lo >> 16), qm = mid / base;
    r = mid % base;
    uint32_t low = (r << 16) | (lo & 0xffffu), ql = low / base;
    r = low % base;
    *v = ((uint64_t)qh << 32) | ((uint64_t)qm << 16) | ql;
    return r;
}
static int field_text(struct pf_out *o, const struct pf_spec *s, const void *text, int wide_text) {
    static const char null_text[] = "(null)";
    if (!text) {
        text = null_text;
        wide_text = 0;
    }
    int n = 0;
    while ((s->precision < 0 || n < s->precision) &&
           (wide_text ? ((const WCHAR *)text)[n] : ((const char *)text)[n]))
        n++;
    if (!s->left && pad(o, s->zero ? '0' : ' ', s->width - n))
        return -1;
    for (int i = 0; i < n; i++)
        if (wide_text ? put_wide(o, ((const WCHAR *)text)[i])
                      : put_narrow(o, ((const uint8_t *)text)[i]))
            return -1;
    return s->left ? pad(o, ' ', s->width - n) : 0;
}
static int field_integer(struct pf_out *o, struct pf_spec *s, unsigned conversion, uint64_t v,
                         int negative) {
    char digits[24];
    int count = 0;
    unsigned base = conversion == 'o' ? 8 : conversion == 'x' || conversion == 'X' ? 16 : 10;
    const char *set = conversion == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    int zero_value = !v;
    if (!(zero_value && s->precision == 0))
        do {
            if (base == 10)
                digits[count++] = set[divide(&v, 10)];
            else {
                digits[count++] = set[(unsigned)v & (base - 1)];
                v >>= base == 8 ? 3 : 4;
            }
        } while (v);
    char prefix[3];
    int prefix_length = 0;
    if (conversion == 'd' || conversion == 'i') {
        if (negative)
            prefix[prefix_length++] = '-';
        else if (s->sign)
            prefix[prefix_length++] = (char)s->sign;
    }
    if (s->alternate && base == 16 && !zero_value) {
        prefix[prefix_length++] = '0';
        prefix[prefix_length++] = (char)(conversion == 'X' ? 'X' : 'x');
    }
    int zeros = s->precision > count ? s->precision - count : 0;
    if (s->alternate && base == 8 && !zeros && (!count || digits[count - 1] != '0'))
        zeros = 1;
    int body = prefix_length + zeros + count;
    if (s->zero && !s->left && s->precision < 0 && s->width > body) {
        zeros += s->width - body;
        body = s->width;
    }
    if (!s->left && pad(o, ' ', s->width - body))
        return -1;
    for (int i = 0; i < prefix_length; i++)
        if (put_narrow(o, (uint8_t)prefix[i]))
            return -1;
    if (pad(o, '0', zeros))
        return -1;
    while (count)
        if (put_narrow(o, (uint8_t)digits[--count]))
            return -1;
    return s->left ? pad(o, ' ', s->width - body) : 0;
}
#define UNIT(i) (format_wide ? ((const WCHAR *)format)[i] : (unsigned)((const uint8_t *)format)[i])
static int pf_format(struct pf_out *o, const void *format, int format_wide, va_list ap) {
    ULONG at = 0;
    for (unsigned c; (c = UNIT(at)) != 0; at++) {
        if (c != '%') {
            if (format_wide ? put_wide(o, (WCHAR)c) : put_narrow(o, (uint8_t)c))
                return -1;
            continue;
        }
        at++;
        struct pf_spec s = {0, 0, 0, 0, 0, -1, PF_DEFAULT, 0};
        for (;; at++) {
            c = UNIT(at);
            if (c == '-')
                s.left = 1;
            else if (c == '0')
                s.zero = 1;
            else if (c == '#')
                s.alternate = 1;
            else if (c == '+')
                s.sign = '+';
            else if (c == ' ') {
                if (!s.sign)
                    s.sign = ' ';
            } else
                break;
        }
        if (UNIT(at) == '*') {
            s.width = va_arg(ap, int);
            if (s.width < 0) {
                s.left = 1;
                s.width = s.width == (-2147483647 - 1) ? 2147483647 : -s.width;
            }
            at++;
        } else
            while (UNIT(at) >= '0' && UNIT(at) <= '9') {
                if (s.width < 214748364)
                    s.width = s.width * 10 + (int)(UNIT(at) - '0');
                at++;
            }
        if (UNIT(at) == '$')
            return -1; /* positional arguments: unsupported, as in Wine */
        if (UNIT(at) == '.') {
            at++;
            s.precision = 0;
            if (UNIT(at) == '*') {
                s.precision = va_arg(ap, int);
                if (s.precision < 0)
                    s.precision = -1;
                at++;
            } else
                while (UNIT(at) >= '0' && UNIT(at) <= '9') {
                    if (s.precision < 214748364)
                        s.precision = s.precision * 10 + (int)(UNIT(at) - '0');
                    at++;
                }
        }
        for (;;) {
            c = UNIT(at);
            if (c == 'l' && UNIT(at + 1) == 'l') {
                s.size = PF_INT64;
                at += 2;
            } else if (c == 'l' || c == 'w') {
                s.size = s.size == PF_INT64 ? PF_INT64 : PF_LONG;
                s.wide_arg = 1;
                at++;
            } else if (c == 'h') {
                s.size = PF_SHORT;
                at++;
            } else if (c == 'I' && UNIT(at + 1) == '6' && UNIT(at + 2) == '4') {
                s.size = PF_INT64;
                at += 3;
            } else if (c == 'I' && UNIT(at + 1) == '3' && UNIT(at + 2) == '2') {
                at += 3;
            } else if (c == 'I' || c == 'z' || c == 't') {
                s.size = PF_NATIVE;
                at++;
            } else if (c == 'j') {
                s.size = PF_INT64;
                at++;
            } else if (c == 'L' || c == 'F' || c == 'N') {
                at++;
            } else
                break;
        }
        c = UNIT(at);
        if (!c)
            break;
        /* The caller's own width for c/s; C/S invert it; h forces narrow,
         * l/w force wide (Wine pf_handle_string_format). */
        int wide_arg = s.size == PF_SHORT ? 0 : s.wide_arg ? 1 : (c == 'C' || c == 'S') != format_wide;
        int r = 0;
        switch (c) {
        case 'c':
        case 'C': {
            WCHAR text[2] = {0, 0};
            char narrow[2] = {0, 0};
            unsigned value = (unsigned)va_arg(ap, int);
            if (wide_arg)
                text[0] = (WCHAR)value;
            else
                narrow[0] = (char)value;
            /* A NUL character is still one output unit. */
            if (!s.left && pad(o, s.zero ? '0' : ' ', s.width - 1))
                return -1;
            r = wide_arg ? put_wide(o, text[0]) : put_narrow(o, (uint8_t)narrow[0]);
            if (!r && s.left)
                r = pad(o, ' ', s.width - 1);
            break;
        }
        case 's':
        case 'S':
            r = field_text(o, &s, va_arg(ap, const void *), wide_arg);
            break;
        case 'd':
        case 'i':
        case 'o':
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v;
            int negative = 0;
            if (s.size == PF_INT64)
                v = va_arg(ap, uint64_t);
            else if (s.size == PF_NATIVE)
                v = (uintptr_t)va_arg(ap, void *);
            else
                v = va_arg(ap, unsigned int);
            if (c == 'd' || c == 'i') {
                int64_t value = s.size == PF_INT64    ? (int64_t)v
                                : s.size == PF_SHORT  ? (int64_t)(int16_t)v
                                : s.size == PF_NATIVE ? (int64_t)(intptr_t)v
                                                      : (int64_t)(int32_t)(uint32_t)v;
                if (value < 0) {
                    negative = 1;
                    v = (uint64_t)(-(value + 1)) + 1;
                } else
                    v = (uint64_t)value;
            } else if (s.size == PF_SHORT)
                v &= 0xffff;
            r = field_integer(o, &s, c, v, negative);
            break;
        }
        case 'p': {
            uint64_t v = (uintptr_t)va_arg(ap, void *);
            struct pf_spec p = s;
            p.precision = (int)(2 * sizeof(void *));
            p.zero = 0;
            p.sign = 0;
            r = field_integer(o, &p, 'X', v, 0);
            break;
        }
        case 'n': {
            void *target = va_arg(ap, void *);
            if (s.size == PF_INT64)
                *(int64_t *)target = (int64_t)o->used;
            else if (s.size == PF_SHORT)
                *(int16_t *)target = (int16_t)o->used;
            else
                *(int *)target = (int)o->used;
            break;
        }
        default:
            /* %% and every unsupported conversion (f e g a ...) are copied. */
            r = format_wide ? put_wide(o, (WCHAR)c) : put_narrow(o, (uint8_t)c);
            break;
        }
        if (r)
            return -1;
    }
    return (int)o->used;
}
#undef UNIT
int _vsnprintf(char *buffer, ULONG length, const char *format, va_list ap) {
    struct pf_out o = {buffer, length, 0, 0};
    if (!format)
        return -1;
    int r = pf_format(&o, format, 0, ap);
    if (buffer && o.used < length)
        buffer[o.used] = 0;
    return r;
}
int _vsnwprintf(WCHAR *buffer, ULONG length, const WCHAR *format, va_list ap) {
    struct pf_out o = {buffer, length, 0, 1};
    if (!format)
        return -1;
    int r = pf_format(&o, format, 1, ap);
    if (buffer && o.used < length)
        buffer[o.used] = 0;
    return r;
}
