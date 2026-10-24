/* SPDX-License-Identifier: MIT
 * One settings hierarchy for the panel, Start and tray shortcuts. */
#ifndef CIUKI_SETTINGS_CATALOG_H
#define CIUKI_SETTINGS_CATALOG_H
#define DESK_SETTINGS_SAVER_PREVIEW 10u /* kind 0..2, full-screen until input */
#define DESK_SETTINGS_PROGRAMS 9u /* values zero; program and launcher inventory changed */
#define DESK_SETTINGS_DISPLAY_PREVIEW 8u /* mode id; Control Panel owns persistence */
#define DESK_SETTINGS_OPEN_POWER 7u /* values zero; open the shell confirmation */
enum { CP_HOME = -1, CP_DISPLAY, CP_MOUSE, CP_KEYBOARD, CP_SOUNDS, CP_DATETIME, CP_SYSTEM,
       CP_DEVICES, CP_DRIVERS, CP_NETWORK, CP_POWER, CP_ADAPTER, CP_FONTS, CP_AUTOPLAY, CP_UNINSTALL, CP_JOYSTICK, CP_PAGES };
enum { CP_CAT_APPEARANCE, CP_CAT_SOUND, CP_CAT_INPUT, CP_CAT_TIME,
       CP_CAT_CONNECTIONS, CP_CAT_DEVICES, CP_CAT_SYSTEM, CP_CATEGORIES };
struct cp_category { const char *label, *icon; };
static const struct cp_category cp_categories[CP_CATEGORIES] = {
    {"Appearance", "preferences-desktop-theme"}, {"Sound", "audio-card"},
    {"Input", "input-keyboard"}, {"Time", "office-calendar"},
    {"Connections", "network-wired"}, {"Power and devices", "drive-removable-media"},
    {"System", "ciuki"}
};
struct cp_applet { int page, category; const char *name, *label, *icon; };
static const struct cp_applet cp_applets[] = {
    {CP_DISPLAY, CP_CAT_APPEARANCE, "display", "Display", "preferences-desktop-wallpaper"},
    {CP_SOUNDS, CP_CAT_SOUND, "sounds", "Sounds and Audio", "audio-card"},
    {CP_KEYBOARD, CP_CAT_INPUT, "keyboard", "Keyboard", "input-keyboard"},
    {CP_MOUSE, CP_CAT_INPUT, "mouse", "Mouse", "input-mouse"},
    {CP_JOYSTICK, CP_CAT_INPUT, "joystick", "Game controllers", "input-gaming"},
    {CP_DATETIME, CP_CAT_TIME, "datetime", "Date and Time", "office-calendar"},
    {CP_NETWORK, CP_CAT_CONNECTIONS, "network", "Network", "network-wired"},
    {CP_POWER, CP_CAT_DEVICES, "power", "Power", "system-shutdown"},
    {CP_AUTOPLAY, CP_CAT_DEVICES, "storage", "Storage and removable devices", "drive-removable-media"},
    {CP_UNINSTALL, CP_CAT_SYSTEM, "programs", "Add/Remove Programs", "application-x-executable"},
    {CP_SYSTEM, CP_CAT_SYSTEM, "system", "System", "ciuki"}
};
#define CP_APPLETS (sizeof(cp_applets) / sizeof(cp_applets[0]))
#endif
