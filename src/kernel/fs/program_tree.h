/* Bounded game-directory operations over the production FAT driver.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_PROGRAM_TREE_H
#define CIUKI_PROGRAM_TREE_H
#include "fat.h"
/* game is the catalogue's 1-based id. size includes allocated directory and
 * file clusters. remove requires an already qualified writable mount. */
int program_tree(struct fat_volume *, unsigned game, bool remove, uint32_t *size);
#endif
