/* SPDX-License-Identifier: MIT
 * US and Italian keyboard layouts; see keymap.h. Tables are indexed by Set-1
 * scan code 0x00..0x58; entries are UTF-8 strings ("" = no character). The
 * layouts follow the Windows KBDUS and KBDIT142 definitions: Italian AltGr
 * gives the euro sign on E, [ ] { } on the egrave/plus keys, @ and # on the
 * ograve/agrave keys. */
#include "keymap.h"
#include <string.h>
#include <strings.h>

const struct du_layout_info du_layouts[DU_LAYOUT_COUNT] = { {"us", "English (United States)"}, {"it", "Italian"} };
#define KEYS 0x59
struct layer { const char *k[KEYS]; };
/* Unlisted scan codes produce no character. Space is handled in every layer. */
static const struct layer us_plain = {{
    [0x02]="1",[0x03]="2",[0x04]="3",[0x05]="4",[0x06]="5",[0x07]="6",[0x08]="7",[0x09]="8",[0x0a]="9",[0x0b]="0",[0x0c]="-",[0x0d]="=",
    [0x10]="q",[0x11]="w",[0x12]="e",[0x13]="r",[0x14]="t",[0x15]="y",[0x16]="u",[0x17]="i",[0x18]="o",[0x19]="p",[0x1a]="[",[0x1b]="]",
    [0x1e]="a",[0x1f]="s",[0x20]="d",[0x21]="f",[0x22]="g",[0x23]="h",[0x24]="j",[0x25]="k",[0x26]="l",[0x27]=";",[0x28]="'",[0x29]="`",
    [0x2b]="\\",[0x2c]="z",[0x2d]="x",[0x2e]="c",[0x2f]="v",[0x30]="b",[0x31]="n",[0x32]="m",[0x33]=",",[0x34]=".",[0x35]="/",
    [0x39]=" ",[0x56]="\\" }};
static const struct layer us_shift = {{
    [0x02]="!",[0x03]="@",[0x04]="#",[0x05]="$",[0x06]="%",[0x07]="^",[0x08]="&",[0x09]="*",[0x0a]="(",[0x0b]=")",[0x0c]="_",[0x0d]="+",
    [0x10]="Q",[0x11]="W",[0x12]="E",[0x13]="R",[0x14]="T",[0x15]="Y",[0x16]="U",[0x17]="I",[0x18]="O",[0x19]="P",[0x1a]="{",[0x1b]="}",
    [0x1e]="A",[0x1f]="S",[0x20]="D",[0x21]="F",[0x22]="G",[0x23]="H",[0x24]="J",[0x25]="K",[0x26]="L",[0x27]=":",[0x28]="\"",[0x29]="~",
    [0x2b]="|",[0x2c]="Z",[0x2d]="X",[0x2e]="C",[0x2f]="V",[0x30]="B",[0x31]="N",[0x32]="M",[0x33]="<",[0x34]=">",[0x35]="?",
    [0x39]=" ",[0x56]="|" }};
static const struct layer us_altgr = {{ [0x39]=" " }};
static const struct layer it_plain = {{
    [0x02]="1",[0x03]="2",[0x04]="3",[0x05]="4",[0x06]="5",[0x07]="6",[0x08]="7",[0x09]="8",[0x0a]="9",[0x0b]="0",[0x0c]="'",[0x0d]="\xc3\xac",
    [0x10]="q",[0x11]="w",[0x12]="e",[0x13]="r",[0x14]="t",[0x15]="y",[0x16]="u",[0x17]="i",[0x18]="o",[0x19]="p",[0x1a]="\xc3\xa8",[0x1b]="+",
    [0x1e]="a",[0x1f]="s",[0x20]="d",[0x21]="f",[0x22]="g",[0x23]="h",[0x24]="j",[0x25]="k",[0x26]="l",[0x27]="\xc3\xb2",[0x28]="\xc3\xa0",[0x29]="\\",
    [0x2b]="\xc3\xb9",[0x2c]="z",[0x2d]="x",[0x2e]="c",[0x2f]="v",[0x30]="b",[0x31]="n",[0x32]="m",[0x33]=",",[0x34]=".",[0x35]="-",
    [0x39]=" ",[0x56]="<" }};
