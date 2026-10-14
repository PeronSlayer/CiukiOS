/* Atomic stereo/mute preference in System.reg. SPDX-License-Identifier: MIT */
#ifndef CIUKI_AUDIO_PREFERENCE_H
#define CIUKI_AUDIO_PREFERENCE_H
#include <stdint.h>
#define CIUKI_MASTER_KEY "System\\Audio"
#define CIUKI_MASTER_VALUE "Master"
#define CIUKI_MASTER_DEFAULT 20u
static inline int32_t ciuki_master_pack(unsigned l, unsigned r, unsigned mute)
{ return (int32_t)(l | (r << 8) | (mute << 16)); }
static inline int ciuki_master_unpack(int32_t v, unsigned *l, unsigned *r, unsigned *mute)
{
    if ((uint32_t)v & ~0x1ffffu) return 0;
    unsigned left = (unsigned)v & 255u, right = ((unsigned)v >> 8) & 255u;
    if (left > 100 || right > 100) return 0;
    *l = left; *r = right; *mute = ((unsigned)v >> 16) & 1u; return 1;
}
#endif
