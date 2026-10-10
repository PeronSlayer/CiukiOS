/* Private interim payload/controller record, not a userspace API.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FILES_PROBE_H
#define CIUKI_FILES_PROBE_H
#include <ciuki/abi.h>
#define FILES_RESULT (CIUKI_IMAGE_BASE + 2 * CIUKI_PAGE_SIZE)
struct files_result {
    uint32_t done, release, checks, errors, stage, tid, reads, decreases;
    struct ciuki_timespec monotonic, realtime, busy_before, busy_after;
    struct ciuki_timespec sleep_cpu_before, sleep_cpu_after, sleep_begin, sleep_end, remaining;
    int32_t sleep_result, interrupted_result;
    uint32_t signal_entries, io_cases;
};
_Static_assert(sizeof(struct files_result) == 192, "interim files result");
_Static_assert(offsetof(struct files_result, remaining) == 160, "interim files remainder");
_Static_assert(offsetof(struct files_result, interrupted_result) == 180, "interim files EINTR");
int probe_f2_fd_table(void);
#endif
