/* SPDX-License-Identifier: MIT
 * f1-23 SDK mode-set boundary. Kept separate from the old call-67 grant
 * record consumer until f1-23 resolves the ciuki_display_info tag collision. */
#include <ciuki/display.h>
struct desktop;
int desk_display_mode_set(struct desktop *d, uint16_t mode)
{
    (void)d;
    return ciuki_display_set(mode); /* 0 or -errno, exactly as f1-23 specifies */
}