static const struct layer it_shift = {{
    [0x02]="!",[0x03]="\"",[0x04]="\xc2\xa3",[0x05]="$",[0x06]="%",[0x07]="&",[0x08]="/",[0x09]="(",[0x0a]=")",[0x0b]="=",[0x0c]="?",[0x0d]="^",
    [0x10]="Q",[0x11]="W",[0x12]="E",[0x13]="R",[0x14]="T",[0x15]="Y",[0x16]="U",[0x17]="I",[0x18]="O",[0x19]="P",[0x1a]="\xc3\xa9",[0x1b]="*",
    [0x1e]="A",[0x1f]="S",[0x20]="D",[0x21]="F",[0x22]="G",[0x23]="H",[0x24]="J",[0x25]="K",[0x26]="L",[0x27]="\xc3\xa7",[0x28]="\xc2\xb0",[0x29]="|",
    [0x2b]="\xc2\xa7",[0x2c]="Z",[0x2d]="X",[0x2e]="C",[0x2f]="V",[0x30]="B",[0x31]="N",[0x32]="M",[0x33]=";",[0x34]=":",[0x35]="_",
    [0x39]=" ",[0x56]=">" }};
static const struct layer it_altgr = {{ [0x12]="\xe2\x82\xac",[0x1a]="[",[0x1b]="]",[0x27]="@",[0x28]="#",[0x39]=" " }};
static const struct layer it_altgr_shift = {{ [0x1a]="{",[0x1b]="}",[0x39]=" " }};
static const struct layer *const layers[DU_LAYOUT_COUNT][4] = {
    { &us_plain, &us_shift, &us_altgr, &us_altgr },
    { &it_plain, &it_shift, &it_altgr, &it_altgr_shift },
};

static uint32_t decode(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    if (!u || !u[0]) return 0;
    if (u[0] < 0x80) return u[0];
    if ((u[0] & 0xe0) == 0xc0) return (uint32_t)(u[0] & 0x1f) << 6 | (u[1] & 0x3f);
    if ((u[0] & 0xf0) == 0xe0) return (uint32_t)(u[0] & 0x0f) << 12 | (uint32_t)(u[1] & 0x3f) << 6 | (u[2] & 0x3f);
    return (uint32_t)(u[0] & 0x07) << 18 | (uint32_t)(u[1] & 0x3f) << 12 | (uint32_t)(u[2] & 0x3f) << 6 | (u[3] & 0x3f);
}
uint32_t du_keymap(enum du_layout layout, unsigned code, unsigned state)
{
    if ((unsigned)layout >= DU_LAYOUT_COUNT || code >= KEYS) return 0;
    int shift = (state & DU_KEYMAP_SHIFT) != 0, altgr = (state & DU_KEYMAP_ALTGR) != 0;
    /* Caps Lock inverts Shift for the letter keys a-z only (Windows behaviour for these layouts). */
    uint32_t base = decode(layers[layout][0]->k[code]);
    if ((state & DU_KEYMAP_CAPS) && !altgr && base >= 'a' && base <= 'z') shift = !shift;
    return decode(layers[layout][(altgr ? 2 : 0) + shift]->k[code]);
}
int du_layout_find(const char *code)
{
    for (int i = 0; i < DU_LAYOUT_COUNT; i++) if (code && !strcasecmp(code, du_layouts[i].code)) return i;
    return -1;
}
size_t du_utf8_encode(uint32_t cp, char out[5])
{
    size_t n = 0;
    if (cp < 0x80) out[n++] = (char)cp;
    else if (cp < 0x800) { out[n++] = (char)(0xc0 | cp >> 6); out[n++] = (char)(0x80 | (cp & 0x3f)); }
    else if (cp < 0x10000) {
        if (cp >= 0xd800 && cp <= 0xdfff) { out[0] = 0; return 0; }
        out[n++] = (char)(0xe0 | cp >> 12); out[n++] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[n++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp <= 0x10ffff) {
        out[n++] = (char)(0xf0 | cp >> 18); out[n++] = (char)(0x80 | ((cp >> 12) & 0x3f));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[n++] = (char)(0x80 | (cp & 0x3f));
    } else { out[0] = 0; return 0; }
    out[n] = 0;
    return n;
}
