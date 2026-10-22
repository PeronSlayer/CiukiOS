/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_NETSURF_FONT_WORK_H
#define CIUKI_NETSURF_FONT_WORK_H
#include <stdbool.h>
#include <stdint.h>
struct ciuki_font_work {
    unsigned opens,measures,hits;
    uint64_t open_ns,measure_ns,bytes;
};
struct ciuki_font_work ciuki_font_work_read(void);
void ciuki_font_work_report(struct ciuki_font_work,uint64_t elapsed,bool local);
#endif
