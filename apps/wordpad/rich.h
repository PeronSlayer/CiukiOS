/* SPDX-License-Identifier: MIT
 * WordPad document model (G28): UTF-8 text with character runs and
 * paragraph attributes, an RTF reader and writer, and a plain text reader and
 * writer. No UI and no system calls: the host tests run the production code.
 *
 * Units follow RTF: sizes are half-points, indents and tab stops are twips
 * (1/1440 inch). Text uses "\n" between paragraphs; U+000B is a soft line
 * break inside a paragraph (RTF \line). One run table covers the text; one
 * paragraph record covers each "\n"-terminated paragraph (the last paragraph
 * has no terminator, so a document always has at least one paragraph).
 * Every mutator either completes or returns -ENOMEM/-EFBIG with the document
 * unchanged. */
#ifndef CIUKI_WORDPAD_RICH_H
#define CIUKI_WORDPAD_RICH_H
#include <stddef.h>
#include <stdint.h>

#define WP_LIMIT (8u * 1024u * 1024u)   /* text bytes (the 0.8 CiukNote limit was 60 KiB) */
#define WP_FONTS 16
#define WP_COLORS 32
#define WP_TABS 8
#define WP_SIZE_MIN 2                   /* half-points */
#define WP_SIZE_MAX 400
#define WP_SIZE_DEFAULT 20              /* 10 pt */
#define WP_INDENT_MAX 31680             /* 22 inches */
#define WP_SOFT_BREAK '\v'
#define WP_PIC_MARK "\xef\xbf\xbc"      /* U+FFFC: one picture or object in the text */
#define WP_PIC_MARK_LEN 3
#define WP_PICS_MAX 2048                /* pictures per document */
#define WP_PIC_BYTES (96u * 1024u * 1024u)   /* picture and object bytes per document */

enum wp_family { WP_SANS, WP_SERIF, WP_MONO, WP_FAMILIES };
enum { WP_BOLD = 1, WP_ITALIC = 2, WP_UNDERLINE = 4, WP_STRIKE = 8 };
enum wp_align { WP_LEFT, WP_CENTER, WP_RIGHT, WP_JUSTIFY };

struct wp_font { char name[32]; uint8_t family; };
/* colour 0 is "automatic" (black); pic is the 1-based picture of a U+FFFC character (0: text). Runs with a picture never merge. */
struct wp_cattr { uint8_t font, flags, color, pad; uint16_t size; uint16_t pic; };
struct wp_pattr {
    uint8_t align, bullet, ntabs, pad;
    int32_t left, first, right;       /* twips; first is relative to left (negative: hanging) */
    int32_t tabs[WP_TABS];            /* ascending stops from the left margin */
};
struct wp_run { uint32_t len; struct wp_cattr a; };
struct wp_para { uint32_t len; struct wp_pattr a; };   /* bytes including its "\n" */

/* A picture or an embedded object (RTF \pict and \object). The bytes are kept exactly as read so that a save writes the
 * same picture; the decoded pixels are a cache filled by the application on first display. Pictures are shared by
 * reference between a document, its undo snapshots and clipboard slices (the count is atomic). */
enum wp_pic_kind { WP_PIC_NONE, WP_PIC_PNG, WP_PIC_JPEG, WP_PIC_DIB, WP_PIC_WMF, WP_PIC_EMF, WP_PIC_PICT };
enum wp_obj_kind { WP_OBJ_NONE, WP_OBJ_EMB, WP_OBJ_LINK, WP_OBJ_AUTLINK };
enum { WP_PX_UNTRIED, WP_PX_OK, WP_PX_FAILED };
struct wp_pic {
    uint32_t refs;
    uint8_t kind, objkind, px_state, pad;
    uint16_t scalex, scaley;          /* percent, 1..1000 */
    uint32_t pw, ph;                  /* pixels when known (0: not yet) */
    int32_t goalw, goalh;             /* natural size in twips (0: the pixel size at 96 dpi) */
    uint8_t *data; uint32_t len;      /* the picture (for an object: its result picture) */
    char cls[32];                     /* object class */
    uint8_t *objdata; uint32_t objlen;   /* object data (\objdata) */
    uint32_t *px; uint32_t pxw, pxh;  /* decoded 0x00RRGGBB, filled by the application */
};

