/* Trusted VMM policy only. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_LPT_H
#define CIUKI_DOSVM_LPT_H
struct dosvm;
struct v86_frame;
struct dosvm_lpt_state { uint32_t index, generation; bool raw; };
int dosvm_lpt_init(struct dosvm *);
void dosvm_lpt_stop(struct dosvm *);
int dosvm_lpt_bios(struct dosvm *, struct v86_frame *);
/* Explicit policy seam; never callable through guest INT17 or guest registers. */
int dosvm_lpt_lease(struct dosvm *, unsigned slot, unsigned physical, bool raw);
#endif
