/* SPDX-License-Identifier: MIT
 * Word 6, 95 and 97+ .doc import for WordPad (G28b): the text of the main document with its character and paragraph
 * formatting. See docs/design/wordpad.md for the sources.
 *
 * Sources: Microsoft, "Word 6.0 Binary File Format" (FIB, FKP, PAPX/CHPX, STSH, PIC, sprm table with 1-byte opcodes) and
 * "Microsoft Office Word 97-2007 Binary File Format (.doc) Specification" (FIB with the table stream, Clx and piece table,
 * Unicode and compressed text, FKP, PapxInFkp, Stshi/STD, 2-byte sprm opcodes and the spra operand sizes, PICF and the
 * OfficeArt BLIP records, lists: LSTF, LVLF, LFO), over the compound file reader of ole.c ([MS-CFB]).
 *
 * Supported: main text (footnotes, headers and the other subdocuments are counted, not shown); piece tables including the
 * grpprls of fast-saved files; character formatting (font, size, bold, italic, underline, strike, colour, caps, hidden
 * text), paragraph formatting (alignment, indents, tab stops), the style sheet (based-on chains, character styles), bullets
 * and numbers (Word 97 lists, Word 6 ANLD bullets), tables (cells become tab-separated, rows paragraphs), fields (the
 * result is shown), inline pictures (PNG, JPEG and DIB BLIPs, Word 6 metafiles). Not supported: encrypted files, files
 * before Word 6, single-sprm piece modifiers (prm variant 1) of fast-saved files, floating drawings, compressed
 * metafile BLIPs. */
#include "rich.h"
#include "ole.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct span { const uint8_t *p; size_t n; };
static unsigned g8(struct span s, size_t o) { return o < s.n ? s.p[o] : 0u; }
static unsigned g16(struct span s, size_t o) { return o + 2 <= s.n ? (unsigned)s.p[o] | (unsigned)s.p[o + 1] << 8 : 0u; }
static uint32_t g32(struct span s, size_t o) { return o + 4 <= s.n ? (uint32_t)s.p[o] | (uint32_t)s.p[o + 1] << 8 | (uint32_t)s.p[o + 2] << 16 | (uint32_t)s.p[o + 3] << 24 : 0u; }
static int s16(struct span s, size_t o) { unsigned v = g16(s, o); return (int)(v & 0x8000 ? (int)v - 65536 : (int)v); }
static int within(struct span s, size_t o, size_t n) { return o <= s.n && n <= s.n - o; }
static struct span sub(struct span s, size_t o, size_t n) { return within(s, o, n) ? (struct span){s.p + o, n} : (struct span){NULL, 0}; }

/* ---- properties ------------------------------------------------------------------------------ */

#define MAX_TABS 32
struct wchp {
    uint8_t bold, italic, strike, kul, ico, vanish, caps, smallcaps, fspec;
    uint16_t ftc, hps;
    int32_t cv;             /* 0xRRGGBB, or -1 when the palette colour (ico) applies */
    int32_t fcpic;          /* offset of the picture, or -1 */
};
struct wpap {
    uint16_t istd; uint8_t jc, intable, ttp, ilvl, bullet, pad; uint16_t ilfo;
    int dleft, dleft1, dright;
    unsigned ntabs; int tab[MAX_TABS]; uint8_t tbd[MAX_TABS];
};
enum {
    S_NONE, S_PISTD, S_PJC, S_PINTABLE, S_PTTP, S_PDXARIGHT, S_PDXALEFT, S_PNEST, S_PDXALEFT1, S_PCHGTABSPAPX, S_PCHGTABS, S_PILFO, S_PILVL, S_PANLD,
    S_CBOLD, S_CITALIC, S_CSTRIKE, S_CKUL, S_CHPS, S_CICO, S_CFTC, S_CFSPEC, S_CPICLOC, S_CVANISH, S_CCAPS, S_CSMALLCAPS, S_CISTD, S_CSIZEPOS, S_CCV,
    S_CPLAIN
};

/* Canonical sprm for an opcode: Word 6 has 1-byte opcodes, Word 97 two bytes (the *80 forms of Word 97 and the newer ones). */
static int canon(int w97, unsigned op)
{
    if (!w97) switch (op) {
        case 2: return S_PISTD; case 5: return S_PJC; case 24: return S_PINTABLE; case 25: return S_PTTP; case 16: return S_PDXARIGHT;
        case 17: return S_PDXALEFT; case 18: return S_PNEST; case 19: return S_PDXALEFT1; case 15: return S_PCHGTABSPAPX; case 23: return S_PCHGTABS;
        case 12: return S_PANLD; case 85: return S_CBOLD; case 86: return S_CITALIC; case 87: return S_CSTRIKE; case 94: return S_CKUL;
        case 99: return S_CHPS; case 98: return S_CICO; case 93: return S_CFTC; case 117: return S_CFSPEC; case 68: return S_CPICLOC;
        case 92: return S_CVANISH; case 91: return S_CCAPS; case 90: return S_CSMALLCAPS; case 80: return S_CISTD; case 95: return S_CSIZEPOS;
        case 83: case 82: return S_CPLAIN;
        default: return S_NONE;
    }
    switch (op) {
    case 0x4600: return S_PISTD; case 0x2461: case 0x2403: return S_PJC; case 0x2416: case 0x244b: return S_PINTABLE; case 0x2417: case 0x244c: return S_PTTP;
    case 0x845d: case 0x840e: return S_PDXARIGHT; case 0x845e: case 0x840f: return S_PDXALEFT; case 0x465f: case 0x4610: return S_PNEST;
    case 0x8460: case 0x8411: return S_PDXALEFT1; case 0xc60d: return S_PCHGTABSPAPX; case 0xc615: return S_PCHGTABS;
    case 0x460b: return S_PILFO; case 0x260a: return S_PILVL;
    case 0x0835: return S_CBOLD; case 0x0836: return S_CITALIC; case 0x0837: case 0x2a53: return S_CSTRIKE; case 0x2a3e: return S_CKUL;
    case 0x4a43: return S_CHPS; case 0x2a42: return S_CICO; case 0x4a4f: return S_CFTC; case 0x0855: return S_CFSPEC; case 0x6a03: return S_CPICLOC;
    case 0x083c: return S_CVANISH; case 0x083b: return S_CCAPS; case 0x083a: return S_CSMALLCAPS; case 0x4a30: return S_CISTD; case 0xea3f: return S_CSIZEPOS;
    case 0x6870: return S_CCV; case 0x2a33: case 0x2a32: return S_CPLAIN;
    default: return S_NONE;
    }
}
/* Operand size of a Word 6 sprm: >0 fixed, 0 none, -1 a count byte follows, -2 a 2-byte count follows, -3 unknown. */
static int w6_size(unsigned op)
{
    static const signed char t[209] = {
        [2] = 2, [3] = -1, [4] = 1, [5] = 1, [6] = 1, [7] = 1, [8] = 1, [9] = 1, [10] = 1, [11] = 1, [12] = -1, [13] = 1, [14] = 1, [15] = -1, [16] = 2,
        [17] = 2, [18] = 2, [19] = 2, [20] = 4, [21] = 2, [22] = 2, [23] = -1, [24] = 1, [25] = 1, [26] = 2, [27] = 2, [28] = 2, [29] = 1,
        [30] = 2, [31] = 2, [32] = 2, [33] = 2, [34] = 2, [35] = 2, [36] = 2, [37] = 1, [38] = 2, [39] = 2, [40] = 2, [41] = 2, [42] = 2, [43] = 2,
        [44] = 1, [45] = 2, [46] = 2, [47] = 2, [48] = 2, [49] = 2, [50] = 1, [51] = 1, [52] = -1,
        [65] = 1, [66] = 1, [67] = 1, [68] = -1, [69] = 2, [70] = 4, [71] = 1, [72] = 2, [73] = 3, [74] = -1, [75] = 1,
        [80] = 2, [81] = -1, [82] = -1, [83] = 0, [85] = 1, [86] = 1, [87] = 1, [88] = 1, [89] = 1, [90] = 1, [91] = 1, [92] = 1, [93] = 2, [94] = 1,
        [95] = 3, [96] = 2, [97] = 2, [98] = 1, [99] = 1, [100] = 1, [101] = 1, [102] = 1, [103] = -1, [104] = 1, [105] = -1, [106] = -1, [107] = 2,
        [108] = -1, [109] = 2, [110] = 2, [117] = 1, [118] = 1, [119] = 1, [120] = -1, [121] = 2, [122] = 2, [123] = 2, [124] = 2,
        [131] = 1, [132] = 1, [133] = -1, [136] = 3, [137] = 3, [138] = 1, [139] = 1, [140] = 2, [141] = 2, [142] = 1, [143] = 1, [144] = 2, [145] = 2,
        [146] = 1, [147] = 1, [148] = 2, [149] = 2, [150] = 1, [151] = 1, [152] = 1, [153] = 1, [154] = 2, [155] = 2, [156] = 2, [157] = 2, [158] = 1,
        [159] = 1, [160] = 2, [161] = 2, [162] = 1, [163] = 1, [164] = 2, [165] = 2, [166] = 2, [167] = 2, [168] = 2, [169] = 2, [170] = 2, [171] = 2,
        [182] = 2, [183] = 2, [184] = 2, [185] = 1, [186] = 1, [187] = 12, [188] = -2, [189] = 2, [190] = -2, [191] = -1, [192] = 4, [193] = 5,
        [194] = 4, [195] = 2, [196] = 4, [197] = 2, [198] = 2, [199] = 5, [200] = 4};
    if (op >= sizeof(t)) return -3;
    if (!t[op] && op != 83) return -3;
    return t[op];
}

