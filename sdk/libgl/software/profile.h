/* Optional host-only work counters, never enabled in the target recipe.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_SW_PROFILE_H
#define CIUKI_SW_PROFILE_H
#include <stdint.h>
enum sw_work {
  SW_TRIANGLE,
  SW_SPAN,
  SW_ATTRIBUTE_SETUP,
  SW_ATTRIBUTE_STEP,
  SW_COVERED,
  SW_EARLY_REJECT,
  SW_SHADED,
  SW_PERSPECTIVE_DIV,
  SW_LOD,
  SW_WIDE_DIV,
  SW_TEXEL,
  SW_STAGE,
  SW_FRAGMENT,
  SW_WRITE,
  SW_VERTEX,
  SW_WORK_COUNT
};
#ifdef SW_PROFILE
void sw_profile_count(enum sw_work, uint64_t);
#define SW_COUNT(key, value) sw_profile_count(key, value)
#else
#define SW_COUNT(key, value) ((void)0)
#endif
#endif
