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
    {"gfxdemo", "Graphics Demo", "applications-graphics", 0, ""},
    {"hwreport", "Hardware Report", "preferences-system", 0, ""},
    {"printers", "Printers", "printer", 0, ""},
    {"regedit", "Registry Editor", "preferences-system", 0, ""},
    {"logview", "Log Viewer", "text-x-generic", 0, ""},   /* G13: boot, NetSurf, Setup and crash logs */
    {"clipbrd", "Clipboard Viewer", "edit-paste", 1, "clp"},
    {"calc", "Calculator", "accessories-calculator", 0, ""},
    {"charmap", "Character Map", "accessories-character-map", 0, ""},
    {"clock", "Clock", "appointment-new", 0, ""},   /* a clock face; Settings > Date & Time keeps office-calendar */
    {"help", "CiukiOS Help", "help-browser", 1, ""},
    {"mixer", "Volume Control", "audio-volume-high", 0, ""},
    {"recorder", "Sound Recorder", "audio-input-microphone", 0, ""},
    {"music", "Music Player", "audio-x-generic", 1, "wav|mp3|ogg|m3u"},
    {"viewer", "Image Viewer", "image-x-generic", 1, "bmp|png|jpg|jpeg|jpe|gif|xrgb"},
    /* Viewer is the default; keep Paint available in the shell's Open With. */
    {"paint", "CiukPaint", "applications-graphics", 1, "bmp|png"},
    /* G28: WordPad opens Rich Text Format (text files stay with CiukNote). */
    {"wordpad", "WordPad", "x-office-document", 1, "rtf"},
};
#define DESK_CATALOG_COUNT (sizeof(desk_catalog) / sizeof(desk_catalog[0]))
/* Native applications with their own desktop window (f6-19). The program
 * record, not a test gate, grants the desktop window channel: every launch
 * route (Programs menu, desktop icon, Run, Terminal, ShellExecute) starts
 * the program with --channel-fd=3. Each record carries the launcher label,
 * the Tango 0.8.90 icon name (16/22/32 px in the store, 48 px derived) and
 * the description. NetSurf is the browser application (owner 2026-10-21):
 * Programs root, never the Developer folder. */
struct desk_application_entry { const char *name, *label, *tango, *description; };
static const struct desk_application_entry desk_applications[] = {
    {"netsurf", "NetSurf", "internet-web-browser", "Web browser"},
};
#define DESK_APPLICATION_COUNT (sizeof(desk_applications) / sizeof(desk_applications[0]))
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
    {"Quake (shareware)", "Quake", "\\QUAKE\\QUAKE.EXE", "input-gaming"},
};
#define DESK_GAME_COUNT (sizeof(desk_games) / sizeof(desk_games[0]))
static inline const struct desk_catalog_entry *desk_catalog_find(const char *name)
{
    for (unsigned i = 0; i < DESK_CATALOG_COUNT; i++) if (!strcmp(desk_catalog[i].name, name)) return &desk_catalog[i];
    return 0;
}
static inline const struct desk_application_entry *desk_application_find(const char *name)
{
    for (unsigned i = 0; i < DESK_APPLICATION_COUNT; i++) if (!strcmp(desk_applications[i].name, name)) return &desk_applications[i];
    return 0;
}
/* Default file association, shared by Files and the shell. Case-insensitive
 * extension tokens; the first matching catalogue entry wins. */
static inline int desk_catalog_supports(const struct desk_catalog_entry *entry, const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext || !ext[1] || strchr(ext, '/')) return 0;
    ext++;
    {
        const char *token = entry->extensions;
        while (*token) {
            const char *end = strchr(token, '|'); size_t n = end ? (size_t)(end - token) : strlen(token);
            if (strlen(ext) == n) {
                size_t k;
                for (k = 0; k < n; k++) {
                    unsigned c = (unsigned char)ext[k]; if (c >= 'A' && c <= 'Z') c += 32;
                    if (c != (unsigned char)token[k]) break;
                }
                if (k == n) return 1;
            }
            if (!end) break;
            token = end + 1;
        }
    }
    return 0;
}
static inline const struct desk_catalog_entry *desk_catalog_file(const char *path)
{
    for (unsigned i=0;i<DESK_CATALOG_COUNT;i++) if (desk_catalog_supports(&desk_catalog[i],path)) return &desk_catalog[i];
    return 0;
}
#endif
