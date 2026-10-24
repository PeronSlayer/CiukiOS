/* SPDX-License-Identifier: MIT
 * Shared scan translation and per-context composition. No external symbols. */
#ifndef CIUKI_INPUT_TEXT_H
#define CIUKI_INPUT_TEXT_H
#include "keymap.h"
/* Index = scan code 0x00..0x39; 0 = no character. */
static const unsigned short us_plain[0x3a] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '};
static const unsigned short us_shift[0x3a] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '};
static const unsigned short it_plain[0x3a] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\'', 0x00ec, 0, 0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 0x00e8, '+', 0, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', 0x00f2, 0x00e0, '\\', 0, 0x00f9, 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '};
static const unsigned short it_shift[0x3a] = {
    0, 0, '!', '"', 0x00a3, '$', '%', '&', '/', '(', ')', '=', '?', '^', 0, 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0x00e9, '*', 0, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', 0x00e7, 0x00b0, '|', 0, 0x00a7, 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '};

static inline unsigned ciuki_input_char(int layout, unsigned code, unsigned mods)
{
    int shift = (mods & CIUKI_KEYMAP_SHIFT) != 0;
    if (code == 0x56)   /* the ISO key between left Shift and Z */
        return layout == 1 ? (shift ? '>' : '<') : (shift ? '|' : '\\');
    if (code >= 0x3a) return 0;
    if (layout == 1 && (mods & CIUKI_KEYMAP_ALTGR)) {
        switch (code) {
        case 0x12: return 0x20ac;                       /* AltGr+E: euro sign */
        case 0x1a: return shift ? '{' : '[';            /* è */
        case 0x1b: return shift ? '}' : ']';            /* + */
        case 0x27: return '@';                          /* ò */
        case 0x28: return '#';                          /* à */
        case 0x29: return 0x00ac;                       /* \ key: not sign */
        default: return 0;
        }
    }
    if (mods & CIUKI_KEYMAP_ALTGR) return 0;
    unsigned ch = layout == 1 ? (shift ? it_shift[code] : it_plain[code]) : (shift ? us_shift[code] : us_plain[code]);
    if (mods & CIUKI_KEYMAP_CAPS) {
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        else if (ch >= 'A' && ch <= 'Z') ch += 32;
    }
    return ch;
}

/* Returns 0 for a pending dead key, 1 for a character, 2 for fallback.
 * Only the caller's context is modified. Existing Italian vowels stay direct. */
static inline unsigned ciuki_input_compose(unsigned *pending, unsigned ch, int dead, unsigned out[2])
{
    unsigned accent=*pending;
    if(!ch)return 0;
    *pending=0;
    if(accent) {
        if(ch==' ' || ch==accent) {out[0]=accent;return 1;}
        static const unsigned plain[]={'a','e','i','o','u','A','E','I','O','U'};
        static const unsigned circumflex[]={0xe2,0xea,0xee,0xf4,0xfb,0xc2,0xca,0xce,0xd4,0xdb};
        for(unsigned k=0;k<10;k++)if(ch==plain[k]) {out[0]=circumflex[k];return 1;}
        out[0]=accent;out[1]=ch;return 2;
    }
    if(dead) {*pending=ch;return 0;}
    out[0]=ch;return 1;
}
static inline unsigned ciuki_input_utf8(unsigned cp, char out[5])
{
    if(cp<128){out[0]=(char)cp;out[1]=0;return 1;}
    if(cp<2048){out[0]=(char)(0xc0|(cp>>6));out[1]=(char)(0x80|(cp&63));out[2]=0;return 2;}
    out[0]=(char)(0xe0|(cp>>12));out[1]=(char)(0x80|((cp>>6)&63));out[2]=(char)(0x80|(cp&63));out[3]=0;return 3;
}
#endif
