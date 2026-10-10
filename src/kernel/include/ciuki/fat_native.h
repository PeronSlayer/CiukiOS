/* F2's required FAT integration boundary. A missing backend MUST fail before
 * namespace mutation; the original F1 driver cannot safely modify an orphan.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FAT_NATIVE_H
#define CIUKI_FAT_NATIVE_H
#include "../../fs/fat.h"
/* True only when regular index=UINT32_MAX entries skip directory updates
 * and fat_remove releases only their retained chain. */
extern bool file_read_cancelled(void) __attribute__((weak));
extern bool fat_native_ready(void) __attribute__((weak));
/* Reserve destination slots/alias before removing either name. Replacement
 * becomes index=UINT32_MAX after removal, retaining its chain when requested.
 * On success, file contains the new owning entry and preserved spelling. */
extern int fat_rename_replace(struct fat_volume *, struct fat_entry *file,
                              struct fat_entry *replacement, uint32_t parent,
                              const char *name, bool retain_replacement) __attribute__((weak));
#endif
