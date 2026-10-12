/* SPDX-License-Identifier: MIT
 * CiukiOS objects -> Tango 0.8.90 icon names (icons.c, deskui.h).
 *
 * Tango 0.8.90 follows the freedesktop.org Icon Naming Specification draft
 * of its time (https://specifications.freedesktop.org/icon-naming-spec/latest/);
 * names here exist in the pinned archive at 16, 22 and 32 px
 * (48 px derived), which tests/host/desktop/test_icons.py verifies for every
 * entry. Identity objects (Computer, About, the Ciuki button) use the
 * approved portrait (DU_ICON_CIUKI), never a Tango or redrawn dog.
 * One icon per object everywhere: the desktop shell's own tables (shell.c
 * programs[] and Start menu, catalog.h) use the same Tango names as the
 * program:, menu: and settings: entries below (test_icons.py checks it), so
 * a program shows one icon in the Start menu, Files' /Programs and Task Manager. */
#include "deskui.h"
#include <string.h>

const struct du_icon_map du_icon_map[] = {
    /* /Programs programs (the desktop menu lists /Programs) */
    {"program:*", "application-x-executable"},
    {"program:files", "system-file-manager"},
    {"program:control", "preferences-desktop"},
    {"program:ciuknote", "accessories-text-editor"},
    {"program:tasks", "utilities-system-monitor"},
    {"program:dosbox", "utilities-terminal"},          /* MS-DOS Prompt class VM window */
    {"program:demo", "preferences-desktop-peripherals"}, /* keyboard/mouse input demo window */
    {"program:hello", "face-smile"},                   /* prints a greeting */
    {"program:lua", "text-x-script"},                  /* Lua interpreter (runs scripts; the Developer folder is applications-development) */
    {"program:libc_smoke", "applications-system"},     /* C library self-test */
    {"program:desktop", "user-desktop"},
    {"program:setup", "system-installer"},
    /* Accessories (apps/desktop/catalog.h: the launcher's Tango names). */
    {"program:calc", "accessories-calculator"},
    {"program:charmap", "accessories-character-map"},
    {"program:clock", "appointment-new"},              /* a clock face (Tango 0.8.90 has no clock app icon) */
    {"program:help", "help-browser"},
    {"program:mixer", "audio-volume-high"},
    {"program:viewer", "image-x-generic"},

    /* Start menu */
    {"menu:*", "application-x-executable"},
    {"menu:start", DU_ICON_CIUKI},
    {"menu:about", DU_ICON_CIUKI},
    {"menu:programs", "applications-other"},
    {"menu:documents", "document-open"},
    {"menu:settings", "preferences-system"},
    {"menu:control-panel", "preferences-desktop"},
    {"menu:find", "system-search"},
    {"menu:help", "help-browser"},
    {"menu:run", "go-jump"},
    {"menu:files", "system-file-manager"},
    {"menu:tasks", "utilities-system-monitor"},
    {"menu:shutdown", "system-shutdown"},
    {"menu:restart", "view-refresh"},
    {"menu:logout", "system-log-out"},
    {"menu:lock", "system-lock-screen"},
    {"menu:accessories", "applications-accessories"},
    {"menu:development", "applications-development"},
    {"menu:games", "applications-games"},
    {"menu:game", "input-gaming"},                     /* a game of Programs > Games (catalog.h desk_games) */
    {"menu:graphics", "applications-graphics"},
    {"menu:internet", "applications-internet"},
    {"menu:multimedia", "applications-multimedia"},
    {"menu:office", "applications-office"},
    {"menu:system-tools", "applications-system"},

    /* Toolbar and menu commands */
    {"action:new", "document-new"},
    {"action:open", "document-open"},
    {"action:save", "document-save"},
    {"action:save-as", "document-save-as"},
    {"action:print", "document-print"},
    {"action:print-preview", "document-print-preview"},
    {"action:properties", "document-properties"},
    {"action:undo", "edit-undo"},
    {"action:redo", "edit-redo"},
    {"action:cut", "edit-cut"},
    {"action:copy", "edit-copy"},
    {"action:paste", "edit-paste"},
    {"action:delete", "edit-delete"},
    {"action:select-all", "edit-select-all"},
    {"action:find", "edit-find"},
    {"action:replace", "edit-find-replace"},
    {"action:clear", "edit-clear"},
    {"action:back", "go-previous"},
    {"action:forward", "go-next"},
    {"action:up", "go-up"},
    {"action:down", "go-down"},
    {"action:home", "go-home"},
    {"action:top", "go-top"},
    {"action:bottom", "go-bottom"},
    {"action:first", "go-first"},
    {"action:last", "go-last"},
    {"action:go", "go-jump"},
    {"action:refresh", "view-refresh"},
    {"action:stop", "process-stop"},
    {"action:end-task", "process-stop"},
    {"action:switch-to", "go-jump"},
    {"action:run", "go-jump"},
    {"action:views", "format-justify-fill"},
    {"action:fullscreen", "view-fullscreen"},
    {"action:new-folder", "folder-new"},
    {"action:new-window", "window-new"},
    {"action:new-tab", "tab-new"},
    {"action:add", "list-add"},
    {"action:remove", "list-remove"},
    {"action:search", "system-search"},
    {"action:bookmark", "bookmark-new"},
    {"action:bold", "format-text-bold"},
    {"action:italic", "format-text-italic"},
    {"action:underline", "format-text-underline"},
    {"action:strikethrough", "format-text-strikethrough"},
    {"action:align-left", "format-justify-left"},
    {"action:align-center", "format-justify-center"},
    {"action:align-right", "format-justify-right"},
    {"action:justify", "format-justify-fill"},
    {"action:indent", "format-indent-more"},
    {"action:unindent", "format-indent-less"},
    {"action:play", "media-playback-start"},
    {"action:pause", "media-playback-pause"},
    {"action:media-stop", "media-playback-stop"},
    {"action:record", "media-record"},
    {"action:eject", "media-eject"},
    {"action:seek-backward", "media-seek-backward"},
    {"action:seek-forward", "media-seek-forward"},
    {"action:skip-backward", "media-skip-backward"},
    {"action:skip-forward", "media-skip-forward"},
    {"action:shutdown", "system-shutdown"},
    {"action:restart", "view-refresh"},
    {"action:logout", "system-log-out"},

    /* Places */
    {"place:*", "folder"},
    {"place:computer", DU_ICON_CIUKI},
    {"place:home", "user-home"},
    {"place:desktop", "user-desktop"},
    {"place:documents", "folder"},
    {"place:applications", "applications-other"},
    {"place:system", "folder"},
    {"place:folder", "folder"},
    {"place:harddisk", "drive-harddisk"},
    {"place:volume", "drive-harddisk"},
    {"place:removable", "drive-removable-media"},
    {"place:floppy", "media-floppy"},
    {"place:optical", "drive-optical"},
    {"place:cdrom", "media-optical"},
    {"place:flash", "media-flash"},
    {"place:network", "network-workgroup"},
    {"place:server", "network-server"},
    {"place:remote", "folder-remote"},
    {"place:trash", "user-trash"},
    {"place:trash-full", "user-trash-full"},
    {"place:saved-search", "folder-saved-search"},

    /* File types (du_icon_file) */
    {"type:*", "text-x-generic-template"},
    {"type:unknown", "text-x-generic-template"},
    {"type:folder", "folder"},
    {"type:folder-open", "folder-open"},
    {"type:folder-drop", "folder-drag-accept"},
    {"type:folder-remote", "folder-remote"},
    {"type:executable", "application-x-executable"},
    {"type:.txt", "text-x-generic"}, {"type:.text", "text-x-generic"}, {"type:.md", "text-x-generic"},
    {"type:.log", "text-x-generic"}, {"type:.me", "text-x-generic"}, {"type:.nfo", "text-x-generic"},
    {"type:.diz", "text-x-generic"}, {"type:.asc", "text-x-generic"}, {"type:.1st", "text-x-generic"},
    {"type:.cfg", "emblem-system"}, {"type:.ini", "emblem-system"}, {"type:.conf", "emblem-system"},
    {"type:.inf", "emblem-system"}, {"type:.json", "emblem-system"}, {"type:.xml", "emblem-system"},
    {"type:.yaml", "emblem-system"}, {"type:.yml", "emblem-system"}, {"type:.toml", "emblem-system"},
    {"type:.reg", "emblem-system"}, {"type:.pif", "emblem-system"},
    {"type:.htm", "text-html"}, {"type:.html", "text-html"},
    {"type:.lua", "text-x-script"}, {"type:.bat", "text-x-script"}, {"type:.cmd", "text-x-script"},
    {"type:.sh", "text-x-script"}, {"type:.py", "text-x-script"}, {"type:.pl", "text-x-script"},
    {"type:.awk", "text-x-script"}, {"type:.c", "text-x-script"}, {"type:.h", "text-x-script"},
    {"type:.cpp", "text-x-script"}, {"type:.asm", "text-x-script"}, {"type:.s", "text-x-script"},
    {"type:.inc", "text-x-script"}, {"type:.pas", "text-x-script"}, {"type:.bas", "text-x-script"},
    {"type:.mak", "text-x-script"},
    {"type:.com", "application-x-executable"}, {"type:.exe", "application-x-executable"},
    {"type:.elf", "application-x-executable"}, {"type:.sys", "application-x-executable"},
    {"type:.drv", "application-x-executable"}, {"type:.ovl", "application-x-executable"},
    {"type:.dll", "application-x-executable"},
    {"type:.bmp", "image-x-generic"}, {"type:.png", "image-x-generic"}, {"type:.xrgb", "image-x-generic"},
    {"type:.pcx", "image-x-generic"}, {"type:.gif", "image-x-generic"}, {"type:.jpg", "image-x-generic"},
    {"type:.jpeg", "image-x-generic"}, {"type:.tga", "image-x-generic"}, {"type:.ico", "image-x-generic"},
    {"type:.ppm", "image-x-generic"}, {"type:.lbm", "image-x-generic"}, {"type:.tif", "image-x-generic"},
    {"type:.tiff", "image-x-generic"}, {"type:.svg", "image-x-generic"},
    {"type:.wav", "audio-x-generic"}, {"type:.mid", "audio-x-generic"}, {"type:.midi", "audio-x-generic"},
    {"type:.voc", "audio-x-generic"}, {"type:.mod", "audio-x-generic"}, {"type:.s3m", "audio-x-generic"},
    {"type:.xm", "audio-x-generic"}, {"type:.it", "audio-x-generic"}, {"type:.mp3", "audio-x-generic"},
    {"type:.ogg", "audio-x-generic"}, {"type:.au", "audio-x-generic"}, {"type:.aif", "audio-x-generic"},
    {"type:.cmf", "audio-x-generic"}, {"type:.rol", "audio-x-generic"}, {"type:.flac", "audio-x-generic"},
    {"type:.avi", "video-x-generic"}, {"type:.mpg", "video-x-generic"}, {"type:.mpeg", "video-x-generic"},
    {"type:.flc", "video-x-generic"}, {"type:.fli", "video-x-generic"}, {"type:.mov", "video-x-generic"},
    {"type:.mp4", "video-x-generic"}, {"type:.mkv", "video-x-generic"}, {"type:.webm", "video-x-generic"},
    {"type:.zip", "package-x-generic"}, {"type:.arj", "package-x-generic"}, {"type:.lzh", "package-x-generic"},
    {"type:.lha", "package-x-generic"}, {"type:.rar", "package-x-generic"}, {"type:.7z", "package-x-generic"},
    {"type:.tar", "package-x-generic"}, {"type:.gz", "package-x-generic"}, {"type:.tgz", "package-x-generic"},
    {"type:.bz2", "package-x-generic"}, {"type:.xz", "package-x-generic"}, {"type:.cab", "package-x-generic"},
    {"type:.zoo", "package-x-generic"},
    {"type:.ttf", "font-x-generic"}, {"type:.otf", "font-x-generic"}, {"type:.fon", "font-x-generic"},
    {"type:.fnt", "font-x-generic"}, {"type:.psf", "font-x-generic"}, {"type:.bdf", "font-x-generic"},
    {"type:.chr", "font-x-generic"},
    {"type:.img", "media-floppy"}, {"type:.ima", "media-floppy"}, {"type:.vfd", "media-floppy"},
    {"type:.dsk", "media-floppy"}, {"type:.iso", "media-optical"}, {"type:.cue", "media-optical"},
    {"type:.vhd", "drive-harddisk"}, {"type:.qcow2", "drive-harddisk"},
    {"type:.doc", "x-office-document"}, {"type:.rtf", "x-office-document"}, {"type:.wri", "x-office-document"},
    {"type:.odt", "x-office-document"}, {"type:.pdf", "x-office-document"}, {"type:.ps", "x-office-document"},
    {"type:.xls", "x-office-spreadsheet"}, {"type:.csv", "x-office-spreadsheet"}, {"type:.ods", "x-office-spreadsheet"},
    {"type:.wk1", "x-office-spreadsheet"}, {"type:.ppt", "x-office-presentation"}, {"type:.odp", "x-office-presentation"},
    {"type:.odg", "x-office-drawing"}, {"type:.vcf", "x-office-address-book"}, {"type:.ics", "x-office-calendar"},
    {"type:.pem", "application-certificate"}, {"type:.crt", "application-certificate"},
    {"type:.cer", "application-certificate"},

    /* Status (tray, notifications) */
    {"status:*", "dialog-information"},
    {"status:volume-high", "audio-volume-high"},
    {"status:volume-medium", "audio-volume-medium"},
    {"status:volume-low", "audio-volume-low"},
    {"status:volume-muted", "audio-volume-muted"},
    {"status:battery", "battery"},
    {"status:battery-low", "battery-caution"},
    {"status:network-idle", "network-idle"},
    {"status:network-offline", "network-offline"},
    {"status:network-error", "network-error"},
    {"status:network-transmit", "network-transmit"},
    {"status:network-receive", "network-receive"},
    {"status:network-transmit-receive", "network-transmit-receive"},
    {"status:network-wireless-encrypted", "network-wireless-encrypted"},
    {"status:update-available", "software-update-available"},
    {"status:update-urgent", "software-update-urgent"},
    {"status:printer-error", "printer-error"},
    {"status:image-loading", "image-loading"},
    {"status:image-missing", "image-missing"},
    {"status:attachment", "mail-attachment"},
    {"status:busy", "process-working-01"},
    {"status:info", "dialog-information"},
    {"status:warning", "dialog-warning"},
    {"status:error", "dialog-error"},
    {"status:important", "emblem-important"},

    /* Message boxes (du_message icons) */
    {"dialog:*", "dialog-information"},
    {"dialog:info", "dialog-information"},
    {"dialog:warning", "dialog-warning"},
    {"dialog:error", "dialog-error"},
    {"dialog:question", "help-browser"},              /* Tango 0.8.90 has no dialog-question */

    /* Devices and hardware */
    {"device:*", "drive-harddisk"},
    {"device:computer", DU_ICON_CIUKI},
    {"device:harddisk", "drive-harddisk"},
    {"device:removable", "drive-removable-media"},
    {"device:floppy", "media-floppy"},
    {"device:cdrom", "drive-optical"},
    {"device:flash", "media-flash"},
    {"device:keyboard", "input-keyboard"},
    {"device:mouse", "input-mouse"},
    {"device:joystick", "input-gaming"},
    {"device:display", "video-display"},
    {"device:sound-card", "audio-card"},
    {"device:microphone", "audio-input-microphone"},
    {"device:printer", "printer"},
    {"device:camera", "camera-photo"},
    {"device:video-camera", "camera-video"},
    {"device:network", "network-wired"},
    {"device:wireless", "network-wireless"},
    {"device:battery", "battery"},
    {"device:media-player", "multimedia-player"},

    /* Settings (control panel items) */
    {"settings:*", "preferences-system"},
    {"settings:system", "preferences-system"},
    {"settings:desktop", "preferences-desktop"},
    {"settings:display", "preferences-desktop-wallpaper"}, /* Settings > Display: the wallpaper and theme page */
    {"settings:theme", "preferences-desktop-theme"},
    {"settings:wallpaper", "preferences-desktop-wallpaper"},
    {"settings:screensaver", "preferences-desktop-screensaver"},
    {"settings:fonts", "preferences-desktop-font"},
    {"settings:keyboard", "input-keyboard"},
    {"settings:shortcuts", "preferences-desktop-keyboard-shortcuts"},
    {"settings:mouse", "input-mouse"},
    {"settings:game-controllers", "input-gaming"},
    {"settings:sound", "audio-card"},
    {"settings:accessibility", "preferences-desktop-accessibility"},
    {"settings:locale", "preferences-desktop-locale"},
    {"settings:date-time", "office-calendar"},
    {"settings:network", "preferences-system-network-proxy"},
    {"settings:users", "system-users"},
    {"settings:windows", "preferences-system-windows"},
    {"settings:session", "preferences-system-session"},
    {"settings:remote-desktop", "preferences-desktop-remote-desktop"},
    {"settings:peripherals", "preferences-desktop-peripherals"},
    {"settings:update", "system-software-update"},
    {"settings:installer", "system-installer"},

    /* Menu categories */
    {"category:*", "applications-other"},
    {"category:accessories", "applications-accessories"},
    {"category:development", "applications-development"},
    {"category:games", "applications-games"},
    {"category:graphics", "applications-graphics"},
    {"category:internet", "applications-internet"},
    {"category:multimedia", "applications-multimedia"},
    {"category:office", "applications-office"},
    {"category:system", "applications-system"},
    {"category:other", "applications-other"},
    {"category:settings", "preferences-system"},

    /* Emblems (overlays) */
    {"emblem:*", "emblem-important"},
    {"emblem:favorite", "emblem-favorite"},
    {"emblem:important", "emblem-important"},
    {"emblem:readonly", "emblem-readonly"},
    {"emblem:unreadable", "emblem-unreadable"},
    {"emblem:symlink", "emblem-symbolic-link"},
    {"emblem:system", "emblem-system"},
    {"emblem:photos", "emblem-photos"},

    /* Window-manager buttons, for a toolkit that draws them with icons */
    {"wm:*", "window-new"},
    {"wm:close", "process-stop"},
    {"wm:minimize", "go-bottom"},
    {"wm:maximize", "view-fullscreen"},
    {"wm:new-window", "window-new"},
};
const unsigned du_icon_map_count = sizeof(du_icon_map) / sizeof(du_icon_map[0]);

