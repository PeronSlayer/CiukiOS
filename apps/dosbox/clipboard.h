/* G17 toolbar clipboard; provider is the shared native toolkit clipboard.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSBOX_CLIPBOARD_H
#define CIUKI_DOSBOX_CLIPBOARD_H
#include "window.h"
#include <ciuki/dosvm.h>
static inline void db_command(struct ciuki_dosvm_command *c, uint32_t id, uint32_t generation)
{
    memset(c,0,sizeof(*c)); c->version=CIUKI_DOSVM_VERSION; c->size=sizeof(*c);
    c->id=id; c->generation=generation;
}
static inline int db_policy(const struct dosbox_window *w, uint32_t id, uint32_t generation)
{
    struct ciuki_dosvm_command c; db_command(&c,id,generation);
    c.policy.marking=(unsigned)w->marking; c.policy.idle_sensitivity=w->profile.idle;
    return ciuki_dosvm(CIUKI_DOSVM_POLICY,&c);
}
static inline void db_end_mark(struct dosbox_window *w, uint32_t id, uint32_t generation)
{
    w->marking=w->selecting=w->selected=0; w->dirty=1;
    (void)db_policy(w,id,generation);
}
static inline int db_mark(struct dosbox_window *w, uint32_t id, uint32_t generation)
{
    struct ciuki_dosvm_command c; db_command(&c,id,generation);
    w->marking=1;
    if (db_policy(w,id,generation)) { w->marking=0; return -1; }
    if (ciuki_dosvm(CIUKI_DOSVM_TEXT_CHUNK,&c) || !c.text_chunk.columns || c.text_chunk.columns>160 ||
        !c.text_chunk.rows || c.text_chunk.rows>100) {
        db_end_mark(w,id,generation); db_notice(w,"Mark is available in DOS text modes."); return -1;
    }
    w->columns=c.text_chunk.columns; w->rows=c.text_chunk.rows;
    db_select(w); w->x0=w->x1; w->y0=w->y1; w->selecting=1;
    db_notice(w,"Mark text with the mouse or Shift+arrows. Enter copies; Esc cancels.");
    return 0;
}
/* OEM CP437 upper half, converted to UTF-8 for the native clipboard. */
static const uint16_t db_cp437[128]={
    0x00c7,0x00fc,0x00e9,0x00e2,0x00e4,0x00e0,0x00e5,0x00e7,0x00ea,0x00eb,0x00e8,0x00ef,0x00ee,0x00ec,0x00c4,0x00c5,
    0x00c9,0x00e6,0x00c6,0x00f4,0x00f6,0x00f2,0x00fb,0x00f9,0x00ff,0x00d6,0x00dc,0x00a2,0x00a3,0x00a5,0x20a7,0x0192,
    0x00e1,0x00ed,0x00f3,0x00fa,0x00f1,0x00d1,0x00aa,0x00ba,0x00bf,0x2310,0x00ac,0x00bd,0x00bc,0x00a1,0x00ab,0x00bb,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255d,0x255c,0x255b,0x2510,
    0x2514,0x2534,0x252c,0x251c,0x2500,0x253c,0x255e,0x255f,0x255a,0x2554,0x2569,0x2566,0x2560,0x2550,0x256c,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256b,0x256a,0x2518,0x250c,0x2588,0x2584,0x258c,0x2590,0x2580,
    0x03b1,0x00df,0x0393,0x03c0,0x03a3,0x03c3,0x00b5,0x03c4,0x03a6,0x0398,0x03a9,0x03b4,0x221e,0x03c6,0x03b5,0x2229,
    0x2261,0x00b1,0x2265,0x2264,0x2320,0x2321,0x00f7,0x2248,0x00b0,0x2219,0x00b7,0x221a,0x207f,0x00b2,0x25a0,0x00a0
};
static inline unsigned db_utf8(unsigned char ch, char *out)
{
    unsigned code=ch<128 ? (ch<32 || ch==127 ? ' ' : ch) : db_cp437[ch-128];
    if (code<128) { out[0]=(char)code; return 1; }
    if (code<2048) { out[0]=(char)(0xC0|(code>>6)); out[1]=(char)(0x80|(code&63)); return 2; }
    out[0]=(char)(0xE0|(code>>12)); out[1]=(char)(0x80|((code>>6)&63)); out[2]=(char)(0x80|(code&63)); return 3;
}
static inline int db_copy(struct dosbox_window *w, uint32_t id, uint32_t generation)
{
    if (!w->marking || !w->selected) { db_notice(w,"Select text with Mark first."); return -1; }
    int left=w->x0<w->x1 ? w->x0 : w->x1, right=w->x0>w->x1 ? w->x0 : w->x1;
    int top=w->y0<w->y1 ? w->y0 : w->y1, bottom=w->y0>w->y1 ? w->y0 : w->y1;
    size_t capacity=(size_t)(right-left+1)*(size_t)(bottom-top+1)*3u+(size_t)(bottom-top+1)*2u+1u;
    char *text=malloc(capacity); if (!text) { db_notice(w,"Not enough memory to copy text."); return -1; }
    size_t at=0;
    for (int row=top;row<=bottom;row++) {
        size_t start=at;
        for (int col=left;col<=right;) {
            struct ciuki_dosvm_command c; db_command(&c,id,generation);
            c.text_chunk.row=(unsigned)row; c.text_chunk.column=(unsigned)col;
            if (ciuki_dosvm(CIUKI_DOSVM_TEXT_CHUNK,&c) || c.text_chunk.columns!=w->columns || c.text_chunk.rows!=w->rows) {
                free(text); db_end_mark(w,id,generation); db_notice(w,"The DOS text mode changed. Select the text again."); return -1;
            }
            for (unsigned i=0;i<CIUKI_DOSVM_TEXT_BYTES && col<=right;i++,col++) at+=db_utf8(c.text_chunk.characters[i],text+at);
        }
        while (at>start && text[at-1]==' ') at--;
        if (row<bottom) { text[at++]='\r'; text[at++]='\n'; }
    }
    text[at]=0;
    int error=du_clipboard_set_n(text,at); free(text);
    if (error) { db_notice(w,"Cannot write to the clipboard."); return -1; }
    db_end_mark(w,id,generation); db_notice(w,"Text copied to the clipboard."); return 0;
}
/* Validate the entire text before any guest input. CR/LF is one Enter;
 * lone CR/LF and tabs are allowed. There is deliberately no command LAUNCH
 * or prompt wait for paste: it types into the currently running program. */
static inline int db_paste_keys(const char *text, struct dosbox_key *keys, unsigned capacity)
{
    if (!text) return -1;
    unsigned at=0;
    for (size_t i=0;text[i];i++) {
        int shift=0; unsigned code;
        if (text[i]=='\r' || text[i]=='\n') { code=CIUKI_KEY_ENTER; if (text[i]=='\r' && text[i+1]=='\n') i++; }
        else if (text[i]=='\t') code=CIUKI_KEY_TAB;
        else code=dosbox_key_for(text[i],&shift);
        if (!code || at+4>capacity) return -1;
        uint16_t group=(uint16_t)at, main=(uint16_t)(at+!!shift);
        if (shift) keys[at++]=(struct dosbox_key){CIUKI_KEY_LEFTSHIFT,1,0,group,main};
        keys[at++]=(struct dosbox_key){(uint16_t)code,1,0,group,main};
        keys[at++]=(struct dosbox_key){(uint16_t)code,0,0,group,main};
        if (shift) keys[at++]=(struct dosbox_key){CIUKI_KEY_LEFTSHIFT,0,0,group,main};
    }
    return (int)at;
}
#endif
