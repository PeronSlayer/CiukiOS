/* SPDX-License-Identifier: MIT
 * Keyboard layouts (keymap.h). Tables follow the engravings of the US 101/104
 * and Italian 102/105 PC keyboards (Microsoft "Italian" KBDIT layout: ò@, à#,
 * è[, +], AltGr+E €). */
#include "keymap.h"
#include <string.h>

#include "input_text.h"

int ciuki_keymap_layout(const char *name)
{
    return name && (!strcmp(name, "it") || !strcmp(name, "IT") || !strcmp(name, "it_IT")) ? 1 : 0;
}

unsigned ciuki_keymap_char(int layout, unsigned code, unsigned mods)
{ return ciuki_input_char(layout,code,mods); }

size_t ciuki_utf8_encode(unsigned cp, char out[5])
{
    if (cp < 0x80) { out[0] = (char)cp; out[1] = 0; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xc0 | cp >> 6); out[1] = (char)(0x80 | (cp & 63)); out[2] = 0; return 2; }
    if (cp >= 0xd800 && cp <= 0xdfff) { out[0] = 0; return 0; }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12); out[1] = (char)(0x80 | ((cp >> 6) & 63)); out[2] = (char)(0x80 | (cp & 63)); out[3] = 0;
        return 3;
    }
    if (cp > 0x10ffff) { out[0] = 0; return 0; }
    out[0] = (char)(0xf0 | cp >> 18); out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63)); out[3] = (char)(0x80 | (cp & 63)); out[4] = 0;
    return 4;
}
