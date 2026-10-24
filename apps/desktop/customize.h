/* SPDX-License-Identifier: MIT
 * Internal folder-backed shell snapshots. Not a public SDK interface. */
#ifndef CIUKI_CUSTOMIZE_H
#define CIUKI_CUSTOMIZE_H
#include "settings.h"
#define SH_START_FOLDER "/Windows/Start Menu"
#define SH_PROGRAMS_FOLDER SH_START_FOLDER "/Programs"
#define SH_QUICK_FOLDER "/Windows/Application Data/Microsoft/Internet Explorer/Quick Launch"
#define SH_FAVORITES_FOLDER "/Windows/Favorites"
#define SH_LINKS_FOLDER "/Windows/Favorites/Links"
#define SH_DESKTOP_FOLDER "/Documents/Desktop"
#define SH_RECENT_FILE "/Documents/recent.lst"
#define SH_FOLDERS 32
#define SH_ENTRIES 256
#define SH_FOLDER_ITEMS 24
/* root slots stay stable across refreshes, children are assigned after them. */
enum { SH_ROOT_START, SH_ROOT_PROGRAMS, SH_ROOT_QUICK, SH_ROOT_LINKS, SH_ROOT_DESKTOP, SH_ROOT_CUSTOM, SH_ROOT_FAVORITES=SH_ROOT_CUSTOM+CFG_TOOLBARS, SH_ROOTS };
/* icon: the du_icon_name() object of the entry ("program:calc", "menu:games", "type:.txt"), resolved by
 * sh_folder_scan from the shortcut target; empty means the generic folder/program icon. */
struct sh_folder_item { char label[64], path[CFG_PATH_MAX], icon[40]; int folder, parent; };
struct sh_folder_snapshot {
    char folders[SH_FOLDERS][CFG_PATH_MAX];
    struct sh_folder_item entries[SH_ENTRIES];
    unsigned count, folders_count;
    int truncated,native_seeded;
};
enum { SH_EDIT_NONE, SH_EDIT_REFRESH, SH_EDIT_ADD, SH_EDIT_DELETE, SH_EDIT_RENAME, SH_EDIT_ORDER, SH_EDIT_CLEAR, SH_EDIT_TOOLBAR };
struct sh_edit_request {
    int operation;
    char path[CFG_PATH_MAX], destination[CFG_PATH_MAX], name[64];
    unsigned count;
    char order[SH_FOLDER_ITEMS][CFG_PATH_MAX];
    unsigned sources_count;
    char sources[SH_FOLDER_ITEMS][CFG_PATH_MAX];
};
#endif
