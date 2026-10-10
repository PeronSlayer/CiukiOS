/* SPDX-License-Identifier: GPL-2.0-only */
#include "fs_port.h"
static fs_calendar_clock calendar;
void fs_set_calendar_clock(fs_calendar_clock clock) { calendar=clock; }
void fs_timestamp(uint16_t *date, uint16_t *time, uint8_t *tenths) {
    if (calendar && calendar(date,time,tenths)) return;
    *date=0x21; *time=0; *tenths=0;
}