struct sprm { unsigned op; struct span arg; };
/* The next sprm of a grpprl: 1 and updates *at, or 0 at the end, at a zero padding opcode or at damage. */
static int sprm_next(int w97, struct span g, size_t *at, struct sprm *out)
{
    size_t o = *at;
    if (o >= g.n) return 0;
    if (!w97) {
        unsigned op = g8(g, o);
        if (!op) return 0;
        int sz = w6_size(op);
        size_t len;
        if (sz == -3) return 0;
        if (sz == -1) {
            unsigned cch = g8(g, o + 1);
            if (cch == 255 && (op == 23 || op == 15)) {   /* sprmPChgTabs: the length is computed */
                unsigned d = g8(g, o + 2), a = g8(g, o + 2 + 1 + 4 * (size_t)d);
                len = 2 + 1 + 4 * (size_t)d + 1 + 3 * (size_t)a;
                if (op == 15) len = 2 + 1 + 2 * (size_t)d + 1 + 3 * (size_t)a;
            } else len = 2 + (size_t)cch;
            if (!within(g, o, len)) return 0;
            *out = (struct sprm){op, {g.p + o + 2, len - 2}};
        } else if (sz == -2) {
            unsigned cb = g16(g, o + 1);
            len = 1 + (size_t)cb + 1;
            if (!within(g, o, len)) return 0;
            *out = (struct sprm){op, {g.p + o + 1, len - 1}};
        } else {
            len = 1 + (size_t)sz;
            if (!within(g, o, len)) return 0;
            *out = (struct sprm){op, {g.p + o + 1, (size_t)sz}};
        }
        *at = o + len;
        return 1;
    }
    if (o + 2 > g.n) return 0;
    unsigned op = g16(g, o);
    if (!op) return 0;
    static const unsigned char spra[8] = {1, 1, 2, 4, 2, 2, 0, 3};
    unsigned sp = op >> 13;
    size_t len;
    if (sp == 6) {
        if (op == 0xd608 || op == 0xd606) { len = 2 + (size_t)g16(g, o + 2) + 1; if (!within(g, o, len)) return 0; *out = (struct sprm){op, {g.p + o + 2, len - 2}}; }
        else {
            size_t cch = g8(g, o + 2);
            if (cch == 255 && op == 0xc615) {
                unsigned d = g8(g, o + 3), a = g8(g, o + 3 + 1 + 4 * (size_t)d);
                cch = 1 + 4 * (size_t)d + 1 + 3 * (size_t)a;
            }
            len = 3 + cch;
            if (!within(g, o, len)) return 0;
            *out = (struct sprm){op, {g.p + o + 3, len - 3}};
        }
    } else {
        len = 2 + spra[sp];
        if (!within(g, o, len)) return 0;
        *out = (struct sprm){op, {g.p + o + 2, spra[sp]}};
    }
    *at = o + len;
    return 1;
}

static void tabs_apply(struct wpap *p, struct span a, int w97, int piece_level)
{
    size_t o = 0;
    unsigned nd = g8(a, o++), i;
    int del[MAX_TABS], close[MAX_TABS];
    unsigned ndel = nd < MAX_TABS ? nd : MAX_TABS;
    for (i = 0; i < nd; i++) { int v = s16(a, o); o += 2; if (i < ndel) del[i] = v; }
    for (i = 0; piece_level && i < nd; i++) { int v = s16(a, o); o += 2; if (i < ndel) close[i] = v; }
    (void)w97;
    for (i = 0; i < ndel; i++) {
        for (unsigned k = 0; k < p->ntabs; k++) {
            int tol = piece_level ? close[i] : 0;
            if (p->tab[k] >= del[i] - tol && p->tab[k] <= del[i] + tol) {
                memmove(p->tab + k, p->tab + k + 1, (p->ntabs - k - 1) * sizeof(*p->tab));
                memmove(p->tbd + k, p->tbd + k + 1, (p->ntabs - k - 1) * sizeof(*p->tbd));
                p->ntabs--; k--;
            }
        }
    }
    unsigned na = g8(a, o++);
    int add[MAX_TABS]; unsigned nadd = na < MAX_TABS ? na : MAX_TABS;
    for (i = 0; i < na; i++) { int v = s16(a, o); o += 2; if (i < nadd) add[i] = v; }
    for (i = 0; i < na; i++) {
        unsigned tbd = g8(a, o++);
        if (i >= nadd || p->ntabs >= MAX_TABS) continue;
        unsigned at = 0;
        while (at < p->ntabs && p->tab[at] < add[i]) at++;
        if (at < p->ntabs && p->tab[at] == add[i]) { p->tbd[at] = (uint8_t)tbd; continue; }
        memmove(p->tab + at + 1, p->tab + at, (p->ntabs - at) * sizeof(*p->tab));
        memmove(p->tbd + at + 1, p->tbd + at, (p->ntabs - at) * sizeof(*p->tbd));
        p->tab[at] = add[i]; p->tbd[at] = (uint8_t)tbd; p->ntabs++;
    }
}

/* ---- reader state --------------------------------------------------------------------------------- */

struct piece { uint32_t cp0, cp1, fc; uint8_t unit; uint16_t prm; };
struct wstyle {
    uint8_t valid, sgc, state; uint16_t base;
    struct span papx, chpx; uint16_t papx_istd;
    struct wpap pap; struct wchp chp;
};
struct wfont { char name[40]; uint8_t family; int8_t map; };
struct wlvl { uint8_t nfc, follow, ok; const uint8_t *papx; unsigned npapx; uint32_t start; struct span xst; };
struct wlist { uint32_t lsid; unsigned nlvl; struct wlvl lvl[9]; uint32_t count[9]; };
struct bte { uint32_t n; struct span fc, pn; unsigned pnsz; };
struct wd {
    struct wp_doc *doc; struct wp_report *rep;
    struct span main, tbl, data;
    int w97, mac;
    uint32_t ccp;
    struct piece *pieces; unsigned npieces;
    struct span *grp; unsigned ngrp;
    struct bte chpx, papx;
    struct wstyle *styles; unsigned nstyles; uint16_t default_ftc;
    struct wfont *fonts; unsigned nfonts;
    struct wlist *lists; unsigned nlists;
    struct span lfo; uint32_t nlfo;
    uint16_t *ch;
    char *buf; size_t bufcap;
    unsigned depth;             /* nesting of grpprls applied from inside a grpprl (character styles, list levels): bounded */
    int err;
};

