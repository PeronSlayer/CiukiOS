/* SPDX-License-Identifier: MIT
 * Microsoft Write (.wri) import for WordPad (G28b).
 *
 * Source: the Windows 3.1 SDK topic ".WRI Write File Format" (header with fcMac and the page numbers pnPara, pnFntb, pnSep,
 * pnSetb, pnPgtb, pnFfntb and pnMac; text from byte 128; the 128-byte pages of character and paragraph FODs with their
 * FPROPs; the CHP and PAP prefixes and their default values; TBD tab descriptors; the FFN font table; the picture header,
 * the BITMAP and METAFILEPICT members and the OLE object header), as reproduced by msxnet.org's word2rtf notes, checked
 * against Write 3.0 files.
 *
 * Supported: text, fonts, sizes, bold, italic, underline, alignment (left, centre, right, justified), indents, tab stops,
 * bitmap and metafile pictures (monochrome and 16-colour bitmaps become DIBs, metafiles are kept), static DIB objects.
 * Header and footer paragraphs, section properties and other OLE objects are not shown (counted). */
#include "rich.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct wspan { const uint8_t *p; size_t n; };
static unsigned r8(struct wspan s, size_t o) { return o < s.n ? s.p[o] : 0u; }
static unsigned r16(struct wspan s, size_t o) { return o + 2 <= s.n ? (unsigned)s.p[o] | (unsigned)s.p[o + 1] << 8 : 0u; }
static uint32_t r32(struct wspan s, size_t o) { return o + 4 <= s.n ? (uint32_t)r16(s, o) | (uint32_t)r16(s, o + 2) << 16 : 0u; }
static int ok(struct wspan s, size_t o, size_t n) { return o <= s.n && n <= s.n - o; }

int wp_is_write(const char *buf, size_t n)
{
    struct wspan s = {(const uint8_t *)buf, n};
    return n >= 128 && (r16(s, 0) == 0xbe31 || r16(s, 0) == 0xbe32) && r16(s, 4) == 0xab00;
}

#define FODS_MAX 65536
struct fod { uint32_t lim; uint8_t prop[80]; unsigned nprop; int has; };
/* All FODs of pages [first, end): each covers the text up to its fcLim; a missing FPROP (0xffff) is the default. */
static int read_fods(struct wspan f, unsigned first, unsigned end, struct fod **out, unsigned *count)
{
    struct fod *v = NULL; unsigned n = 0, cap = 0;
    for (unsigned pg = first; pg < end; pg++) {
        size_t base = (size_t)pg * 128;
        if (!ok(f, base, 128)) { free(v); return -EINVAL; }
        unsigned cfod = r8(f, base + 127);
        if (cfod > 20) { free(v); return -EINVAL; }
        for (unsigned i = 0; i < cfod; i++) {
            size_t fo = base + 4 + (size_t)i * 6;
            if (n == cap) {
                unsigned c = cap ? cap * 2 : 64;
                if (c > FODS_MAX) { free(v); return -EFBIG; }
                struct fod *q = realloc(v, c * sizeof(*v));
                if (!q) { free(v); return -ENOMEM; }
                v = q; cap = c;
            }
            struct fod *d = &v[n++];
            memset(d, 0, sizeof(*d));
            d->lim = r32(f, fo);
            unsigned bf = r16(f, fo + 4);
            if (bf != 0xffff) {
                size_t po = base + 4 + bf;
                unsigned cch = r8(f, po);
                if (po + 1 + cch <= base + 127 && po > base) {
                    d->has = 1; d->nprop = cch < sizeof(d->prop) ? cch : (unsigned)sizeof(d->prop);
                    memcpy(d->prop, f.p + po + 1, d->nprop);
                }
            }
        }
    }
    *out = v; *count = n;
    return 0;
}

