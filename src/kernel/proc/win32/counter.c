/* PIT-backed counter and i386 shared-page sequence publication.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/clock.h>
#include <ciuki/win32.h>
#include <ciuki/w32_objects.h>
/* t23-render (trial image c42f556e, 2026-10-25): every Win32 process maps
 * one and the same section page at CIUKI_W32_SHARED (sections.c
 * w32_shared_map), as Windows NT maps KUSER_SHARED_DATA into every process
 * and updates it from the clock interrupt. The tick used to publish that
 * page again for each live Win32 process through ua_range, ua_read and three
 * ua_write calls: five linear walks over all of the process's extents below
 * 0x7FFE0000, every millisecond, on the device-work thread. With the Quake
 * III installer running, one tick took 1.0-1.08 ms ([work] budget exceeded
 * site=shared_tick_work) and device-work held 274-342 permille of the CPU
 * with 1169-1198 budget overruns per 2 s window. The page is now written
 * once per tick through the kernel direct map: no extent walk, no
 * per-process work. */
void w32_shared_publish(void)
{
    struct ciuki_timespec now;
    uint64_t system = 0;
    if (!file_clock_now(CLOCK_REALTIME, 0, &now) && now.tv_sec >= -11644473600LL)
    {
        uint64_t seconds = (uint64_t)now.tv_sec + 11644473600ULL;
        uint32_t fraction = (uint32_t)now.tv_nsec / 100;
        system = seconds > (UINT64_MAX - fraction) / 10000000 ? UINT64_MAX :
            seconds * 10000000 + fraction;
    }
    /* sections.c clears the frame with interrupts enabled before it frees
     * the page; with interrupts off here the page cannot go away mid-write. */
    uint32_t f = irq_save();
    uint32_t frame = w32_shared_frame;
    if (frame) {
        volatile struct ciuki_w32_shared *s = P2V(frame);
        uint64_t ticks = g_ticks, interrupt = ticks * 10000;
        s->sequence = (s->sequence + 1) | 1;
        __asm__ volatile("" ::: "memory");
        s->tick = (uint32_t)ticks;
        s->interrupt_lo = (uint32_t)interrupt; s->interrupt_hi = (uint32_t)(interrupt >> 32);
        s->system_lo = (uint32_t)system; s->system_hi = (uint32_t)(system >> 32);
        __asm__ volatile("" ::: "memory");
        s->sequence = s->sequence + 1;
    }
    irq_restore(f);
}
