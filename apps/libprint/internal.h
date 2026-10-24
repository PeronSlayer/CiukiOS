/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_PRINT_INTERNAL_H
#define CIUKI_PRINT_INTERNAL_H
#include "print.h"
#include <stdio.h>
FILE *cp_tempfile(const char *, char *, size_t);
void cp_tempclose(FILE *, const char *);
#endif
