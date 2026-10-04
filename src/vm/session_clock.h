/* Per-VM virtual PIT clock for CVSESSION. */
#ifndef CIUKIOS_SESSION_CLOCK_H
#define CIUKIOS_SESSION_CLOCK_H

#include <stdint.h>

#if defined(__WATCOMC__)
#define CVCLK_CALL __cdecl
#else
#define CVCLK_CALL
#endif

#if defined(__cplusplus)
extern "C" {
#endif

/* VM indexes are 0..3. tsc_khz is calibrated TSC cycles per millisecond. */
void CVCLK_CALL cvclock_begin(uint32_t vm, uint32_t tsc_khz);
void CVCLK_CALL cvclock_end(uint32_t vm);
/* Advance before access; writes return the supplied byte, reads its byte. */
uint32_t CVCLK_CALL cvclock_port(uint32_t vm, uint32_t port,
                                uint32_t write, uint32_t value);
/* Advance this VM's PIT to now and return queued channel-0 expirations. */
uint32_t CVCLK_CALL cvclock_pending(uint32_t vm);
/* Advance to now, then consume one queued expiration (1 consumed, 0 empty). */
uint32_t CVCLK_CALL cvclock_consume(uint32_t vm);
/* Packed root-physical restore: low word reload (65536 encoded as 0),
 * next byte normalized channel-0 lo/hi access plus current PIT mode. */
uint32_t CVCLK_CALL cvclock_restore(uint32_t vm);

#if defined(__cplusplus)
}
#endif
#endif
