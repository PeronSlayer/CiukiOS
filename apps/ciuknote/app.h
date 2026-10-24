/* SPDX-License-Identifier: MIT
 * CiukNote application logic on libdeskui: menus, dialogs, find/replace bar and
 * the document commands. The window, the real file system and the clock are
 * supplied by the host (main.c, or the host tests), as for Files. */
#ifndef CIUKI_CIUKNOTE_APP_H
#define CIUKI_CIUKNOTE_APP_H
#include "doc.h"
#include "../libdeskui/deskui.h"
#include "../libdeskui/textarea.h"

/* Command, widget and dialog identifiers (shared with the host tests). */
enum { M_NEW = 1, M_OPEN, M_SAVE, M_SAVEAS, M_EXIT, M_UNDO, M_CUT, M_COPY, M_PASTE, M_DELETE, M_FIND,
       M_FINDNEXT, M_REPLACE, M_GOTO, M_SELALL, M_TIME, M_WRAP, M_FONT, M_STATUS, M_HELP, M_ABOUT };
enum { W_TEXT = 100, W_STATUS, W_FLABEL, W_FIND, W_FNEXT, W_FDIR, W_FCASE, W_FCLOSE,
       W_RLABEL, W_REPL, W_RONE, W_RALL };
enum { D_CHANGES = 200, D_INFO, D_GOTO, D_FONT, D_CREATE, D_OPEN, D_SAVE };

enum cn_after { CN_AFTER_NONE, CN_AFTER_NEW, CN_AFTER_OPEN, CN_AFTER_EXIT, CN_AFTER_DROP };
/* Test-mode checkpoints (the frame after the event is presented, then reported). */
enum { CN_CP_READY = 1, CN_CP_SAVED, CN_CP_OPENED, CN_CHECKPOINTS = 3 };

struct cn_app {
    /* host boundary */
    struct du_ui *ui; const struct cn_fs *fs;
    time_t (*now)(void *);                          /* UTC seconds */
    void (*quit)(void *);                           /* ask the host to close the window */
    void (*checkpoint)(struct cn_app *, int stage); /* test mode only */
    int (*portrait)(uint32_t *pixels256);           /* the approved portrait (XRGB 256x256) */
    void (*recent)(const char *path);               /* recent documents list (libdeskui recent.h); NULL in tests */
    void (*declined)(void *);                       /* G3: the user cancelled an exit (decline the close / veto the session) */
    void *host;
    int test, stage;
    /* document */
    struct du_textarea ta;
    uint64_t ui_revision; size_t ui_cursor,ui_anchor; struct du_rect ui_caret;
    char path[CN_PATH]; int named, crlf;
    /* view and command state */
    int show_status, find_mode, match_case, find_up;
    char note[96];
    enum cn_after after;
    char dir[CN_PATH];         /* last folder of the file dialogs */
    char pending[CN_PATH];     /* a file named on the command line that does not exist yet (create prompt) */
    char dropped[CN_PATH];     /* fx-19: a file dropped on the window, opened after the save question */
    uint32_t ciuki32[1024]; int ciuki;
    /* menus */
    struct du_menu menus[5];
    struct du_menu_item file_items[6], edit_items[14], format_items[2], view_items[1], help_items[3], popup[6];
};

/* Builds menus and widgets on an initialised UI (call after du_window_open/du_init). */
int cn_attach(struct cn_app *, struct du_ui *);
void cn_detach(struct cn_app *);
void cn_event(struct du_ui *, const struct du_event *, void *);
/* Command-line argument: opens the file, or offers to create it. */
int cn_open_bytes(struct cn_app *, const char *path, const char *, size_t);
int cn_open_argument(struct cn_app *, const char *path);
/* A window close request: returns 1 to close now, 0 when a prompt was shown instead. */
int cn_close_request(struct cn_app *);
void cn_refresh(struct cn_app *);
void cn_layout(struct cn_app *);
uint32_t cn_digest(const struct du_ui *);  /* FNV-1a over the surface pixels (little-endian words) */
/* Describes checkpoint `stage` for the serial log (test mode, after its frame was rendered). */
void cn_describe(const struct cn_app *, int stage, char *line, size_t cap);
#endif
