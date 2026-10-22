/* f5-15C: production ntdll _vsnprintf/_vsnwprintf (sdk/win32/ntdll/printf.c)
 * against the Wine 10.0 dlls/ntdll/printf.h semantics: partial copy and -1
 * on overflow, no terminator on an exact fit, NULL buffer counting, the
 * CRT flag/width/precision/size grammar, (null), %p, %n, unsupported float
 * conversions copied, positional arguments refused. ASan/UBSan.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/ntdll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(c)                                                                  \
    do {                                                                          \
        checks++;                                                                 \
        if (!(c)) {                                                               \
            fprintf(stderr, "ntdll printf line %u: %s\n", __LINE__, #c);          \
            abort();                                                              \
        }                                                                         \
    } while (0)
/* strings.c links against these core helpers; only the code page tables are used. */
uint32_t w32_strlen(const char *s) { return s ? (uint32_t)strlen(s) : 0; }
void *w32_copy(void *d, const void *s, uint32_t n) { return memmove(d, s, n); }
void *w32_fill(void *d, uint8_t v, uint32_t n) { return memset(d, v, n); }
static int narrow(char *b, ULONG n, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = _vsnprintf(b, n, f, ap);
    va_end(ap);
    return r;
}
static int wide(WCHAR *b, ULONG n, const WCHAR *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = _vsnwprintf(b, n, f, ap);
    va_end(ap);
    return r;
}
static const WCHAR *W(const char *s) {
    static WCHAR pool[8][128];
    static unsigned next;
    WCHAR *w = pool[next++ % 8];
    unsigned i = 0;
    for (; s[i]; i++)
        w[i] = (uint8_t)s[i];
    w[i] = 0;
    return w;
}
static int wequal(const WCHAR *a, const char *b, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        if (a[i] != (uint8_t)b[i])
            return 0;
    return 1;
}
int main(void) {
    char b[64];
    WCHAR w[64];
    /* Exact fit: count returned, no terminator; one more: -1, partial copy. */
    memset(b, 'Z', sizeof(b));
    CHECK(narrow(b, 5, "%s", "abcde") == 5 && !memcmp(b, "abcde", 5) && b[5] == 'Z');
    memset(b, 'Z', sizeof(b));
    CHECK(narrow(b, 5, "%s", "abcdef") == -1 && !memcmp(b, "abcde", 5) && b[5] == 'Z');
    memset(b, 'Z', sizeof(b));
    CHECK(narrow(b, 6, "x%dy", 1234) == 6 && !memcmp(b, "x1234y", 6) && b[6] == 'Z');
    CHECK(narrow(b, 7, "x%dy", 1234) == 6 && !strcmp(b, "x1234y"));
    memset(b, 'Z', sizeof(b));
    CHECK(narrow(b, 3, "%d", 123456) == -1 && !memcmp(b, "123", 3) && b[3] == 'Z');
    memset(b, 'Z', sizeof(b));
    CHECK(narrow(b, 4, "ab%%cd") == -1 && !memcmp(b, "ab%c", 4) && b[4] == 'Z');
    CHECK(narrow(b, 0, "abc") == -1 && b[0] == 'a');
    /* NULL buffer counts and never overflows. */
    CHECK(narrow(NULL, 0, "%s-%d", "hello", -42) == 9);
    /* Flags, width, precision, sign and alternate forms. */
    CHECK(narrow(b, sizeof(b), "[%5d|%-5d|%05d|%+d|% d|%.3d]", 42, 42, 42, 42, 42, 7) == 31 &&
          !strcmp(b, "[   42|42   |00042|+42| 42|007]"));
    CHECK(narrow(b, sizeof(b), "[%x|%X|%#x|%#X|%#x|%o|%#o|%#o]", 255, 255, 255, 255, 0, 8, 8, 0) > 0 &&
          !strcmp(b, "[ff|FF|0xff|0XFF|0|10|010|0]"));
    CHECK(narrow(b, sizeof(b), "[%u|%d|%hd|%hu|%i]", 4294967295u, -2147483647 - 1, 70000, 70000, -1) > 0 &&
          !strcmp(b, "[4294967295|-2147483648|4464|4464|-1]"));
    CHECK(narrow(b, sizeof(b), "[%I64d|%lld|%I64x|%I32d|%Iu]", -1234567890123ll, 9007199254740993ll,
                 0x123456789abcdefull, 5, (void *)(uintptr_t)77) > 0 &&
          !strcmp(b, "[-1234567890123|9007199254740993|123456789abcdef|5|77]"));
    CHECK(narrow(b, sizeof(b), "[%*d|%-*d|%.*d|%.0d|%.0x]", 4, 1, 4, 1, 3, 5, 0, 0) > 0 &&
          !strcmp(b, "[   1|1   |005||]"));
    CHECK(narrow(b, sizeof(b), "[%*d]", -3, 9) > 0 && !strcmp(b, "[9  ]"));
    CHECK(narrow(b, sizeof(b), "[%08.3d|%-08d]", 5, 5) > 0 && !strcmp(b, "[     005|5       ]"));
    /* Strings: precision, padding, (null), narrow/wide and h/l/w forcing. */
    CHECK(narrow(b, sizeof(b), "[%.2s|%4s|%-4s|%05s]", "abc", "ab", "ab", "ab") > 0 &&
          !strcmp(b, "[ab|  ab|ab  |000ab]"));
    CHECK(narrow(b, sizeof(b), "[%s|%.3s]", (char *)NULL, (char *)NULL) > 0 && !strcmp(b, "[(null)|(nu]"));
    CHECK(narrow(b, sizeof(b), "[%S|%ls|%ws|%hs]", W("wide"), W("long"), W("w"), "short") > 0 &&
          !strcmp(b, "[wide|long|w|short]"));
    {
        const WCHAR euro[] = {0x20ac, 'x', 0x4e00, 0};
        CHECK(narrow(b, sizeof(b), "%S", euro) == 3 && (uint8_t)b[0] == 0x80 && b[1] == 'x' && b[2] == '?');
    }
    CHECK(narrow(b, sizeof(b), "[%c|%3c|%-3c|%C|%hc|%lc]", 'a', 'b', 'c', (int)0x20ac, 'd', (int)'e') > 0 &&
          !strcmp(b, "[a|  b|c  |\x80|d|e]"));
    CHECK(narrow(b, sizeof(b), "a%cb", 0) == 3 && b[0] == 'a' && b[1] == 0 && b[2] == 'b');
    /* %p: upper-case hex, two digits per pointer byte. */
    {
        char expect[64];
        int digits = (int)(2 * sizeof(void *));
        snprintf(expect, sizeof(expect), "[%0*llX|%0*llX  ]", digits, 0xbeefull, digits, 0x10ull);
        CHECK(narrow(b, sizeof(b), "[%p|%-*p]", (void *)(uintptr_t)0xbeef, digits + 2, (void *)(uintptr_t)0x10) > 0 &&
              !strcmp(b, expect));
    }
    /* %n stores the units written so far, by size. */
    {
        int n = -1;
        short h = -1;
        long long ll = -1;
        CHECK(narrow(b, sizeof(b), "abc%n%d%hn%lln", &n, 12, &h, &ll) == 5 && n == 3 && h == 5 && ll == 5 &&
              !strcmp(b, "abc12"));
    }
    /* No floating point: the conversion character is copied; L and F ignored. */
    CHECK(narrow(b, sizeof(b), "[%f|%e|%g|%Lf|%Fd]", 7) > 0 && !strcmp(b, "[f|e|g|f|7]"));
    /* Positional parameters are refused; a trailing lone % ends the format. */
    CHECK(narrow(b, sizeof(b), "a%1$d", 4) == -1);
    CHECK(narrow(b, sizeof(b), "ab%") == 2 && !strcmp(b, "ab"));
    /* Wide flavour: same engine, wide units; %s wide, %S/%hs narrow. */
    memset(w, 0x5a, sizeof(w));
    CHECK(wide(w, 4, W("%s%d"), W("ab"), 12) == 4 && wequal(w, "ab12", 4) && w[4] == 0x5a5a);
    CHECK(wide(w, 4, W("%s%d"), W("ab"), 123) == -1 && wequal(w, "ab12", 4) && w[4] == 0x5a5a);
    CHECK(wide(w, 64, W("[%S|%hs|%s|%ls|%c|%C|%5.2s]"), "nar", "row", W("wide"), W("l"), (int)0x20ac, (int)'\x80',
               W("xyz")) > 0 &&
          w[1] == 'n' && w[5] == 'r' && w[9] == 'w' && w[14] == 'l' && w[16] == 0x20ac && w[18] == 0x20ac &&
          wequal(w + 20, "   xy]", 6) && w[26] == 0);
    CHECK(wide(NULL, 0, W("%08x"), 1) == 8);
    CHECK(wide(w, 64, W("%I64u"), 18446744073709551615ull) == 20 && wequal(w, "18446744073709551615", 21));
    printf("PASS ntdll printf checks=%u\n", checks);
    puts("PASS ntdll printf Wine overflow exact-fit counting flags sizes strings pointers %n float refusal wide");
    return 0;
}