static void chp_null(const struct wd *w, struct wchp *c)
{
    memset(c, 0, sizeof(*c));
    c->hps = 20; c->ftc = w->default_ftc; c->cv = -1; c->fcpic = -1;
}

static void apply_chp(struct wd *w, struct wchp *c, const struct wchp *ref, struct span g);
static void style_compute(struct wd *w, unsigned istd, unsigned depth);

static const uint8_t ico_rgb[17][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 255}, {0, 255, 255}, {0, 255, 0}, {255, 0, 255}, {255, 0, 0}, {255, 255, 0},
    {255, 255, 255}, {0, 0, 128}, {0, 128, 128}, {0, 128, 0}, {128, 0, 128}, {128, 0, 0}, {128, 128, 0}, {128, 128, 128}, {192, 192, 192}};

static unsigned toggled(unsigned v, unsigned ref)
{
    if (v == 0x80) return ref;
    if (v == 0x81) return !ref;
    return v & 1;
}
/* Applies a character grpprl to c; ref is the property set the toggles 128 and 129 refer to. */
static void apply_chp(struct wd *w, struct wchp *c, const struct wchp *ref, struct span g)
{
    size_t at = 0; struct sprm s;
    while (sprm_next(w->w97, g, &at, &s)) {
        switch (canon(w->w97, s.op)) {
        case S_CBOLD: c->bold = (uint8_t)toggled(g8(s.arg, 0), ref->bold); break;
        case S_CITALIC: c->italic = (uint8_t)toggled(g8(s.arg, 0), ref->italic); break;
        case S_CSTRIKE: c->strike = (uint8_t)toggled(g8(s.arg, 0), ref->strike); break;
        case S_CVANISH: c->vanish = (uint8_t)toggled(g8(s.arg, 0), ref->vanish); break;
        case S_CCAPS: c->caps = (uint8_t)toggled(g8(s.arg, 0), ref->caps); break;
        case S_CSMALLCAPS: c->smallcaps = (uint8_t)toggled(g8(s.arg, 0), ref->smallcaps); break;
        case S_CFSPEC: c->fspec = (uint8_t)(g8(s.arg, 0) & 1); break;
        case S_CKUL: c->kul = (uint8_t)g8(s.arg, 0); break;
        case S_CICO: { unsigned v = g8(s.arg, 0); c->ico = (uint8_t)(v <= 16 ? v : 0); c->cv = -1; break; }
        case S_CHPS: { unsigned v = w->w97 ? g16(s.arg, 0) : g8(s.arg, 0); if (v) c->hps = (uint16_t)v; break; }
        case S_CSIZEPOS: { unsigned v = g8(s.arg, 0); if (v) c->hps = (uint16_t)v; break; }
        case S_CFTC: c->ftc = (uint16_t)g16(s.arg, 0); break;
        case S_CPICLOC: c->fcpic = (int32_t)g32(s.arg, 0); c->fspec = 1; break;
        case S_CCV: { uint32_t v = g32(s.arg, 0); c->cv = v == 0xff000000u ? -1 : (int32_t)((v & 255) << 16 | (v >> 8 & 255) << 8 | (v >> 16 & 255)); break; }
        case S_CPLAIN: { int32_t fc = c->fcpic; uint8_t fs = c->fspec; *c = *ref; c->fcpic = fc; c->fspec = fs; break; }
        case S_CISTD: {   /* a character style: its own property exceptions first (the based-on chain, root first) */
            unsigned id = g16(s.arg, 0);
            unsigned chain[16], n = 0;
            while (id < w->nstyles && w->styles[id].valid && w->styles[id].sgc == 2 && n < 16) { chain[n++] = id; id = w->styles[id].base; }
            if (w->depth >= 3) break;   /* a style that names a style that names...: a damaged file */
            w->depth++;
            while (n) apply_chp(w, c, ref, w->styles[chain[--n]].chpx);
            w->depth--;
            break;
        }
        default: break;
        }
    }
}
static void apply_pap(struct wd *w, struct wpap *p, struct span g)
{
    size_t at = 0; struct sprm s;
    while (sprm_next(w->w97, g, &at, &s)) {
        switch (canon(w->w97, s.op)) {
        case S_PISTD: p->istd = (uint16_t)g16(s.arg, 0); break;
        case S_PJC: p->jc = (uint8_t)g8(s.arg, 0); break;
        case S_PINTABLE: p->intable = (uint8_t)(g8(s.arg, 0) & 1); break;
        case S_PTTP: p->ttp = (uint8_t)(g8(s.arg, 0) & 1); break;
        case S_PDXARIGHT: p->dright = s16(s.arg, 0); break;
        case S_PDXALEFT: p->dleft = s16(s.arg, 0); break;
        case S_PNEST: p->dleft += s16(s.arg, 0); if (p->dleft < 0) p->dleft = 0; break;
        case S_PDXALEFT1: p->dleft1 = s16(s.arg, 0); break;
        case S_PCHGTABSPAPX: tabs_apply(p, s.arg, w->w97, 0); break;
        case S_PCHGTABS: tabs_apply(p, s.arg, w->w97, 1); break;
        case S_PANLD: p->bullet = g8(s.arg, 0) == 23; if (p->bullet && !p->dleft && !p->dleft1) { p->dleft = 360; p->dleft1 = -360; } break;
        case S_PILFO: {
            p->ilfo = (uint16_t)g16(s.arg, 0);
            if (!p->ilfo) { p->bullet = 0; break; }
            /* the list level's own paragraph properties (indents) apply before the paragraph's later sprms */
            if (p->ilfo <= w->nlfo) {
                uint32_t lsid = g32(w->lfo, 4 + 16 * (size_t)(p->ilfo - 1));
                for (unsigned i = 0; i < w->nlists; i++) if (w->lists[i].lsid == lsid) {
                    const struct wlvl *l = &w->lists[i].lvl[p->ilvl < w->lists[i].nlvl ? p->ilvl : 0];
                    if (l->ok && w->depth < 3) { w->depth++; apply_pap(w, p, (struct span){l->papx, l->npapx}); w->depth--; p->bullet = l->nfc == 23; }
                    break;
                }
            }
            break;
        }
        case S_PILVL: p->ilvl = (uint8_t)(g8(s.arg, 0) & 15); break;
        default: break;
        }
    }
}

static void style_compute(struct wd *w, unsigned istd, unsigned depth)
{
    struct wstyle *st = &w->styles[istd];
    if (st->state == 2) return;
    if (st->state == 1 || depth > 40) { st->state = 2; chp_null(w, &st->chp); memset(&st->pap, 0, sizeof(st->pap)); return; }
    st->state = 1;
    struct wchp chp; struct wpap pap;
    chp_null(w, &chp); memset(&pap, 0, sizeof(pap));
    if (st->base < w->nstyles && w->styles[st->base].valid) {
        style_compute(w, st->base, depth + 1);
        chp = w->styles[st->base].chp; pap = w->styles[st->base].pap;
    }
    if (st->sgc == 1) { pap.istd = (uint16_t)istd; apply_pap(w, &pap, st->papx); }
    struct wchp ref = chp;
    apply_chp(w, &chp, &ref, st->chpx);
    st->chp = chp; st->pap = pap; st->state = 2;
}

/* ---- the file --------------------------------------------------------------------------------------- */

