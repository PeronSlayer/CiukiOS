/* fx-9: the DOS VM clock task interface (vm/dosvm_clock.c), kept free of the
 * VM implementation headers so the scheduler replays link the production
 * transport. dosvm.c checks the constants against the VM limits.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_CLOCK_H
#define CIUKI_DOSVM_CLOCK_H
#include <stdbool.h>
#include <stdint.h>
/* VM slots served (DOSVM_LIMIT) and the turns of one service: each tick
 * grants a VM at most DOSVM_AUDIO_SLICE (64 ms) of DSP time in
 * DOSVM_AUDIO_TURN (0.5 ms) steps, plus the turn that publishes the tick. */
#define DOSVM_CLOCK_SLOTS 4u
#define DOSVM_CLOCK_TURNS 130u
/* One bounded turn of the VM in slot; *generation (0 = the slot's started
 * VM) pins it across the caller's preemption points. True while the DSP
 * timeline has work left inside this tick's allowance. Task context. */
bool dosvm_clock_turn(unsigned slot, uint32_t *generation);
void dosvm_clock_service(void);   /* the vm-clock task's turns, all VMs */
int dosvm_clock_start(void);      /* the vm-clock task, once, thread context */
void dosvm_clock_signal(void);    /* PIT top half: O(1) wake and deadline */
#endif
