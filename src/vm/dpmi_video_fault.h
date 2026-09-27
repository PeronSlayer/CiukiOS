/* CiukiOS protected-mode video-memory trap for one DPMI 0.9 client.
 *
 * Under an armed CVSESSION, a fresh HDPMI copies the supervisor-only VGA
 * PTEs through VCPI, so a ring-3 access to A0000-BFFFF raises #PF. This
 * client-installed exception 0Eh handler executes exactly that instruction
 * with cvx_execute. Aperture byte cycles are sent, in bus order, to the
 * shared model in CVSESSION through DPMI 0301h (real-mode far call to the
 * JLM entry, VM_OP_VIDEO_ACCESS). Other memory is accessed through the flat
 * data selector. Faults that do not touch the aperture are chained unchanged.
 *
 * Scope: the installing client only. Other DPMI clients (for example an
 * original extender program started later) keep their own exception
 * handlers; covering them needs a host-level hook (see
 * docs/vm-video-coordination-2026-09-27.md). No DOS/BIOS call is made from
 * the handler; the only host service used there is DPMI 0006h/0301h.
 */
#ifndef CIUKIOS_DPMI_VIDEO_FAULT_H
#define CIUKIOS_DPMI_VIDEO_FAULT_H

#include <stdint.h>

enum { CVPM_OK = 0, CVPM_DOS_MEMORY = 1, CVPM_HANDLER = 2, CVPM_INSTALLED = 3, CVPM_NOT_FLAT = 4 };

typedef struct cvpm_frame {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;
    uint32_t ret_eip, ret_cs, error, eip, cs, eflags, esp, ss;
} cvpm_frame;

typedef struct cvpm_stats {
    uint32_t faults, emulated, chained, bridge_calls, aperture_bytes, bridge_failures;
    uint32_t unsupported, reentered;
} cvpm_stats;

extern cvpm_stats cvpm_stats_data;
extern cvpm_frame cvpm_frame_data;
/* entry = real-mode far address of the CVSESSION entry (INT 2Fh/1684h). */
int cvpm_install(uint16_t entry_segment, uint16_t entry_offset);
int cvpm_remove(void);
/* Called by the thunk with cvpm_frame_data filled. 0 = handled. */
int cvpm_fault(void);

#endif
