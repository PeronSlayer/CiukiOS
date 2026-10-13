/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SOUND_STORAGE_H
#define CIUKI_SOUND_STORAGE_H
#include "sounds.h"
#include <ciuki/registry.h>
/* Paths are explicit for host fixtures; production uses SOUND_CFG,
 * SOUND_LEGACY_CFG and REG_DIRECTORY. Loads never create registry keys. */
int sound_load_at(struct sound_scheme *, const char *cfg, const char *legacy, const char *registry);
int sound_save_at(const struct sound_scheme *, const char *registry);
#endif
