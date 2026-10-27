/* SPDX-License-Identifier: MIT
 * Compile the supplied bundle only: never execute its untrusted contents. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "duktape.h"
#include "sdk/netsurf/port/js_memory.h"

static size_t peak;
duk_bool_t dukky_check_timeout(void *data) { (void)data; return 0; }
static void record_peak(void) { if (ciuki_js_charged() > peak) peak = ciuki_js_charged(); }
static void *allocate(void *data, duk_size_t bytes)
{
    (void)data;
    void *p = ciuki_js_alloc(bytes);
    record_peak();
    return p;
}
static void *resize(void *data, void *p, duk_size_t bytes)
{
    (void)data;
    p = ciuki_js_realloc(p, bytes);
    record_peak();
    return p;
}
static void release(void *data, void *p) { (void)data; ciuki_js_free(p); }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0 && size < 1024 * 1024 && !fseek(file, 0, SEEK_SET));
    char *source = malloc((size_t)size);
    assert(source && fread(source, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    duk_context *ctx = duk_create_heap(allocate, resize, release, NULL, NULL);
    assert(ctx != NULL);
    clock_t start = clock();
    int status = duk_pcompile_lstring(ctx, DUK_COMPILE_EVAL, source, (size_t)size);
    double microseconds = (double)(clock() - start) * 1000000.0 / CLOCKS_PER_SEC;
    assert(status != 0);
    const char *error = duk_safe_to_string(ctx, -1);
    assert(strstr(error, "SyntaxError"));
    printf("PASS supplied bundle compile-only: bytes=%ld peak_charged=%zu compile_us=%.0f %s\n",
           size, peak, microseconds, error);
    duk_destroy_heap(ctx);
    free(source);
    assert(ciuki_js_charged() == 0);
    return 0;
}
