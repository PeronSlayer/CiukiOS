/* SPDX-License-Identifier: MIT
 * WordPad (G28): a rich text editor on libdeskui. RTF and text files, fonts,
 * character and paragraph formatting, bullets, tab stops, a ruler, Find and
 * Replace, Page Setup and print preview. The window, the real file system, the
 * clock and the fonts are supplied by the host (main.c and fonts.c, or the
 * host tests), as for CiukNote. */
#ifndef CIUKI_WORDPAD_APP_H
#define CIUKI_WORDPAD_APP_H
#include "rich.h"
#include "layout.h"
#include "../ciuknote/doc.h"
#include "../libdeskui/deskui.h"
#include "../libprint/print.h"
#include <time.h>

#define WP_FILE_MAX (16u * 1024u * 1024u)     /* largest file read */

/* Picture decoding and encoding (image.c in the application, a fake in tests). */
struct wp_image_ops {
    int (*decode)(unsigned kind, const uint8_t *data, size_t len, uint32_t **px, unsigned *w, unsigned *h);
    int (*png)(const uint32_t *px, unsigned w, unsigned h, uint8_t **out, size_t *len);
};
struct wp_draw_ops {
    struct wp_text_ops text;
    /* Draws UTF-8 text of n bytes with its baseline at `baseline`; em is the size in 26.6 pixels. */
    void (*draw)(void *ctx, struct du_canvas *, unsigned family, unsigned flags, unsigned em, int x, int baseline,
                 const char *s, size_t n, uint32_t rgb);
};

/* Commands (menu items and tool buttons; the same identifiers). */
enum { M_NEW = 1, M_OPEN, M_SAVE, M_SAVEAS, M_PRINT, M_PREVIEW, M_PAGESETUP, M_EXIT,
       M_UNDO, M_REDO, M_CUT, M_COPY, M_PASTE, M_CLEAR, M_SELALL, M_FIND, M_FINDNEXT, M_REPLACE,
       M_TOOLBAR, M_FORMATBAR, M_RULER, M_STATUS, M_DATETIME,
       M_FONT, M_BULLET, M_PARAGRAPH, M_TABS, M_BOLD, M_ITALIC, M_UNDERLINE, M_STRIKE, M_LEFT, M_CENTER, M_RIGHT,
       M_HELP, M_ABOUT, M_COLORMENU, M_FONTMENU, M_SIZEMENU,
       M_PV_NEXT, M_PV_PREV, M_PV_ZOOMIN, M_PV_ZOOMOUT, M_PV_CLOSE,
       M_INSERTOBJ, M_INSERTPIC, M_OPTIONS, M_JUSTIFY, M_PV_PRINT, M_CONTEXTHELP };
enum { W_DOC = 100, W_SCROLL, W_STATUS, W_TOOLBAR, W_FORMATBAR, W_FLABEL, W_FIND, W_FNEXT, W_FDIR, W_FCASE, W_FCLOSE,
       W_RLABEL, W_REPL, W_RONE, W_RALL, W_FONTBTN, W_SIZEBTN, W_COLORBTN, W_SEP1, W_SEP2, W_SEP3, W_SEP4,
       W_PVBAR, W_RULER, W_HSCROLL };
enum { D_CHANGES = 200, D_REPLACE, D_INFO, D_FONT, D_PARAGRAPH, D_TABS, D_PAGESETUP, D_DATETIME, D_OPEN, D_SAVE,
       D_TEXTWARN, D_NOTICE, D_INSERTOBJ, D_PICFILE, D_OPTIONS, D_SAVEIMPORT };
/* Popup commands: the font, size and colour menus. */
enum { P_FONT = 1000, P_SIZE = 1100, P_COLOR = 1200 };

enum wp_after { WP_AFTER_NONE, WP_AFTER_NEW, WP_AFTER_OPEN, WP_AFTER_EXIT, WP_AFTER_DROP };
/* WP_FMT_WORD and WP_FMT_WRITE: a .doc or .wri document that was imported; it is saved as Rich Text Format. */
enum wp_format { WP_FMT_RTF, WP_FMT_TEXT, WP_FMT_WORD, WP_FMT_WRITE, WP_FORMATS };
enum wp_wrap { WP_WRAP_WINDOW, WP_WRAP_NONE, WP_WRAP_RULER };
enum wp_unit { WP_UNIT_INCH, WP_UNIT_CM, WP_UNIT_POINT, WP_UNIT_PICA, WP_UNITS };
#define WP_UNDO_LEVELS 100
#ifndef WP_UNDO_BYTES
#define WP_UNDO_BYTES (64u * 1024u * 1024u)   /* text bytes held by the undo levels (host tests lower it) */
#endif
#define WP_SETTINGS "/system/settings/wordpad.cfg"
/* One undo or redo level: the document before the command and the selection to restore. */
struct wp_snap { struct wp_doc doc; size_t cursor, anchor; };
enum wp_edit { WP_EDIT_OTHER, WP_EDIT_TYPE, WP_EDIT_BACK, WP_EDIT_FORWARD };

