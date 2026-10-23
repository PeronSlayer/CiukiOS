/* fx-9: the DOS VM clock (virtual PIT, SB16 DSP/DMA timeline, OPL timers,
 * COM wake, present checkpoint) on its own real-time task.
 *
 * From f0-46 to fx-9 the PIT tick queued the VM clock on the shared
 * `device-work` ring, one callback per DOSVM_AUDIO_TURN (0.5 ms of DSP time)
 * followed by a yield. `device-work` is an ordinary device-class consumer of
 * the shared 4 ms per 10 ms device budget (f1-90, R2); when other device
 * tasks exhausted that budget it was demoted to level 7, below the focused
 * VM (9), and the VM's own clock stopped while the VM kept running. T23
 * trial 423b803a (DOOM full screen, volume keys pressed): device-work
 * demoted 1264-1338 times per 2 s window, vm_perf_service ticks=3518 of
 * 5000, SB16 IRQ latency and acknowledgement age up to 1.17 s (the 1 s
 * DOSVM_AUDIO_BACKLOG plus delivery), source frames > 20 ms late 161372,
 * the host source starved 241664 frames in 5 s.
 *
 * Windows 9x reference (Win95 DDK, Adjust_Exec_Priority): VPICD simulates
 * hardware interrupts into a VM with Time_Critical_Boost, "for operations
 * that require immediate processing, even when another virtual machine is
 * in a critical section"; no other device's work can lower it. Here: the
 * PIT top half (task.c sched_tick -> dosvm_tick) wakes this task in O(1)
 * (kwait_wake_irq's audio-class path, as the AC'97 interrupt wakes the
 * refill, f1-90 R1) and annotates an immediate service deadline, so the
 * dispatch is urgent (task.c device_urgent) at whatever level the task is.
 * The task is audio class: its CPU is charged to its own 2 ms per 10 ms
 * budget, never to the shared one, and other tasks' consumption never
 * demotes it. One service runs every due VM to its tick allowance (at most
 * DOSVM_AUDIO_SLICE of DSP time per tick) in bounded turns separated by
 * preemption points (R1: one turn between safe points).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include "dosvm_clock.h"

void dosvm_clock_service(void)
{
    for (unsigned slot = 0; slot < DOSVM_CLOCK_SLOTS; slot++) {
        uint32_t generation = 0;
        for (unsigned turn = 1; dosvm_clock_turn(slot, &generation); turn++) {
            /* The tick allowance is exhausted after DOSVM_CLOCK_TURNS; the
             * next tick grants the remaining backlog (dosvm_audio_advance). */
            if (turn >= DOSVM_CLOCK_TURNS) break;
            kpreempt_point();
        }
    }
}

#ifndef DOSVM_CLOCK_HOST_TRANSPORT
static struct kwait clock_wait;
static struct task *clock_task;
static uint32_t clock_signals, clock_served;

static bool clock_due(void *arg)
{
    (void)arg;
    return __atomic_load_n(&clock_signals, __ATOMIC_ACQUIRE) != clock_served;
}
static void clock_main(void *arg)
{
    (void)arg;
    for (;;) {
        /* Only the PIT top half wakes this queue; no timer is armed, so the
         * wait never rides the shared worker's timer queue. */
        kwait_wait_until(&clock_wait, clock_due, 0, UINT64_MAX);
        clock_served = __atomic_load_n(&clock_signals, __ATOMIC_ACQUIRE);
        dosvm_clock_service();
    }
}
int dosvm_clock_start(void)
{
    if (__atomic_load_n(&clock_task, __ATOMIC_ACQUIRE)) return 0;
    kwait_init(&clock_wait);
    struct task *t = task_create_kernel("vm-clock", clock_main, 0, P_DEVICE);
    if (!t) return -ENOMEM;
    task_set_audio_class(t);
    __atomic_store_n(&clock_task, t, __ATOMIC_RELEASE);
    task_start(t);
    return 0;
}
/* PIT top half, IF clear: O(1). */
void dosvm_clock_signal(void)
{
    struct task *t = __atomic_load_n(&clock_task, __ATOMIC_ACQUIRE);
    if (!t) return;
    __atomic_fetch_add(&clock_signals, 1u, __ATOMIC_RELEASE);
    task_device_deadline(t, g_ticks);
    kwait_wake_irq(&clock_wait, false);
}
#endif
