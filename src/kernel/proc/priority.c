/* API encodings are distinct from internal scheduler levels. CiukiOS R2
 * policy; no public priority setter/elevation is introduced here.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/process.h>

int proc_priority_posix(int policy, int priority, int nice_value)
{
    if (policy || priority) return -EPERM; /* no unprivileged realtime */
    if (nice_value < -20 || nice_value > 19) return -EINVAL;
    /* Nice -20..19 -> 13..1 in bounded three-value bands; nice 0 -> 7. */
    static const uint8_t levels[40] = {
        13,13,13,13,12,12,12,11,11,11,10,10,10,9,9,9,8,8,8,7,
        7,7,7,6,6,6,5,5,5,4,4,4,3,3,3,2,2,2,1,1
    };
    return levels[nice_value + 20];
}

int proc_priority_win32(uint32_t priority_class, int relative, bool foreground)
{
    unsigned base;
    switch (priority_class) {
    case 0x40: base = 4; break; /* IDLE_PRIORITY_CLASS (not the idle task) */
    case 0x20: base = foreground ? SCHED_FOREGROUND : CIUKI_SCHED_NORMAL; break;
    case 0x80: base = SCHED_HIGH; break;
    case 0x100: return -EPERM; /* REALTIME_PRIORITY_CLASS reserved */
    default: return -EINVAL;
    }
    if (relative == -15) return 1; /* THREAD_PRIORITY_IDLE */
    if (relative == 15) return SCHED_BOOST_CAP; /* TIME_CRITICAL */
    if (relative < -2 || relative > 2) return -EINVAL;
    int level = (int)base + relative;
    return level < 1 ? 1 : level > (int)SCHED_BOOST_CAP ? SCHED_BOOST_CAP : level;
}
