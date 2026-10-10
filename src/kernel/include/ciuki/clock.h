/* F2 clock service, integer milliseconds only.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_CLOCK_H
#define CIUKI_CLOCK_H
#ifndef CIUKI_BUILD_EPOCH
#define CIUKI_BUILD_EPOCH 0 /* host tests; the kernel build passes the recorded UTC epoch */
#endif
#include <ciuki/process.h>
struct clock_seed {
    int64_t utc;
    uint64_t tick;
    uint32_t source;
    bool qualified, valid;
};
bool clock_fat_utc(uint16_t date, uint16_t time, int64_t *seconds);
void file_clock_init(int64_t build_epoch, bool qualified, bool valid,
                     uint16_t date, uint16_t time, uint8_t tenths, uint64_t tick);
void file_clock_start(int64_t build_epoch, bool rtc_qualified);
void file_clock_snapshot(struct clock_seed *out);
void file_clock_tick(void); /* PIT, once per scheduled 1 ms quantum */
int file_clock_now(uint32_t clock, const struct process *, struct ciuki_timespec *out);
int file_clock_res(uint32_t clock, struct ciuki_timespec *out);
int file_sleep_deadline(const struct ciuki_timespec *, uint64_t now, uint64_t *deadline);
int file_nanosleep(uint32_t request, uint32_t remaining);
#endif