static int load_styles(struct wd *w, uint32_t fc, uint32_t lcb)
{
    struct span t = w->tbl;
    if (!lcb || !within(t, fc, lcb)) return 0;
    struct span s = sub(t, fc, lcb);
    unsigned cbstshi = g16(s, 0);
    size_t pos = 2 + (size_t)cbstshi;
    unsigned cstd = g16(s, 2), cbbase = g16(s, 4);
    if (cbstshi < 14) return 0;
    if (g16(s, 2 + 12) < 0x8000u) w->default_ftc = (uint16_t)g16(s, 2 + 12);
    if (!cstd || cstd > 4095 || cbbase < 8) return 0;
    w->styles = calloc(cstd, sizeof(*w->styles));
    if (!w->styles) return -ENOMEM;
    w->nstyles = cstd;
    for (unsigned i = 0; i < cstd; i++) {
        if (pos + 2 > s.n) break;
        unsigned cb = g16(s, pos);
        pos += 2;
        if (!cb) continue;
        struct span d = sub(s, pos, cb);
        pos += cb;
        if (!d.p || cb < cbbase) continue;
        struct wstyle *st = &w->styles[i];
        st->sgc = (uint8_t)(g16(d, 2) & 15); st->base = (uint16_t)(g16(d, 2) >> 4);
        unsigned cupx = g16(d, 4) & 15;
        size_t at = cbbase;
        if (w->w97) at += 2 + 2 * ((size_t)g16(d, at) + 1); else at += 2 + g8(d, at);
        if (at & 1) at++;
        for (unsigned u = 0; u < cupx; u++) {
            if (at + 2 > d.n) break;
            unsigned cbu = g16(d, at);
            struct span up = sub(d, at + 2, cbu);
            at += 2 + cbu; if (at & 1) at++;
            if (!up.p && cbu) break;
            if (st->sgc == 1 && u == 0) { st->papx_istd = (uint16_t)g16(up, 0); st->papx = sub(up, 2, up.n >= 2 ? up.n - 2 : 0); }
            else if ((st->sgc == 1 && u == 1) || (st->sgc == 2 && u == 0)) st->chpx = up;
        }
        st->valid = (uint8_t)(st->sgc == 1 || st->sgc == 2);
    }
    return 0;
}

static unsigned family_of(unsigned ff, const char *name)
{
    unsigned f = wp_family_for_name(name);
    if (f != WP_SANS) return f;
    return ff == 1 ? WP_SERIF : ff == 3 ? WP_MONO : WP_SANS;
}
static void load_fonts(struct wd *w, uint32_t fc, uint32_t lcb)
{
    struct span src = w->w97 ? w->tbl : w->main;
    if (!lcb || !within(src, fc, lcb)) return;
    struct span s = sub(src, fc, lcb);
    unsigned cap = w->w97 ? g16(s, 0) : 256;
    if (cap > 2048) cap = 2048;
    w->fonts = calloc(cap ? cap : 1, sizeof(*w->fonts));
    if (!w->fonts) { w->err = -ENOMEM; return; }
    size_t pos = w->w97 ? 4 : 2;
    while (w->nfonts < cap && pos < s.n) {
        unsigned cb = g8(s, pos);
        struct span e = sub(s, pos + 1, cb);
        if (!e.p) break;
        struct wfont *f = &w->fonts[w->nfonts++];
        f->map = -2;
        unsigned ff = (g8(e, 0) >> 4) & 7;
        size_t k = 0;
        if (w->w97) { for (size_t o = 39; o + 1 < e.n && k < sizeof(f->name) - 1; o += 2) { unsigned c = g16(e, o); if (!c) break; f->name[k++] = c < 127 && c >= 32 ? (char)c : '?'; } }
        else { for (size_t o = 5; o < e.n && k < sizeof(f->name) - 1; o++) { unsigned c = g8(e, o); if (!c) break; f->name[k++] = c < 127 && c >= 32 ? (char)c : '?'; } }
        f->name[k] = 0;
        f->family = (uint8_t)family_of(ff, f->name);
        pos += 1 + cb;
    }
}
static int font_index(struct wd *w, unsigned ftc)
{
    if (ftc >= w->nfonts) return 0;
    struct wfont *f = &w->fonts[ftc];
    if (f->map == -2) {
        int i = f->name[0] ? wp_font_add(w->doc, f->name, f->family) : -1;
        f->map = (int8_t)(i >= 0 ? i : (int)f->family);   /* a full table falls back to the family's built-in face */
    }
    return f->map;
}

static void load_lists(struct wd *w, uint32_t fclst, uint32_t lcblst, uint32_t fclfo, uint32_t lcblfo)
{
    struct span t = w->tbl;
    if (lcblfo >= 4 && within(t, fclfo, lcblfo)) {
        w->lfo = sub(t, fclfo, lcblfo);
        w->nlfo = g32(w->lfo, 0);
        if ((uint64_t)w->nlfo * 16 + 4 > w->lfo.n) w->nlfo = (uint32_t)((w->lfo.n - 4) / 16);
    }
    if (lcblst < 2 || !within(t, fclst, lcblst)) return;
    struct span s = sub(t, fclst, lcblst);
    unsigned n = g16(s, 0);
    if (!n || (size_t)n * 28 + 2 > s.n) return;
    w->lists = calloc(n, sizeof(*w->lists));
    if (!w->lists) { w->err = -ENOMEM; return; }
    size_t q = 2 + (size_t)n * 28;
    for (unsigned i = 0; i < n; i++) {
        struct wlist *l = &w->lists[i];
        l->lsid = g32(s, 2 + (size_t)i * 28);
        l->nlvl = (g8(s, 2 + (size_t)i * 28 + 26) & 1) ? 1 : 9;
        for (unsigned v = 0; v < l->nlvl; v++) {
            if (!within(s, q, 28)) { w->nlists = i + 1; return; }
            unsigned cbchpx = g8(s, q + 24), cbpapx = g8(s, q + 25);
            struct wlvl *lv = &l->lvl[v];
            lv->nfc = (uint8_t)g8(s, q + 4); lv->follow = (uint8_t)g8(s, q + 15); lv->start = g32(s, q);
            size_t x = q + 28 + cbpapx + cbchpx;
            unsigned cch = g16(s, x);
            if (!within(s, q + 28, (size_t)cbpapx + cbchpx + 2 + 2 * (size_t)cch)) { w->nlists = i + 1; return; }
            lv->papx = s.p + q + 28; lv->npapx = cbpapx;
            lv->xst = (struct span){s.p + x + 2, 2 * (size_t)cch};
            lv->ok = 1;
            q = x + 2 + 2 * (size_t)cch;
        }
    }
    w->nlists = n;
}

