/* SPDX-License-Identifier: MIT
 * Setup's fixed tables: the component rows of screen 6 (labels from the
 * approved mockup), keyboard layouts (the Control Panel's ids,
 * apps/desktop/settings.h), time zones, "Did you know?" tips, the texts of
 * the error classes of plan §7, and the size arithmetic of screen 6. */
#include "setup.h"
#include <ciuki/tzrule.h>
#include <stdio.h>

const struct st_component st_components[SETUP_COMP_COUNT] = {
    {SETUP_COMP_SYSTEM, "system", "CiukiOS system, desktop, Control Panel, Files, Music, Paint, Notepad", 0},
    {SETUP_COMP_DOS, "dos", "DOS personality (CiukiDOS, DOS Box, DPMI, Sound Blaster, IPX LAN)", 0},
    {SETUP_COMP_WINDOWS, "windows", "Windows 9x game personality (Win32, DirectX 7 class, OpenGL)", 0},
    {SETUP_COMP_NETSURF, "netsurf", "NetSurf web browser with HTTPS", 0},
    {SETUP_COMP_GAMES, "games", "Shareware games: DOOM, Quake, Wolfenstein 3D", SETUP_COMP_DOS},
    {SETUP_COMP_DEVTOOLS, "devtools", "Developer tools: CiukIDE, TCC, Lua, SDK samples", 0},
    {SETUP_COMP_SOUNDFONT, "soundfont", "General MIDI soundfont for Windows game music", 0},
    {SETUP_COMP_SOURCES, "sources", "Install the source archives", 0},
};

/* Mockup order; the ids are cfg_keyboard_names (apps/desktop/settings.c). */
const struct st_keyboard st_keyboards[] = {
    {"it", "Italian (QWERTY, 142)"},
    {"us", "US English (101)"},
};
const unsigned st_keyboard_count = sizeof(st_keyboards) / sizeof(st_keyboards[0]);

/* G1: zones come from ciuki/tzdata.h (IANA rules); the "+hh:mm" form of
 * SETUP.INF TimeZone= stays a fixed offset (zone < 0 below). */
int st_zone_index(int minutes) { return ciuki_tz_find_minutes(minutes); }
int st_zone_find(const char *id) { return id && id[0] ? ciuki_tz_find(id) : -1; }
static int zone_rule(int zone, int minutes, struct tzr *r)
{
    if (zone >= 0 && zone < (int)st_zone_count && tzr_parse(st_zones[zone].tz, r)) return 1;
    char text[TZR_TEXT];
    return tzr_fixed_text(minutes, text, sizeof(text)) && tzr_parse(text, r);
}
int32_t st_zone_offset(int zone, int minutes, int64_t utc)
{
    struct tzr r;
    return zone_rule(zone, minutes, &r) ? tzr_offset(&r, utc, 0) : minutes * 60;
}
int64_t st_zone_local_to_utc(int zone, int minutes, int64_t local)
{
    struct tzr r;
    return zone_rule(zone, minutes, &r) ? tzr_local_to_utc(&r, local, -1, 0) : local - (int64_t)minutes * 60;
}

/* The first tip is the approved mockup text. */
const char *const st_tips[] = {
    "CiukiOS runs DOS games in their own virtual machine with a Sound Blaster 16 and an IPX network card, so DOOM and Duke Nukem 3D play on the LAN exactly as they did in 1996. Press F12 in the DOS Box to change the kernel, memory and network settings per game.",
    "Everything you choose in Setup can be added or removed later from Control Panel \xe2\x80\xba Programs.",
    "If the screen stays black after a restart, press S at the boot menu for safe video.",
    "Secure web sites (HTTPS) need a correct clock. Setup writes the time you confirmed to the battery-backed clock of this computer.",
    "CiukiOS is dedicated to Ciuki, the dog in the portrait.",
};
const unsigned st_tip_count = sizeof(st_tips) / sizeof(st_tips[0]);

uint64_t st_partition_sectors(uint64_t capacity)
{
    uint64_t limit = (uint64_t)1 << 28;   /* partition.c: start + count <= 2^28 */
    uint64_t end = capacity < limit ? capacity : limit;
    return end > 2048 ? end - 2048 : 0;
}

/* Payload of the selected components, rounded to 4 KiB clusters where the
 * catalogue records it, else bytes plus half a cluster of slack per file. */
uint64_t st_required_estimate(const struct st_app *a, uint32_t components)
{
    uint64_t total = 0;
    for (unsigned i = 0; i < SETUP_COMP_COUNT; i++) {
        const struct st_size *s = &a->sizes[i];
        if (!(components & st_components[i].bit) || !s->known) continue;
        total += s->alloc_bytes ? s->alloc_bytes : s->bytes + (uint64_t)s->files * 2048u;
    }
    return total;
}

static const char *const error_ids[] = {
    "none", "source_read", "source_mismatch", "media_removed", "dest_write", "dest_flush", "dest_full",
    "dest_changed", "lease", "consistency", "cancelled", "memory", "internal"
};
const char *st_error_id(enum setup_error_class c)
{ return (unsigned)c < sizeof(error_ids) / sizeof(error_ids[0]) ? error_ids[c] : "internal"; }

const char *st_error_title(enum setup_error_class c)
{
    switch (c) {
    case SETUP_ERR_SOURCE_READ: return "Setup cannot read the CD";
    case SETUP_ERR_MEDIA_REMOVED: return "Insert the CiukiOS Setup CD";
    case SETUP_ERR_SOURCE_MISMATCH: return "Wrong or damaged Setup CD";
    case SETUP_ERR_DEST_FULL: return "The hard disk is full";
    case SETUP_ERR_DEST_CHANGED: return "The disk has changed";
    case SETUP_ERR_CANCELLED: return "Setup was cancelled";
    case SETUP_ERR_MEMORY: return "Not enough memory";
    default: return "Setup cannot write to the hard disk";
    }
}

const char *st_error_text(enum setup_error_class c)
{
    switch (c) {
    case SETUP_ERR_SOURCE_READ: return "Setup cannot read the CD. Clean the disc and check that the drive is closed, then choose Retry.";
    case SETUP_ERR_MEDIA_REMOVED: return "Insert the CiukiOS Setup CD into the drive, then choose Retry.";
    case SETUP_ERR_SOURCE_MISMATCH: return "The CD in the drive is not the Setup CD that started this installation, or a file on it is damaged.";
    case SETUP_ERR_DEST_WRITE: return "The hard disk did not accept the data written to it.";
    case SETUP_ERR_DEST_FLUSH: return "The disk did not confirm the write.";
    case SETUP_ERR_DEST_FULL: return "The hard disk has no space left for the selected components.";
    case SETUP_ERR_DEST_CHANGED: return "The disk has changed since the preview.";
    case SETUP_ERR_LEASE: return "Setup could not take exclusive control of the hard disk.";
    case SETUP_ERR_CONSISTENCY: return "The check of the new system volume found errors.";
    case SETUP_ERR_CANCELLED: return "Setup was cancelled before the installation was complete.";
    case SETUP_ERR_MEMORY: return "Setup ran out of memory and stopped instead of continuing with less.";
    default: return "Setup stopped because of an internal error.";
    }
}
