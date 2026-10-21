/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_NETSURF_BUDGET_H
#define CIUKI_NETSURF_BUDGET_H
#include <stdbool.h>
#include <stdint.h>
extern uint64_t ciuki_layout_max_ns;
uint64_t ciuki_layout_begin(void);
bool ciuki_layout_expired(void);
bool ciuki_layout_end(uint64_t started, bool accepted);
#endif
