/* Shared across Duktape heaps, admitted before allocation. SPDX-License-Identifier: MIT */
#ifndef CIUKI_JS_MEMORY_H
#define CIUKI_JS_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
void *ciuki_js_alloc(size_t);
void *ciuki_js_realloc(void *,size_t);
void ciuki_js_free(void *);
size_t ciuki_js_charged(void);
/* Budget left (the budget is re-read from free memory while nothing is
 * charged) and the number of refused allocations, for heap setup checks. */
size_t ciuki_js_available(void);
unsigned long ciuki_js_refusals(void);
/* Set by the exit report just before netsurf_exit(): on the target the
 * Duktape heaps are then left to the process teardown (patch 0012). */
extern bool ciuki_js_exiting;
#endif