static void number_text(uint32_t v, unsigned nfc, char *out, size_t cap)
{
    char t[16]; size_t k = 0;
    if (nfc == 1 || nfc == 2) {
        static const struct { unsigned v; const char *s; } r[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"}, {50, "L"}, {40, "XL"}, {10, "X"}, {9, "IX"}, {5, "V"}, {4, "IV"}, {1, "I"}};
        out[0] = 0;
        for (unsigned i = 0; i < 13 && v; i++) while (v >= r[i].v && strlen(out) + 4 < cap) { strcat(out, r[i].s); v -= r[i].v; }
        if (nfc == 2) for (char *c = out; *c; c++) *c = (char)(*c + 32);
        return;
    }
    if (nfc == 3 || nfc == 4) {
        if (!v) v = 1;
        t[k++] = (char)((nfc == 3 ? 'A' : 'a') + (v - 1) % 26);
        t[k] = 0;
        memcpy(out, t, k + 1);
        return;
    }
    char d[12]; unsigned n = 0;
    if (!v) d[n++] = '0';
    while (v && n < sizeof(d)) { d[n++] = (char)('0' + v % 10); v /= 10; }
    for (unsigned i = 0; i < n && i + 1 < cap; i++) out[i] = d[n - 1 - i];
    out[n < cap ? n : cap - 1] = 0;
}
/* The text of a numbered paragraph's number (and the character after it), or an empty string for a bullet. */
static size_t list_prefix(struct wd *w, const struct wpap *p, char *out, size_t cap)
{
    out[0] = 0;
    if (!p->ilfo || p->ilfo > w->nlfo) return 0;
    uint32_t lsid = g32(w->lfo, 4 + 16 * (size_t)(p->ilfo - 1));
    for (unsigned i = 0; i < w->nlists; i++) {
        struct wlist *l = &w->lists[i];
        if (l->lsid != lsid) continue;
        unsigned lv = p->ilvl < l->nlvl ? p->ilvl : 0;
        const struct wlvl *v = &l->lvl[lv];
        if (!v->ok) return 0;
        if (v->nfc == 23 || v->nfc == 255) return 0;
        l->count[lv] = l->count[lv] ? l->count[lv] + 1 : v->start;
        for (unsigned k = lv + 1; k < l->nlvl; k++) l->count[k] = 0;
        size_t n = 0;
        for (size_t o = 0; o + 1 < v->xst.n && n + 8 < cap; o += 2) {
            unsigned c = g16(v->xst, o);
            if (c < 9) {
                char num[16];
                const struct wlvl *src = &l->lvl[c < l->nlvl ? c : 0];
                number_text(l->count[c < l->nlvl ? c : 0] ? l->count[c < l->nlvl ? c : 0] : src->start, src->nfc, num, sizeof(num));
                for (char *q = num; *q && n + 1 < cap; q++) out[n++] = *q;
            } else if (c < 127) out[n++] = (char)c;
            else n += wp_utf8_put(out + n, c);
        }
        if (v->follow == 0) out[n++] = '\t'; else if (v->follow == 1) out[n++] = ' ';
        out[n] = 0;
        return n;
    }
    return 0;
}

/* ---- bin tables and FKPs ---------------------------------------------------------------------------- */

static void bte_load(struct wd *w, struct bte *t, uint32_t fc, uint32_t lcb)
{
    t->n = 0;
    struct span s = w->w97 ? w->tbl : w->main;
    if (lcb < 4 || !within(s, fc, lcb)) return;
    t->pnsz = w->w97 ? 4 : 2;
    t->n = (lcb - 4) / (4 + t->pnsz);
    t->fc = sub(s, fc, 4 * (size_t)(t->n + 1));
    t->pn = sub(s, fc + 4 * (size_t)(t->n + 1), (size_t)t->n * t->pnsz);
}
static const uint8_t *bte_page(const struct wd *w, const struct bte *t, uint32_t fc)
{
    uint32_t lo = 0, hi = t->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g32(t->fc, 4 * (size_t)mid) <= fc) lo = mid + 1; else hi = mid;
    }
    if (!lo) return NULL;
    uint32_t i = lo - 1;
    if (fc >= g32(t->fc, 4 * (size_t)(i + 1))) return NULL;
    uint32_t pn = t->pnsz == 4 ? g32(t->pn, 4 * (size_t)i) & 0x3fffff : g16(t->pn, 2 * (size_t)i);
    if (!within(w->main, (size_t)pn * 512, 512)) return NULL;
    return w->main.p + (size_t)pn * 512;
}
struct fkprun { uint32_t fc0, fc1; struct span grp; uint16_t istd; int found; };
/* The CHPX run or the PAPX paragraph of the FKP page that covers fc. */
static struct fkprun fkp_find(const struct wd *w, const struct bte *t, uint32_t fc, int papx)
{
    struct fkprun r = {0, 0, {NULL, 0}, 0, 0};
    const uint8_t *pg = bte_page(w, t, fc);
    if (!pg) return r;
    struct span p = {pg, 512};
    unsigned crun = g8(p, 511);
    if (!crun) return r;
    unsigned bx = papx ? (w->w97 ? 13 : 7) : 1;
    if (4 * (size_t)(crun + 1) + bx * (size_t)crun > 511) return r;
    unsigned lo = 0, hi = crun;
    while (lo < hi) { unsigned mid = (lo + hi) / 2; if (g32(p, 4 * (size_t)mid) <= fc) lo = mid + 1; else hi = mid; }
    if (!lo) return r;
    unsigned i = lo - 1;
    r.fc0 = g32(p, 4 * (size_t)i); r.fc1 = g32(p, 4 * (size_t)(i + 1));
    if (fc >= r.fc1) return r;
    r.found = 1;
    unsigned b = g8(p, 4 * (size_t)(crun + 1) + bx * (size_t)i);
    if (!b) return r;
    size_t o = 2 * (size_t)b;
    if (!papx) { unsigned cb = g8(p, o); r.grp = sub(p, o + 1, cb); return r; }
    unsigned cb = g8(p, o);
    size_t start, len;
    if (cb) { start = o + 1; len = 2 * (size_t)cb - 1; }
    else { cb = g8(p, o + 1); start = o + 2; len = 2 * (size_t)cb; }
    struct span d = sub(p, start, len);
    if (d.n < 2) return r;
    r.istd = (uint16_t)g16(d, 0);
    r.grp = sub(d, 2, d.n - 2);
    return r;
}

/* ---- text ------------------------------------------------------------------------------------------- */

static const uint16_t mac_roman[128] = {
    0x00c4, 0x00c5, 0x00c7, 0x00c9, 0x00d1, 0x00d6, 0x00dc, 0x00e1, 0x00e0, 0x00e2, 0x00e4, 0x00e3, 0x00e5, 0x00e7, 0x00e9, 0x00e8,
    0x00ea, 0x00eb, 0x00ed, 0x00ec, 0x00ee, 0x00ef, 0x00f1, 0x00f3, 0x00f2, 0x00f4, 0x00f6, 0x00f5, 0x00fa, 0x00f9, 0x00fb, 0x00fc,
    0x2020, 0x00b0, 0x00a2, 0x00a3, 0x00a7, 0x2022, 0x00b6, 0x00df, 0x00ae, 0x00a9, 0x2122, 0x00b4, 0x00a8, 0x2260, 0x00c6, 0x00d8,
    0x221e, 0x00b1, 0x2264, 0x2265, 0x00a5, 0x00b5, 0x2202, 0x2211, 0x220f, 0x03c0, 0x222b, 0x00aa, 0x00ba, 0x03a9, 0x00e6, 0x00f8,
    0x00bf, 0x00a1, 0x00ac, 0x221a, 0x0192, 0x2248, 0x2206, 0x00ab, 0x00bb, 0x2026, 0x00a0, 0x00c0, 0x00c3, 0x00d5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201c, 0x201d, 0x2018, 0x2019, 0x00f7, 0x25ca, 0x00ff, 0x0178, 0x2044, 0x20ac, 0x2039, 0x203a, 0xfb01, 0xfb02,
    0x2021, 0x00b7, 0x201a, 0x201e, 0x2030, 0x00c2, 0x00ca, 0x00c1, 0x00cb, 0x00c8, 0x00cd, 0x00ce, 0x00cf, 0x00cc, 0x00d3, 0x00d4,
    0xf8ff, 0x00d2, 0x00da, 0x00db, 0x00d9, 0x0131, 0x02c6, 0x02dc, 0x00af, 0x02d8, 0x02d9, 0x02da, 0x00b8, 0x02dd, 0x02db, 0x02c7};