static const uint8_t win16[16][3] = {{0, 0, 0}, {128, 0, 0}, {0, 128, 0}, {128, 128, 0}, {0, 0, 128}, {128, 0, 128}, {0, 128, 128}, {192, 192, 192},
                                     {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0}, {0, 0, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
/* A device-dependent 1- or 4-bit bitmap (rows top-down, padded to 16 bits) as a bottom-up DIB. */
static uint8_t *ddb_to_dib(const uint8_t *bits, size_t nbits, unsigned w, unsigned h, unsigned wbytes, unsigned bpp, size_t *len)
{
    if ((bpp != 1 && bpp != 4) || !w || !h || w > 8192 || h > 8192 || wbytes * 8 < w * bpp || (size_t)wbytes * h > nbits) return NULL;
    size_t pal = (size_t)(1u << bpp) * 4, stride = (((size_t)w * bpp + 31) / 32) * 4, total = 40 + pal + stride * h;
    uint8_t *d = calloc(1, total);
    if (!d) return NULL;
    put32(d, 40); put32(d + 4, w); put32(d + 8, h); d[12] = 1; d[14] = (uint8_t)bpp; put32(d + 20, (uint32_t)(stride * h));
    for (unsigned i = 0; i < (1u << bpp); i++) {
        unsigned c = bpp == 1 ? (i ? 15 : 0) : i;   /* monochrome: bit 1 is white */
        d[40 + 4 * i] = win16[c][2]; d[41 + 4 * i] = win16[c][1]; d[42 + 4 * i] = win16[c][0];
    }
    for (unsigned y = 0; y < h; y++) memcpy(d + 40 + pal + stride * (h - 1 - y), bits + (size_t)y * wbytes, (wbytes * 1u < stride ? wbytes : stride));
    *len = total;
    return d;
}

static int add_picture(struct wp_doc *doc, struct wp_report *rep, struct wspan f, size_t at, size_t end)
{
    struct wspan h = {f.p + at, end - at};
    if (h.n < 40) { rep->skipped_pictures++; return 0; }
    unsigned mm = r16(h, 0), cbh = r16(h, 30);
    int dxs = (int)r16(h, 10), dys = (int)r16(h, 12);
    unsigned mx = r16(h, 36), my = r16(h, 38);
    struct wp_pic *p = NULL;
    if (mm == 0xe4) {   /* an OLE object: only static DIBs are shown */
        uint32_t size = r32(h, 16);
        if (r16(h, 6) == 1 && cbh >= 40 && ok(h, cbh, size)) {
            struct wspan o = {h.p + cbh, size};
            if (r32(o, 0) == 0x501 && r32(o, 4) == 3) {
                uint32_t tl = r32(o, 8);
                if (tl >= 4 && tl < 64 && ok(o, 12, tl) && !memcmp(o.p + 12, "DIB", 4)) {
                    size_t dat = 12 + tl + 8 + 4;
                    uint32_t dl = r32(o, 12 + tl + 8);
                    if (ok(o, dat, dl) && dl >= 40) p = wp_pic_new(WP_PIC_DIB, o.p + dat, dl);
                }
            }
        }
        if (!p) { rep->skipped_objects++; return 0; }
    } else if (cbh >= 40 && ok(h, cbh, 1)) {
        size_t size = r32(h, 32);
        if (!size) size = r16(h, 14);
        if (size > h.n - cbh) size = h.n - cbh;
        if (mm == 0xe3) {   /* a bitmap */
            size_t dl;
            uint8_t *dib = ddb_to_dib(h.p + cbh, size, r16(h, 18), r16(h, 20), r16(h, 22), r8(h, 25), &dl);
            if (r8(h, 24) != 1) { free(dib); dib = NULL; }
            if (!dib) { rep->skipped_pictures++; rep->unsupported_pictures++; return 0; }
            p = wp_pic_new(WP_PIC_DIB, dib, dl);
            free(dib);
        } else if (size) p = wp_pic_new(WP_PIC_WMF, h.p + cbh, size);
    }
    if (!p) { rep->skipped_pictures++; return 0; }
    if (dxs > 0 && dys > 0) { p->goalw = dxs; p->goalh = dys; }
    unsigned sx = mx / 10, sy = my / 10;
    p->scalex = (uint16_t)(sx < 1 ? 100 : sx > 1000 ? 1000 : sx); p->scaley = (uint16_t)(sy < 1 ? 100 : sy > 1000 ? 1000 : sy);
    int id = wp_pic_attach(doc, p);
    if (id < 0) { if (id == -ENOMEM) return id; rep->skipped_pictures++; return 0; }
    struct wp_cattr a = wp_default_cattr(); a.pic = (uint16_t)id;
    int e = wp_append(doc, WP_PIC_MARK, WP_PIC_MARK_LEN, &a);
    if (!e) rep->kept_pictures++;
    return e;
}

static unsigned family_of(unsigned ffid, const char *name)
{
    unsigned f = wp_family_for_name(name);
    if (f != WP_SANS) return f;
    ffid &= 0xf0;
    return ffid == 0x10 ? WP_SERIF : ffid == 0x30 ? WP_MONO : WP_SANS;
}

int wp_write_read(struct wp_doc *dst, const uint8_t *buf, size_t n, struct wp_report *rep)
{
    struct wp_report none;
    if (!rep) rep = &none;
    memset(rep, 0, sizeof(*rep));
    struct wspan f = {buf, n};
    if (!wp_is_write((const char *)buf, n)) { rep->reason = "This is not a Write document."; return -EINVAL; }
    rep->version = 3;
    uint32_t fcmac = r32(f, 14);
    unsigned pnpara = r16(f, 18), pnfntb = r16(f, 20), pnffntb = r16(f, 28), pnmac = r16(f, 96);
    if (r16(f, 0) == 0xbe32) { /* OLE objects present: pictures that are objects are handled below */ }
    if (!pnmac) { rep->reason = "This is a Word document, not a Write document."; return -EINVAL; }
    if (fcmac < 128) { rep->reason = "The Write document is damaged."; return -EINVAL; }
    if (fcmac > n) fcmac = (uint32_t)n;
    unsigned pnchar = (fcmac + 127) / 128;
    if (pnpara < pnchar || pnfntb < pnpara || (size_t)pnfntb * 128 > n + 128) { rep->reason = "The Write document is damaged."; return -EINVAL; }
    if ((uint64_t)(fcmac - 128) > WP_LIMIT) return -EFBIG;
    struct fod *chars = NULL, *paras = NULL; unsigned nch = 0, npa = 0;
    int e = read_fods(f, pnchar, pnpara, &chars, &nch);
    if (!e) e = read_fods(f, pnpara, pnfntb, &paras, &npa);
    if (e) { free(chars); free(paras); if (e == -EINVAL) rep->reason = "The Write document is damaged."; return e; }
    struct wp_doc work;
    e = wp_init(&work);
    if (e) { free(chars); free(paras); return e; }
    /* fonts: the FFNTB; the font code of a character is the index of its entry */
    int *fmap = calloc(1024, sizeof(*fmap));
    unsigned nfont = 0;
    if (!fmap) { free(chars); free(paras); wp_free(&work); return -ENOMEM; }
    if (pnffntb && pnffntb < pnmac && (size_t)pnffntb * 128 + 2 <= n) {
        size_t o = (size_t)pnffntb * 128;
        unsigned cffn = r16(f, o);
        o += 2;
        for (unsigned i = 0; i < cffn && nfont < 1024; i++) {
            unsigned cb = r16(f, o);
            if (cb == 0xffff) { o = (o + 127) / 128 * 128; cb = r16(f, o); }
            if (!cb || !ok(f, o + 2, cb)) break;
            char name[40]; size_t k = 0;
            for (size_t q = o + 3; q < o + 2 + cb && f.p[q] && k < sizeof(name) - 1; q++) name[k++] = f.p[q] >= 32 && f.p[q] < 127 ? (char)f.p[q] : '?';
            name[k] = 0;
            int idx = k ? wp_font_add(&work, name, family_of(r8(f, o + 2), name)) : -1;
            fmap[nfont++] = idx >= 0 ? idx : (int)family_of(r8(f, o + 2), name);
            o += 2 + cb;
        }
    }
    unsigned ci = 0;
    uint32_t pstart = 128;
    for (unsigned pi = 0; pi < npa && !e; pi++) {
        uint32_t plim = paras[pi].lim;
        if (plim > fcmac) plim = fcmac;
        if (plim <= pstart) { if (plim < pstart || paras[pi].lim >= fcmac) break; continue; }
        uint8_t pap[80];
        memset(pap, 0, sizeof(pap)); pap[0] = 61; pap[2] = 30; pap[10] = 240 & 255; pap[11] = 240 >> 8;
        if (paras[pi].has) memcpy(pap, paras[pi].prop, paras[pi].nprop);
        struct wspan pp = {pap, sizeof(pap)};
        int last = pi + 1 >= npa || plim >= fcmac;
        unsigned hf = r8(pp, 16);
        if (hf & 0x06) { rep->ignored_parts = 1; pstart = plim; continue; }   /* header and footer paragraphs */
        struct wp_pattr m = wp_default_pattr();
        unsigned jc = r8(pp, 1) & 3;
        m.align = jc == 1 ? WP_CENTER : jc == 2 ? WP_RIGHT : jc == 3 ? WP_JUSTIFY : WP_LEFT;
        int dr = (int)r16(pp, 4), dl = (int)r16(pp, 6), d1 = (int)(int16_t)r16(pp, 8);
        m.right = dr > WP_INDENT_MAX ? WP_INDENT_MAX : dr; m.left = dl > WP_INDENT_MAX ? WP_INDENT_MAX : dl;
        m.first = d1 < -WP_INDENT_MAX ? -WP_INDENT_MAX : d1 > WP_INDENT_MAX ? WP_INDENT_MAX : d1;
        if (m.left + m.first < 0) m.first = -m.left;
        for (unsigned t = 0; t < 14 && m.ntabs < WP_TABS; t++) {
            int dx = (int)r16(pp, 22 + 4 * t);
            if (dx <= 0 || dx > WP_INDENT_MAX || (m.ntabs && dx <= m.tabs[m.ntabs - 1])) continue;
            m.tabs[m.ntabs++] = dx;
        }
        work.paras[work.npara - 1].a = m;
        if (hf & 0x10) {   /* a picture paragraph */
            e = add_picture(&work, rep, f, pstart, plim);
            if (!e && !last) { struct wp_cattr a = wp_default_cattr(); e = wp_append(&work, "\n", 1, &a); }
            pstart = plim;
            continue;
        }
        uint32_t textend = plim;
        int has_mark = 0;
        if (textend >= pstart + 2 && f.p[textend - 2] == 13 && f.p[textend - 1] == 10) { textend -= 2; has_mark = 1; }
        uint32_t at = pstart;
        struct wp_cattr last_attr = wp_default_cattr();   /* the paragraph mark has the format of the last character */
        while (at < textend && !e) {
            while (ci + 1 < nch && chars[ci].lim <= at) ci++;
            uint32_t runend = ci < nch && chars[ci].lim > at ? chars[ci].lim : textend;
            if (runend > textend) runend = textend;
            uint8_t chp[80];
            memset(chp, 0, sizeof(chp)); chp[0] = 1; chp[2] = 24;
            if (ci < nch && chars[ci].has) memcpy(chp, chars[ci].prop, chars[ci].nprop);
            struct wspan cc = {chp, sizeof(chp)};
            struct wp_cattr a = wp_default_cattr();
            unsigned ftc = (r8(cc, 1) >> 2) | ((r8(cc, 4) & 7u) << 6);
            a.font = (uint8_t)(ftc < nfont ? fmap[ftc] : 0);
            a.size = (uint16_t)(r8(cc, 2) < WP_SIZE_MIN ? WP_SIZE_DEFAULT : r8(cc, 2));
            a.flags = (uint8_t)((r8(cc, 1) & 1 ? WP_BOLD : 0) | (r8(cc, 1) & 2 ? WP_ITALIC : 0) | (r8(cc, 3) & 1 ? WP_UNDERLINE : 0));
            int special = r8(cc, 3) & 0x40;
            char tmp[1024]; size_t k = 0;
            for (uint32_t i = at; i < runend && !e; i++) {
                unsigned c = f.p[i];
                if (special) continue;                  /* "(page)" */
                if (c == 12 || c == 31 || (c < 32 && c != 9)) continue;
                uint32_t u = c < 0x80 ? c : wp_cp1252(c);
                if (!u) u = 0xfffd;
                if (k + 4 > sizeof(tmp)) { e = wp_append(&work, tmp, k, &a); k = 0; }
                k += wp_utf8_put(tmp + k, u);
            }
            if (!e && k) e = wp_append(&work, tmp, k, &a);
            if (k) last_attr = a;
            at = runend;
        }
        if (!e && has_mark && !last) e = wp_append(&work, "\n", 1, &last_attr);
        pstart = plim;
    }
    free(chars); free(paras); free(fmap);
    if (e) { wp_free(&work); return e; }
    wp_free(dst); *dst = work;
    return 0;
}
