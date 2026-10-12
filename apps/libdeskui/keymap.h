/* SPDX-License-Identifier: MIT
 * Keyboard layouts as data: Set-1 scan codes (the codes the desktop input
 * path delivers) to Unicode code points, for US (Windows "US", KBDUS) and
 * Italian (Windows "Italian" 142, KBDIT142) keyboards, with Shift, Caps Lock
 * (letters only) and AltGr (right Alt). The setting and its wiring into text
 * input belong to the desktop shell; du_key_char (US, ASCII) is unchanged. */
#ifndef CIUKI_DESKUI_KEYMAP_H
#define CIUKI_DESKUI_KEYMAP_H
#include <stddef.h>
#include <stdint.h>
enum du_layout { DU_LAYOUT_US, DU_LAYOUT_IT, DU_LAYOUT_COUNT };
struct du_layout_info { const char *code, *name; };
extern const struct du_layout_info du_layouts[DU_LAYOUT_COUNT];
#define DU_KEYMAP_SHIFT 1u
#define DU_KEYMAP_ALTGR 2u
#define DU_KEYMAP_CAPS 4u
/* Code point for a key press, or 0 when the key produces no character. */
uint32_t du_keymap(enum du_layout, unsigned scancode, unsigned state);
/* "us"/"it" (case-insensitive) to the layout; -1 when unknown. */
int du_layout_find(const char *code);
/* UTF-8 encoding of a code point (1..4 bytes, NUL terminated); 0 for invalid. */
size_t du_utf8_encode(uint32_t cp, char out[5]);
#endif
