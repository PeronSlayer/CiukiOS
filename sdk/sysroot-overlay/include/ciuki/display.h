/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_DISPLAY_H
#define CIUKI_DISPLAY_H
#include <ciuki/abi.h>
/* These two calls return 0 or a negative errno (they do not translate to -1).
 * Query requires version=CIUKI_DISPLAY_VERSION and size=sizeof(*info). */
int ciuki_display_query(struct ciuki_display_info *);
int ciuki_display_set(uint16_t mode_id);
#endif
