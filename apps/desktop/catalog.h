/* SPDX-License-Identifier: MIT
 * Launcher catalogue of the accessories recovered from the 0.8 line:
 * /bin name, launcher label, Tango Icon Theme 0.8.90 icon name (Public
 * Domain, assets/icons; resolved by du_icon(name, size) from the icons
 * branch) and whether the program opens a file given as its argument
 * (LAUNCH from Files). Every entry is a graphical desktop client. */
#ifndef CIUKI_DESKTOP_CATALOG_H
#define CIUKI_DESKTOP_CATALOG_H
#include <string.h>
struct desk_catalog_entry { const char *name, *label, *tango; int takes_file; const char *extensions; };
static const struct desk_catalog_entry desk_catalog[] = {
    {"calc", "Calculator", "accessories-calculator", 0, ""},
    {"charmap", "Character Map", "accessories-character-map", 0, ""},
    {"clock", "Clock", "office-calendar", 0, ""},
    {"help", "CiukiOS Help", "help-browser", 1, ""},
    {"mixer", "Volume Control", "audio-volume-high", 0, ""},
    {"viewer", "Image Viewer", "image-x-generic", 1, "png|jpg|jpeg|jpe|bmp|gif|xrgb"},
};
#define DESK_CATALOG_COUNT (sizeof(desk_catalog) / sizeof(desk_catalog[0]))
static inline const struct desk_catalog_entry *desk_catalog_find(const char *name)
{
    for (unsigned i = 0; i < DESK_CATALOG_COUNT; i++) if (!strcmp(desk_catalog[i].name, name)) return &desk_catalog[i];
    return 0;
}
#endif
