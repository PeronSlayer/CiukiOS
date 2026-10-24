/* Shared across Duktape heaps, admitted before allocation. SPDX-License-Identifier: MIT */
#ifndef CIUKI_JS_MEMORY_H
#define CIUKI_JS_MEMORY_H
#include <stddef.h>
void *ciuki_js_alloc(size_t);
void *ciuki_js_realloc(void *,size_t);
void ciuki_js_free(void *);
size_t ciuki_js_charged(void);
#endif