struct wp_doc {
    char *text; size_t len, cap;
    struct wp_run *runs; unsigned nruns, cruns;
    struct wp_para *paras; unsigned npara, cpara;
    struct wp_font fonts[WP_FONTS]; unsigned nfonts;
    uint32_t colors[WP_COLORS]; unsigned ncolors;   /* 0xRRGGBB; colors[0] is automatic */
    struct wp_pic **pics; unsigned npics, cpics; size_t pic_bytes;
    uint64_t revision;
};

/* Character and paragraph changes: a field applies when its mask bit is set. */
struct wp_cstyle { int font, size, color; unsigned set, clear; };   /* font/color -1 and size 0 keep */
enum { WP_P_ALIGN = 1, WP_P_BULLET = 2, WP_P_LEFT = 4, WP_P_FIRST = 8, WP_P_RIGHT = 16, WP_P_TABS = 32 };
struct wp_pstyle { unsigned mask; struct wp_pattr v; };

int wp_init(struct wp_doc *);                 /* empty document: Arial 10 pt, black, one empty paragraph */
void wp_free(struct wp_doc *);
int wp_reset(struct wp_doc *);                /* back to the state after wp_init */
int wp_copy(struct wp_doc *dst, const struct wp_doc *src);   /* dst is overwritten; src unchanged */

struct wp_cattr wp_default_cattr(void);
struct wp_pattr wp_default_pattr(void);
int wp_cattr_eq(const struct wp_cattr *, const struct wp_cattr *);
int wp_pattr_eq(const struct wp_pattr *, const struct wp_pattr *);

/* Font table: the three installed families plus the names a document brings. */
int wp_font_find(const struct wp_doc *, const char *name);                    /* case-insensitive, -1 if absent */
int wp_font_add(struct wp_doc *, const char *name, unsigned family);          /* index, or -1 when the table is full */
unsigned wp_family_for_name(const char *name);                                /* Arial/Tahoma/... -> sans, Times/Georgia -> serif, Courier/Consolas -> mono */
int wp_color_find(const struct wp_doc *, uint32_t rgb);
int wp_color_add(struct wp_doc *, uint32_t rgb);                              /* nearest existing entry when full */

/* Positions are byte offsets on UTF-8 boundaries. */
size_t wp_prev(const struct wp_doc *, size_t at);
size_t wp_next(const struct wp_doc *, size_t at);
unsigned wp_para_at(const struct wp_doc *, size_t at, size_t *start, size_t *end);   /* end excludes the "\n" */
struct wp_cattr wp_attr_at(const struct wp_doc *, size_t at);                  /* the typing attribute at at: the previous character's, the first at 0 */
struct wp_cattr wp_attr_of(const struct wp_doc *, size_t at);                  /* the attribute of the character starting at at (the one after) */
struct wp_pattr wp_pattr_at(const struct wp_doc *, size_t at);

int wp_insert(struct wp_doc *, size_t at, const char *s, size_t n, const struct wp_cattr *attr);   /* attr NULL: wp_attr_at; "\n" splits the paragraph, copying its attributes */
int wp_append(struct wp_doc *, const char *s, size_t n, const struct wp_cattr *attr);   /* wp_insert at the end in O(1) per call (readers) */
int wp_delete(struct wp_doc *, size_t from, size_t to);       /* joined paragraphs keep the attributes of the paragraph whose mark survives */
int wp_style_chars(struct wp_doc *, size_t from, size_t to, const struct wp_cstyle *);
int wp_style_paras(struct wp_doc *, size_t from, size_t to, const struct wp_pstyle *);
/* Attributes shared by the whole range, for the format bar: a field is -1 (mixed) or its value. */
struct wp_cmix { int font, size, color, bold, italic, underline, strike; };
void wp_char_summary(const struct wp_doc *, size_t from, size_t to, struct wp_cmix *);
struct wp_pmix { int align, bullet; };
void wp_para_summary(const struct wp_doc *, size_t from, size_t to, struct wp_pmix *);
/* A slice as a standalone document (rich copy): fonts and colours are carried over. */
int wp_slice(struct wp_doc *dst, const struct wp_doc *src, size_t from, size_t to);
/* Inserts a slice, remapping its fonts and colours into dst. */
int wp_insert_slice(struct wp_doc *, size_t at, const struct wp_doc *slice);

