/* SPDX-License-Identifier: MIT
 * Private bridge to the patched libcss; not part of the Ciuki SDK ABI. */
#ifndef CIUKI_NETSURF_CSS_GAP_H
#define CIUKI_NETSURF_CSS_GAP_H
#include <libcss/computed.h>
void css__ciuki_computed_row_gap(const css_computed_style *style,
        css_fixed *length, css_unit *unit);
#endif
