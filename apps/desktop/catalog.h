/* SPDX-License-Identifier: MIT
 * Launcher catalogue of the accessories recovered from the 0.8 line:
 * /Programs name, launcher label, Tango Icon Theme 0.8.90 icon name (Public
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
    {"clock", "Clock", "appointment-new", 0, ""},   /* a clock face; Settings > Date & Time keeps office-calendar */
    {"help", "CiukiOS Help", "help-browser", 1, ""},
    {"mixer", "Volume Control", "audio-volume-high", 0, ""},
    {"viewer", "Image Viewer", "image-x-generic", 1, "png|jpg|jpeg|jpe|bmp|gif|xrgb"},
};
#define DESK_CATALOG_COUNT (sizeof(desk_catalog) / sizeof(desk_catalog[0]))
/* Pre-installed DOS games (owner decision 2026-10-12): the shareware
 * releases on the bundled games disk (C:\GAMES\CIUKIGAM.IMG, built by
 * scripts/build_games_disk.py), which every /Programs/dosbox window boots.
 * Programs > Games starts "/Programs/dosbox --name=<title> <command>"; dosbox
 * types the command at the DOS prompt (apps/dosbox/autorun.h). Commands
 * use only characters autorun.h can type; title is the window title.
 * Icon: Tango input-gaming (a game), distinct from the Games folder
 * (applications-games) and from the Ciuki Demo. */
struct desk_game_entry { const char *label, *title, *command, *tango; };
static const struct desk_game_entry desk_games[] = {
    {"Wolfenstein 3D (shareware)", "Wolfenstein 3D", "\\WOLF3D\\WOLF3D.EXE", "input-gaming"},
    {"DOOM (shareware)", "DOOM", "\\DOOM\\DOOM.EXE -config CIUKI.CFG", "input-gaming"},
};
#define DESK_GAME_COUNT (sizeof(desk_games) / sizeof(desk_games[0]))
static inline const struct desk_catalog_entry *desk_catalog_find(const char *name)
{
    for (unsigned i = 0; i < DESK_CATALOG_COUNT; i++) if (!strcmp(desk_catalog[i].name, name)) return &desk_catalog[i];
    return 0;
}
#endif
