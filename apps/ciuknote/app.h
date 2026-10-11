/* SPDX-License-Identifier: MIT
 * CiukNote application logic on libdeskui: menus, dialogs, file dialog page,
 * find/replace bar and the document commands. The window, the real file system
 * and the clock are supplied by the host (main.c, or the host tests). */
#ifndef CIUKI_CIUKNOTE_APP_H
#define CIUKI_CIUKNOTE_APP_H
#include "doc.h"
#include "../libdeskui/deskui.h"
#include "../libdeskui/textarea.h"

#define CN_WIDTH 600
#define CN_HEIGHT 352

/* Command, widget and dialog identifiers (shared with the host tests). */
enum { M_NEW = 1, M_OPEN, M_SAVE, M_SAVEAS, M_EXIT, M_UNDO, M_CUT, M_COPY, M_PASTE, M_DELETE, M_FIND,
       M_FINDNEXT, M_REPLACE, M_GOTO, M_SELALL, M_TIME, M_WRAP, M_FONT, M_STATUS, M_HELP, M_ABOUT };
enum { W_TEXT = 100, W_NAME, W_POS, W_FLABEL, W_FIND, W_FNEXT, W_FDIR, W_FCASE, W_FCLOSE,
       W_RLABEL, W_REPL, W_RONE, W_RALL,
       W_PLABEL, W_PATH, W_LIST, W_SCROLL, W_NLABEL, W_NAMEEDIT, W_TLABEL, W_TYPE, W_OK, W_CANCEL, W_NOTE };
enum { D_CHANGES = 200, D_REPLACE, D_INFO, D_GOTO, D_FONT, D_CREATE };

enum cn_page { CN_PAGE_EDIT, CN_PAGE_OPEN, CN_PAGE_SAVE };
enum cn_after { CN_AFTER_NONE, CN_AFTER_NEW, CN_AFTER_OPEN, CN_AFTER_EXIT };
enum cn_checkpoint { CN_CP_NONE, CN_CP_READY, CN_CP_SAVED, CN_CP_OPENED };

struct cn_cells { char size[24], modified[24]; };

struct cn_app {
    /* host boundary */
    struct du_ui *ui; const struct cn_fs *fs;
    time_t (*now)(void *);     /* UTC seconds */
    uint64_t (*mono)(void *);  /* monotonic nanoseconds, for double-click timing */
    void (*quit)(void *);      /* ask the host to close the window */
    void *host;
    int test;                  /* --test: deterministic checkpoints (the host supplies the fixed clock) */
    /* document */
    struct du_textarea ta;
    char path[CN_PATH]; int named, crlf;
    char clip[CN_LIMIT + 1]; size_t clip_len; char mirror[DU_TEXT_MAX];
    /* view and command state */
    int show_status, find_mode, match_case, find_up;
    char note[96];
    enum cn_page page; enum cn_after after;
    char pending[CN_PATH];     /* a Save As target waiting for the replace answer */
    /* menus: labels are built once; items are updated by cn_refresh */
    struct du_menu menus[5];
    struct du_menu_item file_items[5], edit_items[11], format_items[2], view_items[1], help_items[2], context_items[6];
    char labels[32][32]; unsigned label_count; unsigned menu_keys[5]; char *wrap_label, *status_label;
    /* file dialog page */
    char dir[CN_PATH]; int text_filter; unsigned sort_column; int sort_descending;
    struct cn_listing listing; struct du_row *rows; struct cn_cells *cells; unsigned row_count; int has_parent;
    int last_click; uint64_t click_ns; char fd_note[160];
    /* test evidence */
    enum cn_checkpoint checkpoint;
};

/* Builds menus and widgets on an initialised UI (call after du_window_open/du_init). */
int cn_attach(struct cn_app *, struct du_ui *);
void cn_detach(struct cn_app *);
/* Command-line argument: opens the file, or offers to create it. */
int cn_open_argument(struct cn_app *, const char *path);
/* A window close request: returns 1 to close now, 0 when a prompt was shown instead. */
int cn_close_request(struct cn_app *);
void cn_refresh(struct cn_app *);
uint32_t cn_digest(const struct du_ui *);  /* FNV-1a over the surface pixels (little-endian words) */
/* Once the frame after a checkpoint event has been rendered, describes it in `line`. */
int cn_checkpoint(struct cn_app *, char *line, size_t cap);
#endif