static uint16_t ansi_unit(const struct wd *w, unsigned b)
{
    if (b < 0x80) return (uint16_t)b;
    if (w->mac) return mac_roman[b - 0x80];
    uint32_t u = wp_cp1252(b);
    return (uint16_t)(u ? u : 0xfffd);
}
static const struct piece *piece_at(const struct wd *w, uint32_t cp)
{
    unsigned lo = 0, hi = w->npieces;
    while (lo < hi) { unsigned mid = (lo + hi) / 2; if (w->pieces[mid].cp1 <= cp) lo = mid + 1; else hi = mid; }
    return lo < w->npieces ? &w->pieces[lo] : NULL;
}
static int build_pieces(struct wd *w, uint32_t fib_fcmin, uint32_t fcclx, uint32_t lcbclx, int complex_file)
{
    if (!lcbclx || (!complex_file && !w->w97)) {   /* Word 97 and later always have a Clx; Word 6 only in fast-saved files */
        if (w->w97) return -EINVAL;
        w->pieces = calloc(1, sizeof(*w->pieces));
        if (!w->pieces) return -ENOMEM;
        w->pieces[0] = (struct piece){0, w->ccp, fib_fcmin, 1, 0};
        w->npieces = 1;
        return 0;
    }
    struct span c = w->w97 ? w->tbl : w->main;
    if (!within(c, fcclx, lcbclx)) return -EINVAL;
    c = sub(c, fcclx, lcbclx);
    size_t at = 0;
    while (at < c.n && g8(c, at) == 1) {   /* clxtGrpprl */
        unsigned cb = g16(c, at + 1);
        if (!within(c, at + 3, cb)) return -EINVAL;
        void *q = realloc(w->grp, (w->ngrp + 1) * sizeof(*w->grp));
        if (!q) return -ENOMEM;
        w->grp = q;
        w->grp[w->ngrp++] = (struct span){c.p + at + 3, cb};
        at += 3 + cb;
    }
    if (g8(c, at) != 2) return -EINVAL;
    uint32_t lcb = g32(c, at + 1);
    if (!within(c, at + 5, lcb) || lcb < 4) return -EINVAL;
    struct span pl = sub(c, at + 5, lcb);
    uint32_t n = (lcb - 4) / 12;
    if (!n) return -EINVAL;
    w->pieces = calloc(n, sizeof(*w->pieces));
    if (!w->pieces) return -ENOMEM;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cp0 = g32(pl, 4 * (size_t)i), cp1 = g32(pl, 4 * (size_t)(i + 1));
        size_t pcd = 4 * (size_t)(n + 1) + 8 * (size_t)i;
        uint32_t fc = g32(pl, pcd + 2);
        unsigned unit = 2;
        if (w->w97) { if (fc & 0x40000000u) { fc = (fc & ~0x40000000u) / 2; unit = 1; } }
        else unit = 1;
        if (cp1 < cp0) return -EINVAL;
        if (cp0 >= w->ccp) break;
        if (cp1 > w->ccp) cp1 = w->ccp;
        w->pieces[w->npieces++] = (struct piece){cp0, cp1, fc, (uint8_t)unit, (uint16_t)g16(pl, pcd + 6)};
    }
    if (!w->npieces || w->pieces[0].cp0 != 0) return -EINVAL;
    return 0;
}
static int decode_text(struct wd *w)
{
    w->ch = malloc((size_t)(w->ccp ? w->ccp : 1) * sizeof(*w->ch));
    if (!w->ch) return -ENOMEM;
    uint32_t done = 0;
    for (unsigned i = 0; i < w->npieces; i++) {
        const struct piece *p = &w->pieces[i];
        if (p->cp0 != done) return -EINVAL;
        size_t need = (size_t)(p->cp1 - p->cp0) * p->unit;
        if (!within(w->main, p->fc, need)) return -EINVAL;
        const uint8_t *src = w->main.p + p->fc;
        for (uint32_t k = 0; k < p->cp1 - p->cp0; k++)
            w->ch[p->cp0 + k] = p->unit == 1 ? ansi_unit(w, src[k]) : (uint16_t)(src[2 * k] | src[2 * k + 1] << 8);
        done = p->cp1;
    }
    return done == w->ccp ? 0 : -EINVAL;
}
static uint32_t fc_of(const struct wd *w, const struct piece *p, uint32_t cp) { (void)w; return p->fc + (cp - p->cp0) * p->unit; }

/* ---- pictures ---------------------------------------------------------------------------------------- */

static struct wp_pic *make_pic(struct wd *w, unsigned kind, struct span data, int gw, int gh, unsigned mx, unsigned my)
{
    if (!data.p || !data.n) return NULL;
    struct wp_pic *p = wp_pic_new(kind, data.p, data.n);
    if (!p) { w->err = -ENOMEM; return NULL; }
    if (gw > 0 && gh > 0) { p->goalw = gw; p->goalh = gh; }
    unsigned sx = mx / 10, sy = my / 10;
    p->scalex = (uint16_t)(sx < 1 ? 100 : sx > 1000 ? 1000 : sx);
    p->scaley = (uint16_t)(sy < 1 ? 100 : sy > 1000 ? 1000 : sy);
    return p;
}
/* The first usable BLIP of a run of OfficeArt records (containers are entered). */
static int blip_scan(struct span s, int depth, unsigned *kind, struct span *out)
{
    size_t at = 0;
    while (at + 8 <= s.n) {
        unsigned ver_inst = g16(s, at), type = g16(s, at + 2);
        size_t len = g32(s, at + 4), body = at + 8;
        if (len > s.n - body) len = s.n - body;
        if ((ver_inst & 15) == 15) {   /* a container */
            int r = depth < 8 ? blip_scan(sub(s, body, len), depth + 1, kind, out) : 0;
            if (r) return r;
        } else if (type == 0xf007 && len >= 36) {   /* an FBSE: 36 bytes, the name, then the BLIP itself */
            size_t skip = 36 + (size_t)g8(s, body + 33);
            if (skip < len) { int r = depth < 8 ? blip_scan(sub(s, body + skip, len - skip), depth + 1, kind, out) : 0; if (r) return r; }
        } else if (type >= 0xf01a && type <= 0xf02a) {
            unsigned inst = ver_inst >> 4;
            size_t skip = 16 + ((inst & 1) ? 16 : 0);
            if (type == 0xf01d || type == 0xf01e || type == 0xf01f) {
                size_t off = body + skip + 1;   /* the tag byte */
                if (off <= body + len) {
                    *out = sub(s, off, body + len - off);
                    *kind = type == 0xf01d ? WP_PIC_JPEG : type == 0xf01e ? WP_PIC_PNG : WP_PIC_DIB;
                    return out->p != NULL;
                }
            } else return -1;   /* a metafile or TIFF BLIP: not supported */
        }
        at = body + len;
    }
    return 0;
}
/* The picture of a 0x01 character: 0 = nothing to show (counted), 1 = inserted. */
static int insert_picture(struct wd *w, int32_t fcpic, const struct wp_cattr *attr)
{
    struct span src = w->w97 ? w->data : w->main;
    if (fcpic < 0 || !within(src, (size_t)fcpic, 40)) { w->rep->skipped_pictures++; return 0; }
    struct span h = sub(src, (size_t)fcpic, src.n - (size_t)fcpic);
    uint32_t lcb = g32(h, 0); unsigned cbh = g16(h, 4), mm = g16(h, 6);
    if (lcb < cbh || cbh < 0x20 || !within(h, 0, lcb)) { w->rep->skipped_pictures++; return 0; }
    int gw = s16(h, 28), gh = s16(h, 30);
    struct span data = sub(h, cbh, lcb - cbh);
    struct wp_pic *p = NULL;
    if (mm >= 1 && mm <= 8) {   /* a Windows metafile (Word 6; also the legacy form in Word 97) */
        p = make_pic(w, WP_PIC_WMF, data, gw, gh, g16(h, 32), g16(h, 34));
        if (!p) { if (!w->err) w->rep->skipped_pictures++; return 0; }
    } else if (w->w97 && (mm == 0x64 || mm == 0x66)) {
        struct span rec = data;
        if (mm == 0x66 && rec.n) { size_t nm = 1 + g8(rec, 0); rec = sub(rec, nm, rec.n >= nm ? rec.n - nm : 0); }
        unsigned kind = 0; struct span blip = {NULL, 0};
        int r = blip_scan(rec, 0, &kind, &blip);
        if (r <= 0) { w->rep->skipped_pictures++; if (r < 0) w->rep->unsupported_pictures++; return 0; }
        p = make_pic(w, kind, blip, gw, gh, g16(h, 32), g16(h, 34));
        if (!p) { if (!w->err) w->rep->skipped_pictures++; return 0; }
    } else { w->rep->skipped_pictures++; w->rep->unsupported_pictures++; return 0; }
    int id = wp_pic_attach(w->doc, p);
    if (id < 0) { if (id == -ENOMEM) w->err = id; w->rep->skipped_pictures++; return 0; }
    struct wp_cattr a = *attr; a.pic = (uint16_t)id;
    int e = wp_append(w->doc, WP_PIC_MARK, WP_PIC_MARK_LEN, &a);
    if (e) w->err = e; else w->rep->kept_pictures++;
    return !e;
}

