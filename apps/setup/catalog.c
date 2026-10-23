/* SPDX-License-Identifier: MIT
 * Setup's fixed tables: the component rows of screen 6 (labels from the
 * approved mockup), keyboard layouts (the Control Panel's ids,
 * apps/desktop/settings.h), time zones, "Did you know?" tips, the texts of
 * the error classes of plan §7, and the size arithmetic of screen 6. */
#include "setup.h"
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

/* Standard-time offsets (TimeZone= has no daylight-saving rule). */
const struct st_zone st_zones[] = {
    {-720, "UTC-12:00 Baker Island"}, {-660, "UTC-11:00 Pago Pago"}, {-600, "UTC-10:00 Honolulu"},
    {-570, "UTC-09:30 Marquesas Islands"}, {-540, "UTC-09:00 Anchorage"}, {-480, "UTC-08:00 Los Angeles, Vancouver"},
    {-420, "UTC-07:00 Denver, Phoenix"}, {-360, "UTC-06:00 Chicago, Mexico City"},
    {-300, "UTC-05:00 New York, Toronto, Bogota"}, {-240, "UTC-04:00 Halifax, Caracas"},
    {-210, "UTC-03:30 St. John's"}, {-180, "UTC-03:00 Buenos Aires, Sao Paulo"}, {-120, "UTC-02:00 South Georgia"},
    {-60, "UTC-01:00 Azores, Cape Verde"}, {0, "UTC+00:00 London, Lisbon"}, {60, "UTC+01:00 Rome, Berlin, Paris"},
    {120, "UTC+02:00 Athens, Helsinki, Cairo"}, {180, "UTC+03:00 Moscow, Istanbul, Nairobi"},
    {210, "UTC+03:30 Tehran"}, {240, "UTC+04:00 Dubai, Baku"}, {270, "UTC+04:30 Kabul"},
    {300, "UTC+05:00 Karachi, Tashkent"}, {330, "UTC+05:30 Mumbai, New Delhi"}, {345, "UTC+05:45 Kathmandu"},
    {360, "UTC+06:00 Dhaka"}, {390, "UTC+06:30 Yangon"}, {420, "UTC+07:00 Bangkok, Jakarta"},
    {480, "UTC+08:00 Beijing, Singapore, Perth"}, {540, "UTC+09:00 Tokyo, Seoul"},
    {570, "UTC+09:30 Adelaide, Darwin"}, {600, "UTC+10:00 Sydney, Brisbane"}, {660, "UTC+11:00 Noumea"},
    {720, "UTC+12:00 Auckland, Fiji"}, {780, "UTC+13:00 Nuku'alofa"}, {840, "UTC+14:00 Kiritimati"},
};
const unsigned st_zone_count = sizeof(st_zones) / sizeof(st_zones[0]);

int st_zone_index(int minutes)
{
    for (unsigned i = 0; i < st_zone_count; i++) if (st_zones[i].minutes == minutes) return (int)i;
    return -1;
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