struct wp_app {
    /* host boundary */
    struct du_ui *ui; const struct cn_fs *fs; const struct wp_draw_ops *draw;
    const struct wp_image_ops *images;              /* picture decoding and encoding; NULL: pictures show a placeholder */
    size_t (*format_time)(char *, size_t, const char *, const struct tm *);   /* strftime (the user's regional formats); NULL: strftime */
    void (*print)(void *host, const struct cp_page *initial);   /* opens the Print dialog for the document (G31); NULL: Print is unavailable */
    time_t (*now)(void *);                          /* UTC seconds */
    void (*quit)(void *);
    void (*declined)(void *);
    void (*recent)(const char *path);
    void *host;
    int a4;                                         /* the default paper */
    /* document */
    struct wp_doc doc, clip;
    struct wp_snap *undo_s, *redo_s; unsigned nundo, nredo, cundo, credo; size_t undo_bytes;   /* multi-level undo and redo stacks */
    int clip_valid;
    char *clip_plain;
    size_t cursor, anchor;
    struct wp_cattr pending; int pending_valid;     /* the format of the next typed character at a bare caret */
    enum wp_edit last_edit; size_t last_edit_end;
    char path[CN_PATH]; int named, modified, format, crlf;
    /* view */
    struct wp_layout lay; unsigned dirty_para; int top, left, want_x, want_valid;
    int show_toolbar, show_format, show_ruler, show_status;
    int wrap[WP_FORMATS], units;                    /* word wrap per document type, measurement units */
    int caret_on, doc_w, doc_h, hs_visible;
    struct { int kind, index, pos, active; struct wp_pattr orig; } rdrag;  /* ruler drag: marker, tab index, position in twips, the paragraph at its start */
    uint64_t click_ns; int click_x, click_y; unsigned clicks;
    int dragging; size_t drag_lo, drag_hi;
    struct du_rect doc_box, ruler_box;
    /* format bar state */
    struct wp_cmix cmix; struct wp_pmix pmix;
    char font_label[40], size_label[8];
    /* find */
    int find_mode, match_case, find_up;
    char note[96], filter[64], suggest[CN_PATH];
    const char *reason;                              /* why the last file could not be opened (a static string), or NULL */
    enum wp_after after; int after_set;
    char dir[CN_PATH], pending_path[CN_PATH], dropped[CN_PATH];
    /* page setup and preview */
    struct wp_page_setup page;
    int preview, preview_page, preview_zoom;
    struct wp_layout plo; struct wp_pages pages; int preview_valid;
    /* dialogs */
    struct du_item font_items[WP_FONTS], size_items[16], style_items[4], color_items[WP_COLORS], date_items[8], object_items[2];
    char color_names[WP_COLORS][20], date_text[8][64], dlg_edit[4][24];
    struct tm tm;
    struct du_item sheet_page;
    /* menus */
    struct du_menu menus[6];
    struct du_menu_item file_items[9], edit_items[13], view_items[6], insert_items[3], format_items[5], help_items[4];
    struct du_menu_item popup[8], list_popup[WP_COLORS + 1];
};

int wp_attach(struct wp_app *, struct du_ui *);
void wp_detach(struct wp_app *);
void wp_event(struct du_ui *, const struct du_event *, void *);
/* Opens a file (the command line, a dropped file); an error is reported in a dialog. */
int wp_open_path(struct wp_app *, const char *path);
int wp_open_bytes(struct wp_app *, const char *path, const char *raw, size_t n);
int wp_close_request(struct wp_app *);     /* 1 to close now, 0 when a prompt was shown */
void wp_layout_ui(struct wp_app *);
void wp_refresh(struct wp_app *);
uint32_t wp_digest(const struct du_ui *);
/* Printing: lays the document out for the paper and margins the user chose and describes every page (text, rules, pictures)
 * for the spooler (cp_prepare_fn of libprint/dialog.h, the application as ctx). */
int wp_print_prepare(void *app, const struct cp_page *page, unsigned driver, struct cp_doc *out);
/* Test hooks. */
size_t wp_cursor(const struct wp_app *);
#endif
