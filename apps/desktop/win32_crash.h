/* Text-only crash persistence. SPDX-License-Identifier: MIT */
#ifndef DESK_W32_CRASH_H
#define DESK_W32_CRASH_H
#include <ciuki/w32_diagnostics.h>
int desk_w32_crash_store(const char *, const struct ciuki_w32_fault *);
#endif
