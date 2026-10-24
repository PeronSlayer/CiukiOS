/* SPDX-License-Identifier: MIT
 * Shared atlas ordering for converter, compositor and toolkit. Keep existing
 * indices stable: ASCII, Latin-1, bullet and ellipsis precede additions. */
#ifndef CIUKI_FONT_CODES_H
#define CIUKI_FONT_CODES_H
static const unsigned desk_font_extra_codes[] = {
    0x2022, 0x2026, 0x2010, 0x2011, 0x2013, 0x2014,
    0x2018, 0x2019, 0x201c, 0x201d, 0x203a, 0x20ac,
    0x2190, 0x2191, 0x2192, 0x2193, 0x2212
};
#define DESK_FONT_CODE_COUNT (191 + sizeof(desk_font_extra_codes) / sizeof(*desk_font_extra_codes))
/* -1 is genuinely outside the atlas, not a substituted '?' glyph. */
static inline int desk_font_code_index(unsigned c)
{
    if (c>=0x20 && c<0x7f) return (int)c-0x20;
    if (c>=0xa0 && c<=0xff) return 95+(int)c-0xa0;
    for (unsigned i=0;i<sizeof(desk_font_extra_codes)/sizeof(*desk_font_extra_codes);i++)
        if (c==desk_font_extra_codes[i]) return 191+(int)i;
    return -1;
}
#endif
