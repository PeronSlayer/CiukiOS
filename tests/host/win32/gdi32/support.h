/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GDI_TEST_SUPPORT_H
#define GDI_TEST_SUPPORT_H
#include <stdint.h>
#include <w32/gdi32.h>
extern long test_allocations, test_bytes, test_fail_after;
void test_reset(void);
struct w32_dib test_dib(void *, unsigned, unsigned, unsigned, unsigned, int);
void test_save(const char *, const char *, const struct w32_dib *);
#endif
