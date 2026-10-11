/* f4-05 host build of audio/vm_source.c, audio/opl3.c and audio/mixer.c as
 * their own translation units beside the DOS VM harness: the harness is
 * single-threaded, so the IRQ sections are no-ops here (the harness keeps
 * its own fake IF for the VM code). SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VM_AUDIO_SHIM_H
#define CIUKI_VM_AUDIO_SHIM_H
#define CIUKI_CPU_H
#include <stdint.h>
static inline uint32_t irq_save(void) { return 0; }
static inline void irq_restore(uint32_t flags) { (void)flags; }
#endif
