#ifndef CIUKIOS_DPMI_VIDEO_IO_H
#define CIUKIOS_DPMI_VIDEO_IO_H

#include "virtual_vga.h"

/* Pinned HDPMI 3.24 IOPL=0 adapter, one owning 32-bit flat client. This is not
 * the incompatible old HDPMI fork's register callback ABI. The session loader
 * must select the qualified packaged host before entering this client.
 * It owns no guest memory mappings, IRQs, scheduling or DOS contexts.
 *
 * No BIOS/DOS calls, port passthrough or monitor transitions occur in a trap.
 * String I/O is explicitly unsupported in this first adapter: its instruction
 * is consumed without memory/port access, and stats.fatal becomes nonzero.
 * The session owner MUST terminate/reject that session on a fatal diagnostic;
 * continuing to treat it as a functioning device would be incorrect.
 * init/install/remove are foreground calls. The model and this client must
 * remain resident until remove succeeds. Packaged HDPMI has no swapping.
 */
enum {
    CVIO_OK = 0, CVIO_NO_STATE = 1, CVIO_NO_HDPMI = 2,
    CVIO_RANGE_BUSY = 3, CVIO_REMOVE_FAILED = 4, CVIO_STRING_UNSUPPORTED = 5,
    CVIO_BAD_WIDTH = 6, CVIO_REENTRANT = 7, CVIO_BAD_LENGTH = 8,
    CVIO_NOT_FLAT = 9, CVIO_ALREADY_INSTALLED = 10, CVIO_RANGE_CROSSING = 11
};
typedef struct cvio_stats {
    uint32_t handle, traps, reads, writes, unsupported, fatal, reentries;
    uint32_t last_flags, last_port, last_eip;
} cvio_stats;

extern cvio_stats cvio_stats_data;
extern cvga_state *cvio_state;
int CVGA_CALL cvio_init(cvga_state *state);
int CVGA_CALL cvio_install(void);
int CVGA_CALL cvio_remove(void);
void CVGA_CALL cvio_set_status1(unsigned status1);

#endif