/* ---- pictures ---------------------------------------------------------------- */
struct wp_pic *wp_pic_new(unsigned kind, const void *data, size_t len);        /* copies data; refs 1; NULL when out of memory */
void wp_pic_ref(struct wp_pic *);
void wp_pic_unref(struct wp_pic *);
/* Displayed size in twips: the natural size (goal, else the pixels at 96 dpi, else 1 inch) scaled by scalex/scaley. */
void wp_pic_size(const struct wp_pic *, int *w_twips, int *h_twips);
/* Inserts a picture character at `at`. Takes over the caller's reference on success and on failure alike. attr NULL: the typing attribute. */
int wp_insert_pic(struct wp_doc *, size_t at, struct wp_pic *, const struct wp_cattr *attr);
int wp_pic_attach(struct wp_doc *, struct wp_pic *);                           /* registers a picture (taking the caller's reference): its 1-based id, or -ENOMEM/-EFBIG */
uint8_t *wp_dib_to_bmp(const uint8_t *dib, size_t len, size_t *bmp_len);       /* a BMP file image from a DIB (for decoders that read files); malloc'd */
struct wp_pic *wp_pic_at(const struct wp_doc *, size_t at);                    /* the picture of the character starting at `at`, or NULL */
unsigned wp_pic_count_used(const struct wp_doc *);                              /* pictures still referenced by a character */
/* Image header sniffing: kind and pixel size of PNG, JPEG and DIB bytes (0: unknown). */
unsigned wp_pic_sniff(const uint8_t *data, size_t len, uint32_t *w, uint32_t *h);
/* A DIB (BITMAPINFOHEADER and bits, no file header) from a BMP file's bytes; NULL when it is not a plain BMP. malloc'd. */
uint8_t *wp_bmp_to_dib(const uint8_t *file, size_t len, size_t *dib_len);

/* Converts a UTF-8 text of n bytes: drops CR of CRLF, other control characters except "\t" and "\n"; invalid bytes become U+FFFD. */
int wp_sanitize(const char *in, size_t n, char **out, size_t *len);

/* ---- Files ---------------------------------------------------------------- */
struct wp_report {
    unsigned skipped_objects, skipped_pictures, unknown_charset, kept_pictures, kept_objects, unsupported_pictures;
    unsigned ignored_parts;       /* Word: footnote, header, annotation, text box subdocuments that are not shown */
    unsigned version;             /* Word: 6, 95 or 97 (97 and later); Write: 3 */
    const char *reason;           /* why a file was refused (-ENOTSUP, -EINVAL), or NULL */
};
int wp_is_rtf(const char *buf, size_t n);                                      /* starts with "{\rtf" */
int wp_rtf_read(struct wp_doc *, const char *buf, size_t n, struct wp_report *);  /* 0, -EINVAL (not RTF), -EFBIG, -ENOMEM */
int wp_rtf_write(const struct wp_doc *, char **out, size_t *n);               /* malloc'd; free(*out) */
/* Word 6, 95, 97 and later .doc files (OLE2 compound files with a WordDocument stream) and Write .wri files: text with character
 * and paragraph formatting, fonts, colours, tab stops, bullets and bitmap pictures. 0, -EINVAL (not a valid file), -ENOTSUP
 * (encrypted, or a version before Word 6), -EFBIG, -ENOMEM; rep->reason says why when it is set. */
int wp_is_word(const char *buf, size_t n);                                     /* an OLE2 file */
int wp_word_read(struct wp_doc *, const uint8_t *buf, size_t n, struct wp_report *);
int wp_is_write(const char *buf, size_t n);                                    /* wIdent 0xBE31 or 0xBE32 and wTool 0xAB00 */
int wp_write_read(struct wp_doc *, const uint8_t *buf, size_t n, struct wp_report *);
int wp_text_read(struct wp_doc *, const char *buf, size_t n);                  /* UTF-8 (BOM allowed) or Windows-1252, CRLF/CR/LF */
int wp_text_write(const struct wp_doc *, char **out, size_t *n, int crlf);     /* UTF-8 */
/* Windows-1252 byte 0x80..0x9f to Unicode (0 for the five undefined bytes). */
uint32_t wp_cp1252(unsigned byte);
unsigned wp_utf8_put(char *out, uint32_t cp);                                  /* 1..4 bytes */
#endif
