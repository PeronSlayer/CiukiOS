/* Keyboard layout tables (US, Italian) and the recent-documents list.
 * SPDX-License-Identifier: MIT */
#include "keymap.h"
#include "recent.h"
#include "deskui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    /* US agrees with libdeskui's du_key_char for every printable key. */
    static struct du_ui ui; static uint32_t px[4];
    du_init(&ui, px, 2, 2);
    for (unsigned code = 0; code < 0x36; code++) for (int shift = 0; shift < 2; shift++) {
        ui.modifiers = shift ? DU_SHIFT : 0;
        unsigned ascii = du_key_char(&ui, code);
        if (ascii) assert(du_keymap(DU_LAYOUT_US, code, shift ? DU_KEYMAP_SHIFT : 0) == ascii);
    }
    assert(du_keymap(DU_LAYOUT_US, 0x10, DU_KEYMAP_CAPS) == 'Q' && du_keymap(DU_LAYOUT_US, 0x10, DU_KEYMAP_CAPS | DU_KEYMAP_SHIFT) == 'q');
    assert(du_keymap(DU_LAYOUT_US, 0x02, DU_KEYMAP_CAPS) == '1');
    /* Italian (KBDIT142). */
    assert(du_keymap(DU_LAYOUT_IT, 0x1a, 0) == 0xe8 && du_keymap(DU_LAYOUT_IT, 0x1a, DU_KEYMAP_SHIFT) == 0xe9);   /* è é */
    assert(du_keymap(DU_LAYOUT_IT, 0x27, 0) == 0xf2 && du_keymap(DU_LAYOUT_IT, 0x27, DU_KEYMAP_SHIFT) == 0xe7);   /* ò ç */
    assert(du_keymap(DU_LAYOUT_IT, 0x28, 0) == 0xe0 && du_keymap(DU_LAYOUT_IT, 0x28, DU_KEYMAP_SHIFT) == 0xb0);   /* à ° */
    assert(du_keymap(DU_LAYOUT_IT, 0x2b, 0) == 0xf9 && du_keymap(DU_LAYOUT_IT, 0x2b, DU_KEYMAP_SHIFT) == 0xa7);   /* ù § */
    assert(du_keymap(DU_LAYOUT_IT, 0x0d, 0) == 0xec && du_keymap(DU_LAYOUT_IT, 0x0d, DU_KEYMAP_SHIFT) == '^');    /* ì ^ */
    assert(du_keymap(DU_LAYOUT_IT, 0x04, DU_KEYMAP_SHIFT) == 0xa3 && du_keymap(DU_LAYOUT_IT, 0x08, DU_KEYMAP_SHIFT) == '/');
    assert(du_keymap(DU_LAYOUT_IT, 0x12, DU_KEYMAP_ALTGR) == 0x20ac);                                            /* € */
    assert(du_keymap(DU_LAYOUT_IT, 0x27, DU_KEYMAP_ALTGR) == '@' && du_keymap(DU_LAYOUT_IT, 0x28, DU_KEYMAP_ALTGR) == '#');
    assert(du_keymap(DU_LAYOUT_IT, 0x1a, DU_KEYMAP_ALTGR) == '[' && du_keymap(DU_LAYOUT_IT, 0x1b, DU_KEYMAP_ALTGR | DU_KEYMAP_SHIFT) == '}');
    assert(du_keymap(DU_LAYOUT_IT, 0x56, 0) == '<' && du_keymap(DU_LAYOUT_IT, 0x56, DU_KEYMAP_SHIFT) == '>');
    assert(du_keymap(DU_LAYOUT_IT, 0x35, 0) == '-' && du_keymap(DU_LAYOUT_IT, 0x35, DU_KEYMAP_SHIFT) == '_');
    assert(du_keymap(DU_LAYOUT_IT, 0x1a, DU_KEYMAP_CAPS) == 0xe8);   /* Caps Lock leaves accented letters */
    assert(du_keymap(DU_LAYOUT_IT, 0x1e, DU_KEYMAP_CAPS) == 'A' && !du_keymap(DU_LAYOUT_IT, 0x01, 0) && !du_keymap(DU_LAYOUT_IT, 0x200, 0));
    assert(du_keymap(DU_LAYOUT_IT, 0x39, DU_KEYMAP_ALTGR) == ' ');
    assert(du_layout_find("IT") == DU_LAYOUT_IT && du_layout_find("us") == DU_LAYOUT_US && du_layout_find("fr") == -1);
    char s[5];
    assert(du_utf8_encode(0xe8, s) == 2 && !strcmp(s, "\xc3\xa8"));
    assert(du_utf8_encode(0x20ac, s) == 3 && !strcmp(s, "\xe2\x82\xac"));
    assert(du_utf8_encode('a', s) == 1 && du_utf8_encode(0xd800, s) == 0 && du_utf8_encode(0x110000, s) == 0);
    /* Recent documents (model only; the file helpers are covered by the viewer test). */
    struct du_recent r = { .count = 0 };
    assert(du_recent_push(&r, "relative") == -1 && du_recent_push(&r, "/a\nb") == -1);
    for (int i = 0; i < DU_RECENT_MAX + 3; i++) { char p[32]; snprintf(p, sizeof(p), "/home/doc%d.txt", i); assert(!du_recent_push(&r, p)); }
    assert(r.count == DU_RECENT_MAX && !strcmp(r.paths[0], "/home/doc17.txt") && !strcmp(r.paths[DU_RECENT_MAX - 1], "/home/doc3.txt"));
    assert(!du_recent_push(&r, "/home/doc10.txt") && !strcmp(r.paths[0], "/home/doc10.txt") && r.count == DU_RECENT_MAX &&
           !strcmp(r.paths[1], "/home/doc17.txt") && !strcmp(r.paths[DU_RECENT_MAX - 1], "/home/doc3.txt"));
    char text[DU_RECENT_MAX * (DU_RECENT_PATH + 1) + 1];
    size_t n = du_recent_format(&r, text, sizeof(text));
    struct du_recent back; du_recent_parse(&back, text, n);
    assert(back.count == r.count && !memcmp(back.paths, r.paths, sizeof(r.paths)));
    du_recent_parse(&back, "/x\r\n\nbad\n/x\n/y", 14);
    assert(back.count == 2 && !strcmp(back.paths[0], "/x") && !strcmp(back.paths[1], "/y"));
    puts("keymap+recent: PASS");
    return 0;
}
