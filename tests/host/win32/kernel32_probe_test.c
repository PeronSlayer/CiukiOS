/* Result protocol dry-run only; guest execution is a separate qualification.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "../../../src/probes/win32_kernel32/controller.h"
static unsigned records;
static void emit(const char *probe, const char *event, const char *format, ...)
{
    va_list args;
    assert(probe && event && format);
    printf("%s %s ", probe, event);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    records++;
}
int main(void)
{
    struct w32_kernel32_result r = { .magic = W32_KERNEL32_MAGIC, .version = 1, .done = 1 };
    for (unsigned i=0; i<W32_K32_CASES; i++) r.checked[i] = 1;
    assert(w32_kernel32_valid(&r));
    assert(w32_kernel32_pass(&r, 0));
    assert(!w32_kernel32_pass(&r, 1));
    w32_kernel32_publish(&r, emit);
    assert(records == W32_K32_CASES);
    for (unsigned i=0; i<W32_K32_CASES; i++) {
        r.errors[i] = 1;
        assert(!w32_kernel32_pass(&r, 0));
        r.errors[i] = 0;
        r.checked[i] = 0;
        assert(!w32_kernel32_valid(&r));
        r.checked[i] = 1;
    }
    r.done = 2; assert(!w32_kernel32_valid(&r)); r.done = 1;
    r.version = 2; assert(!w32_kernel32_valid(&r)); r.version = 1;
    r.magic = 0; assert(!w32_kernel32_valid(&r));
    return 0;
}
