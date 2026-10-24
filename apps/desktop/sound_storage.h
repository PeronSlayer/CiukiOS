/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SOUND_STORAGE_H
#define CIUKI_SOUND_STORAGE_H
#include "sounds.h"
#include <ciuki/registry.h>
#include "../../sdk/sysroot-overlay/include/ciuki/audio_preference.h"
int master_preference_save(const char *directory, unsigned left, unsigned right, unsigned mute);
/* Paths are explicit for host fixtures; production uses SOUND_CFG,
 * SOUND_LEGACY_CFG and REG_DIRECTORY. Loads never create registry keys. */
int sound_load_at(struct sound_scheme *, const char *cfg, const char *legacy, const char *registry);
int sound_save_at(const struct sound_scheme *, const char *registry);
int sound_scheme_list_at(const char *directory, char names[][SOUND_SCHEME_NAME], unsigned capacity);
int sound_scheme_at(const char *directory, unsigned operation, const char *name, struct sound_scheme *);
int sound_validate_file(const char *path);
#endif
