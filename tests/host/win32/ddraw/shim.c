/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/shims.h>
static struct ciuki_w32_compat compat;
const struct ciuki_w32_compat *w32_shim_current(void) { return &compat; }
void host_shim_mask(uint32_t mask) { compat.mask = mask; }
