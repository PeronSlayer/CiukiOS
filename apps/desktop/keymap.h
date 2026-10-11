/* SPDX-License-Identifier: MIT
 * Set-1 scan code to Unicode for the desktop's keyboard layouts (settings.h
 * CFG_KEYS_*): US English and Italian (IBM PC Italian 142, the layout
 * printed on Italian PC keyboards, with AltGr). Shared by the desktop's own
 * text fields and libdeskui. */
#ifndef CIUKI_DESKTOP_KEYMAP_H
#define CIUKI_DESKTOP_KEYMAP_H
#include <stddef.h>
#define CIUKI_KEYMAP_SHIFT 1u
#define CIUKI_KEYMAP_ALTGR 2u
#define CIUKI_KEYMAP_CAPS 4u
/* Returns the code point, or 0 when the key produces no character. */
unsigned ciuki_keymap_char(int layout, unsigned code, unsigned modifiers);
/* Layout index for a settings name ("us", "it") or CIUKI_KEYMAP env value; 0 when unknown. */
int ciuki_keymap_layout(const char *name);
/* UTF-8 encoding of a code point; returns the byte count (0 for invalid). */
size_t ciuki_utf8_encode(unsigned cp, char out[5]);
#endif
