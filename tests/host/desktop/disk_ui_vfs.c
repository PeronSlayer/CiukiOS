/* SPDX-License-Identifier: MIT
 * The compositor and Files fixtures both historically export fake_reset. */
#define fake_reset disk_ui_vfs_reset
#include "fake_vfs.c"
