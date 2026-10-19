/* Private path conversion; no locale or host-library dependency.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/ldr.h>
#include "../kernel32/codepages.inc"
WCHAR w32_acp_character(uint8_t c) { return c<128 ? c : cp1252[c-128]; }
int w32_utf16_path(const WCHAR *s, ULONG n, char *out, ULONG cap) {
    ULONG at=0;
    for (ULONG i=0;i<n;i++) {
        ULONG c=s[i],size=c<128 ? 1 : c<2048 ? 2 : 3;
        if (!c || (c>=0xd800 && c<=0xdfff) || at>=cap || size>=cap-at) return 0;
        if (size==3) out[at++]=(char)(0xe0|(c>>12));
        if (size>=2) out[at++]=(char)((size==2 ? 0xc0 : 0x80)|((c>>6)&(size==2 ? 31 : 63)));
        out[at++]=(char)(size==1 ? c : 0x80|(c&63));
    }
    if (at>=cap) return 0;
    out[at]=0;
    return 1;
}
int w32_utf8_path(const char *s, WCHAR *out, ULONG cap) {
    ULONG at=0;
    while (*s) {
        ULONG c=(uint8_t)*s++,extra=0,min=0;
        if (c>=0xc2 && c<=0xdf) { c&=31;extra=1;min=128; }
        else if (c>=0xe0 && c<=0xef) { c&=15;extra=2;min=2048; }
        else if (c>=128) return 0;
        while (extra--) {
            ULONG b=(uint8_t)*s; if ((b&0xc0)!=0x80) return 0;
            s++;c=(c<<6)|(b&63);
        }
        if (c<min || (c>=0xd800 && c<=0xdfff) || at+1>=cap) return 0;
        out[at++]=(WCHAR)c;
    }
    if (at>=cap) return 0;
    out[at]=0;
    return 1;
}
