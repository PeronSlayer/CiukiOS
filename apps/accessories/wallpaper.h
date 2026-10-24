/* SPDX-License-Identifier: MIT
 * "Set As Wallpaper" for Paint and the Image Viewer (G27): updates the
 * wallpaper and wallpaper_mode keys of the desktop settings file (the other
 * keys are kept) and asks the desktop to reload it, the same two steps the
 * Control Panel's Display page takes. */
#ifndef CIUKI_ACC_WALLPAPER_H
#define CIUKI_ACC_WALLPAPER_H
#include "../libdeskui/deskui.h"
#include "../desktop/settings.h"
/* Windows 98 placements: Centered, Tiled, Stretched. */
enum acc_wallpaper_mode { ACC_WALL_CENTER = CFG_WALL_CENTER, ACC_WALL_TILE = CFG_WALL_TILE, ACC_WALL_STRETCH = CFG_WALL_STRETCH };
/* Wallpapers the desktop can decode: BMP, PNG and JPEG (not GIF). */
int acc_wallpaper_supported(const char *path);
/* Rewrites the settings file at cfg_path (missing file: defaults). 0 or -errno. */
int acc_wallpaper_write(const char *cfg_path, const char *image_path, int mode);
/* CFG_FILE plus a DESK_SETTINGS_RELOAD request on the desktop channel. */
int acc_wallpaper_apply(struct du_window *, const char *image_path, int mode);
#endif
