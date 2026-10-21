/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_NETSURF_CHARSET_H
#define CIUKI_NETSURF_CHARSET_H
#include <stddef.h>
#include "utils/errors.h"
/* Exactly the four approved encodings; no iconv ABI or identity fallback. */
nserror ciuki_charset(const char *, size_t, const char *, const char *, char **, size_t *);
#endif