/* ---- the main walk ------------------------------------------------------------------------------------- */

static int flush(struct wd *w, size_t n, const struct wp_cattr *a)
{
    if (!n) return 0;
    int e = wp_append(w->doc, w->buf, n, a);
    if (e) w->err = e;
    return e;
}
static int ensure(struct wd *w, size_t need)
{
    if (need <= w->bufcap) return 0;
    size_t c = w->bufcap ? w->bufcap : 4096;
    while (c < need) c *= 2;
    char *q = realloc(w->buf, c);
    if (!q) return w->err = -ENOMEM;
    w->buf = q; w->bufcap = c;
    return 0;
}
static struct wp_cattr cattr_of(struct wd *w, const struct wchp *c)
{
    struct wp_cattr a = wp_default_cattr();
    a.font = (uint8_t)font_index(w, c->ftc);
    a.size = (uint16_t)(c->hps < WP_SIZE_MIN ? WP_SIZE_MIN : c->hps > WP_SIZE_MAX ? WP_SIZE_MAX : c->hps);
    a.flags = (uint8_t)((c->bold ? WP_BOLD : 0) | (c->italic ? WP_ITALIC : 0) | (c->kul && c->kul != 5 ? WP_UNDERLINE : 0) | (c->strike ? WP_STRIKE : 0));
    if (c->cv >= 0) a.color = (uint8_t)wp_color_add(w->doc, (uint32_t)c->cv);
    else if (c->ico) a.color = (uint8_t)wp_color_add(w->doc, (uint32_t)ico_rgb[c->ico][0] << 16 | (uint32_t)ico_rgb[c->ico][1] << 8 | ico_rgb[c->ico][2]);
    return a;
}
static int para_end(unsigned c) { return c == 0x0d || c == 0x07 || c == 0x0c; }

static void pap_to_model(const struct wpap *p, struct wp_pattr *m, int numbered)
{
    *m = wp_default_pattr();
    m->align = p->jc == 1 ? WP_CENTER : p->jc == 2 ? WP_RIGHT : p->jc == 3 ? WP_JUSTIFY : WP_LEFT;
    m->bullet = p->bullet;
    m->left = p->dleft < 0 ? 0 : p->dleft > WP_INDENT_MAX ? WP_INDENT_MAX : p->dleft;
    m->right = p->dright < 0 ? 0 : p->dright > WP_INDENT_MAX ? WP_INDENT_MAX : p->dright;
    m->first = p->dleft1 < -WP_INDENT_MAX ? -WP_INDENT_MAX : p->dleft1 > WP_INDENT_MAX ? WP_INDENT_MAX : p->dleft1;
    if (m->left + m->first < 0) m->first = -m->left;
    for (unsigned i = 0; i < p->ntabs && m->ntabs < WP_TABS; i++) {
        if ((p->tbd[i] & 7) > 3 || p->tab[i] <= 0 || p->tab[i] > WP_INDENT_MAX) continue;   /* bar tabs are not tabs */
        if (m->ntabs && p->tab[i] <= m->tabs[m->ntabs - 1]) continue;
        m->tabs[m->ntabs++] = p->tab[i];
    }
    if (numbered && m->first < 0 && m->ntabs < WP_TABS && (!m->ntabs || m->tabs[m->ntabs - 1] < m->left)) {   /* the text after a hanging number starts at the left indent */
        int ok = 1;
        for (unsigned i = 0; i < m->ntabs; i++) if (m->tabs[i] == m->left) ok = 0;
        if (ok && m->left > 0) m->tabs[m->ntabs++] = m->left;
    }
}

static int walk(struct wd *w)
{
    struct wp_doc *doc = w->doc;
    unsigned fstack[16], fdepth = 0;
    uint32_t cp = 0;
    while (cp < w->ccp && !w->err) {
        uint32_t end = cp;
        while (end < w->ccp && !para_end(w->ch[end])) end++;
        uint32_t last = end < w->ccp ? end : w->ccp - 1;
        int has_mark = end < w->ccp, final = last == w->ccp - 1;
        const struct piece *mp = piece_at(w, last);
        if (!mp) return -EINVAL;
        uint32_t mfc = fc_of(w, mp, last);
        /* paragraph properties: the style's, the PAPX of the mark, the grpprl of its piece */
        struct fkprun pr = fkp_find(w, &w->papx, mfc, 1);
        unsigned istd = pr.found ? pr.istd : 0;
        if (istd >= w->nstyles) istd = 0;
        struct wpap pap; memset(&pap, 0, sizeof(pap));
        struct wchp base; chp_null(w, &base);
        if (w->nstyles && w->styles[istd].valid) { style_compute(w, istd, 0); pap = w->styles[istd].pap; base = w->styles[istd].chp; }
        else if (w->nstyles && w->styles[0].valid) { style_compute(w, 0, 0); pap = w->styles[0].pap; base = w->styles[0].chp; }
        pap.istd = (uint16_t)istd;
        if (pr.grp.p) apply_pap(w, &pap, pr.grp);
        if ((mp->prm & 1) && (mp->prm >> 1) < w->ngrp) apply_pap(w, &pap, w->grp[mp->prm >> 1]);
        char prefix[64];
        size_t npref = list_prefix(w, &pap, prefix, sizeof(prefix));
        struct wp_pattr model;
        pap_to_model(&pap, &model, npref != 0);
        doc->paras[doc->npara - 1].a = model;
        /* the paragraph's text */
        struct wp_cattr first_attr = cattr_of(w, &base);
        if (npref) {
            if (ensure(w, npref + 4)) break;
            memcpy(w->buf, prefix, npref);
            if (flush(w, npref, &first_attr)) break;
        }
        uint32_t c = cp;
        while (c <= last && !w->err) {
            const struct piece *p = piece_at(w, c);
            if (!p) return -EINVAL;
            uint32_t fc = fc_of(w, p, c);
            struct fkprun cr = fkp_find(w, &w->chpx, fc, 0);
            uint32_t ce = c + 1;
            if (cr.found && cr.fc1 > fc) { uint32_t units = (cr.fc1 - fc + p->unit - 1) / p->unit; ce = c + (units ? units : 1); }
            else ce = p->cp1;
            if (ce > p->cp1) ce = p->cp1;
            if (ce > last + 1) ce = last + 1;
            struct wchp chp = base, ref = base;
            if (cr.grp.p) apply_chp(w, &chp, &ref, cr.grp);
            if ((p->prm & 1) && (p->prm >> 1) < w->ngrp) apply_chp(w, &chp, &ref, w->grp[p->prm >> 1]);
            struct wp_cattr attr = cattr_of(w, &chp);
            if (ensure(w, (size_t)(ce - c) * 4 + 8)) break;
            size_t n = 0;
            for (uint32_t k = c; k < ce; k++) {
                unsigned u = w->ch[k];
                int hidden = chp.vanish;
                for (unsigned f = 0; f < fdepth; f++) if (fstack[f] == 1) hidden = 1;
                if (u == 0x13) { if (fdepth < 16) fstack[fdepth++] = 1; continue; }
                if (u == 0x14) { if (fdepth) fstack[fdepth - 1] = 2; continue; }
                if (u == 0x15) { if (fdepth) fdepth--; continue; }
                if (hidden) continue;
                if (k == last && has_mark) {   /* the paragraph mark */
                    if (final) continue;
                    if (u == 0x07) w->buf[n++] = pap.ttp ? '\n' : pap.intable ? '\t' : '\n';
                    else w->buf[n++] = '\n';
                    continue;
                }
                if (u == 0x01 && chp.fspec) {
                    if (flush(w, n, &attr)) break;
                    n = 0;
                    insert_picture(w, chp.fcpic, &attr);
                    continue;
                }
                if (chp.fspec && u < 0x20) continue;   /* other special characters: footnote references, drawings, page numbers */
                if (u == 0x0b) { w->buf[n++] = WP_SOFT_BREAK; continue; }
                if (u == 0x09) { w->buf[n++] = '\t'; continue; }
                if (u == 0x1e) { w->buf[n++] = '-'; continue; }
                if (u < 0x20 || u == 0x7f || u == 0xfffc || u == 0x1f) continue;
                if (u >= 0xd800 && u < 0xdc00 && k + 1 < ce && w->ch[k + 1] >= 0xdc00 && w->ch[k + 1] < 0xe000) {
                    uint32_t v = 0x10000 + (((uint32_t)u - 0xd800) << 10) + (w->ch[k + 1] - 0xdc00u);
                    n += wp_utf8_put(w->buf + n, v); k++; continue;
                }
                if (u >= 0xd800 && u < 0xe000) u = 0xfffd;
                if (chp.caps) { if (u >= 'a' && u <= 'z') u -= 32; else if (u >= 0xe0 && u <= 0xfe && u != 0xf7) u -= 32; }
                n += wp_utf8_put(w->buf + n, u);
            }
            if (n && !w->err) flush(w, n, &attr);
            c = ce;
        }
        cp = last + 1;
    }
    return w->err;
}

