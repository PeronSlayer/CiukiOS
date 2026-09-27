/* Optional GPL-2.0-or-later DBOPL synthesis adapter.
 *
 * This wrapper is linked only when the pinned VSBHDA/DOSBox DBOPL source is
 * selected.  Distributors of a combined binary must follow that source's GPL
 * terms.  The core guest_peripherals.c model does not depend on this header.
 */
#ifndef CIUKIOS_GUEST_OPL_DBOPL_H
#define CIUKIOS_GUEST_OPL_DBOPL_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct cvgp_dbopl cvgp_dbopl;

cvgp_dbopl *cvgp_dbopl_create(unsigned sample_rate);
void cvgp_dbopl_destroy(cvgp_dbopl *opl);
void cvgp_dbopl_reset(void *opaque, unsigned sample_rate);
void cvgp_dbopl_write(void *opaque, uint16_t reg, uint8_t value);
int cvgp_dbopl_generate(void *opaque, int16_t *stereo, unsigned frames);

#if defined(__cplusplus)
}
#endif
#endif
