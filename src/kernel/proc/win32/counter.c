/* PIT-backed counter and i386 shared-page sequence publication.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/clock.h>
#include <ciuki/win32.h>
void w32_shared_update(struct process *p)
{
    if (!p->memory || !ua_range(p->memory, CIUKI_W32_SHARED, sizeof(struct ciuki_w32_shared), PROT_READ)) return;
    struct ciuki_w32_shared data;
    if (ua_read(p->memory, &data, CIUKI_W32_SHARED, sizeof(data))) return;
    data.sequence = (data.sequence + 1) | 1;
    ua_write(p->memory, CIUKI_W32_SHARED, &data.sequence, 4);
    data.tick = (uint32_t)g_ticks;
    uint64_t interrupt = g_ticks * 10000;
    struct ciuki_timespec now;
    uint64_t system = 0;
    if (!file_clock_now(CLOCK_REALTIME, p, &now) && now.tv_sec >= -11644473600LL)
    {
        uint64_t seconds = (uint64_t)now.tv_sec + 11644473600ULL;
        uint32_t fraction = (uint32_t)now.tv_nsec / 100;
        system = seconds > (UINT64_MAX - fraction) / 10000000 ? UINT64_MAX :
            seconds * 10000000 + fraction;
    }
    data.interrupt_lo = (uint32_t)interrupt; data.interrupt_hi = (uint32_t)(interrupt >> 32);
    data.system_lo = (uint32_t)system; data.system_hi = (uint32_t)(system >> 32);
    ua_write(p->memory, CIUKI_W32_SHARED + 4, (uint8_t *)&data + 4, sizeof(data) - 4);
    data.sequence++;
    __asm__ volatile("" ::: "memory");
    ua_write(p->memory, CIUKI_W32_SHARED, &data.sequence, 4);
}