static const char *exact(const char *object)
{
    for (unsigned i = 0; i < du_icon_map_count; i++)
        if (!strcmp(du_icon_map[i].object, object)) return du_icon_map[i].tango;
    return NULL;
}

const char *du_icon_name(const char *object)
{
    if (!object) return NULL;
    const char *colon = strchr(object, ':'), *found;
    if (!colon) return object;
    if ((found = exact(object))) return found;
    char kind[24];
    size_t n = (size_t)(colon - object);
    if (n + 3 > sizeof(kind)) return NULL;
    memcpy(kind, object, n);
    memcpy(kind + n, ":*", 3);
    return exact(kind);
}

/* Directory, then a native executable (by its /Programs program name), then the
 * extension (case-insensitive), then the unknown-file icon. */
const char *du_icon_file(const char *filename, int directory, int executable)
{
    if (directory) return du_icon_name("type:folder");
    const char *base = filename ? strrchr(filename, '/') : NULL, *dot, *found;
    base = base ? base + 1 : filename ? filename : "";
    char key[80];
    if (executable) {
        if (strlen(base) < sizeof(key) - 8) {
            strcpy(key, "program:");
            strcat(key, base);
            if ((found = exact(key))) return found;
        }
        return du_icon_name("type:executable");
    }
    dot = strrchr(base, '.');
    if (dot && dot != base && dot[1] && strlen(dot) < 16) {
        size_t k = 5;
        memcpy(key, "type:", 5);
        for (const char *p = dot; *p; p++) key[k++] = (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
        key[k] = 0;
        if ((found = exact(key))) return found;
    }
    return du_icon_name("type:unknown");
}

const struct du_icon *du_icon_for(const char *object, unsigned size)
{ return du_icon(du_icon_name(object), size); }