int wp_is_word(const char *buf, size_t n) { return wp_is_ole(buf, n); }

static void wd_free(struct wd *w)
{
    free(w->pieces); free(w->grp); free(w->styles); free(w->fonts); free(w->lists); free(w->ch); free(w->buf);
}

int wp_word_read(struct wp_doc *dst, const uint8_t *buf, size_t n, struct wp_report *rep)
{
    struct wp_report none;
    if (!rep) rep = &none;
    memset(rep, 0, sizeof(*rep));
    struct wp_ole ole;
    int e = wp_ole_open(&ole, buf, n);
    if (e) { rep->reason = e == -EINVAL ? "This is not a valid Word document." : NULL; return e; }
    uint8_t *main = NULL, *tbl = NULL, *dat = NULL; size_t mainlen = 0, tbllen = 0, datlen = 0;
    struct wd w; memset(&w, 0, sizeof(w));
    struct wp_doc work; memset(&work, 0, sizeof(work));
    e = wp_ole_read(&ole, "WordDocument", &main, &mainlen);
    if (e) { rep->reason = "This compound file is not a Word document."; wp_ole_close(&ole); return e == -ENOENT ? -EINVAL : e; }
    w.main = (struct span){main, mainlen};
    unsigned wident = g16(w.main, 0), nfib = g16(w.main, 2), flags = g16(w.main, 10);
    if ((wident != 0xa5ec && wident != 0xa5dc) || mainlen < 0x60) { rep->reason = "This is not a Word 6 or later document."; e = -EINVAL; goto out; }
    if (nfib < 101) { rep->reason = "Documents saved by versions of Word before Word 6.0 cannot be opened."; e = -ENOTSUP; goto out; }
    if (flags & 0x100) { rep->reason = "The document is protected by a password and cannot be opened."; e = -ENOTSUP; goto out; }
    w.w97 = nfib > 105; w.mac = g16(w.main, 0x14) == 256; w.rep = rep;
    rep->version = !w.w97 ? (nfib >= 104 ? 95u : 6u) : 97u;
    uint32_t fcmin, ccp, fcstsh, lcbstsh, fcchpx, lcbchpx, fcpapx, lcbpapx, fcffn, lcbffn, fcclx, lcbclx, fclst = 0, lcblst = 0, fclfo = 0, lcblfo = 0, ccpother = 0;
    if (!w.w97) {
        if (mainlen < 0x168) { rep->reason = "The Word document is damaged."; e = -EINVAL; goto out; }
        fcmin = g32(w.main, 0x18); ccp = g32(w.main, 0x34);
        for (unsigned o = 0x38; o <= 0x50; o += 4) ccpother += g32(w.main, o) & 0x3fffffffu;
        fcstsh = g32(w.main, 0x60); lcbstsh = g32(w.main, 0x64); fcchpx = g32(w.main, 184); lcbchpx = g32(w.main, 188);
        fcpapx = g32(w.main, 192); lcbpapx = g32(w.main, 196); fcffn = g32(w.main, 208); lcbffn = g32(w.main, 212);
        fcclx = g32(w.main, 352); lcbclx = g32(w.main, 356);
        w.tbl = w.main;
    } else {
        if (mainlen < 0x1ae || g16(w.main, 32) < 14) { rep->reason = "The Word document is damaged."; e = -EINVAL; goto out; }
        fcmin = g32(w.main, 0x18); ccp = g32(w.main, 0x4c);
        for (unsigned o = 0x50; o <= 0x6c; o += 4) ccpother += g32(w.main, o) & 0x3fffffffu;
        fcstsh = g32(w.main, 0xa2); lcbstsh = g32(w.main, 0xa6); fcchpx = g32(w.main, 0xfa); lcbchpx = g32(w.main, 0xfe);
        fcpapx = g32(w.main, 0x102); lcbpapx = g32(w.main, 0x106); fcffn = g32(w.main, 0x112); lcbffn = g32(w.main, 0x116);
        fcclx = g32(w.main, 0x1a2); lcbclx = g32(w.main, 0x1a6);
        if (mainlen >= 0x2f2) { fclst = g32(w.main, 0x2e2); lcblst = g32(w.main, 0x2e6); fclfo = g32(w.main, 0x2ea); lcblfo = g32(w.main, 0x2ee); }
        e = wp_ole_read(&ole, flags & 0x200 ? "1Table" : "0Table", &tbl, &tbllen);
        if (e) { rep->reason = "The Word document has no table stream."; e = e == -ENOENT ? -EINVAL : e; goto out; }
        w.tbl = (struct span){tbl, tbllen};
        if (wp_ole_read(&ole, "Data", &dat, &datlen)) { dat = NULL; datlen = 0; }
        w.data = (struct span){dat, datlen};
    }
    if (ccp > WP_LIMIT) { e = -EFBIG; goto out; }
    w.ccp = ccp;
    if (ccpother) rep->ignored_parts = 1;
    e = wp_init(&work);
    if (e) goto out;
    w.doc = &work;
    e = build_pieces(&w, fcmin, fcclx, lcbclx, (flags & 4) != 0);
    if (!e && ccp) e = decode_text(&w);
    if (e) { if (e == -EINVAL) rep->reason = "The Word document is damaged."; goto out; }
    w.default_ftc = 0;
    e = load_styles(&w, fcstsh, lcbstsh);
    if (e) goto out;
    load_fonts(&w, fcffn, lcbffn);
    if (w.w97) load_lists(&w, fclst, lcblst, fclfo, lcblfo);
    bte_load(&w, &w.chpx, fcchpx, lcbchpx);
    bte_load(&w, &w.papx, fcpapx, lcbpapx);
    if (w.err) { e = w.err; goto out; }
    if (ccp) e = walk(&w);
    if (!e) e = w.err;
out:
    free(main); free(tbl); free(dat);
    wd_free(&w);
    wp_ole_close(&ole);
    if (e) { wp_free(&work); return e; }
    wp_free(dst);
    *dst = work;
    return 0;
}
