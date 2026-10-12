/* SPDX-License-Identifier: MIT
 * HOST ONLY, until f1-23 lands. The old call-67 record already uses the tag
 * ciuki_display_info. Preserve its layout and assertions under a local name
 * so tests can compile the EXACT new query record from the f1-23 directive.
 * f1-23 must resolve this tag collision in the real ABI/SDK; no kernel or
 * shipping ABI is changed by this host preinclude. Remove after that merge. */
#define ciuki_display_info ciuki_legacy_display_info
#include <ciuki/abi.h>
#undef ciuki_display_info
