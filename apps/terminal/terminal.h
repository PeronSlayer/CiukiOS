/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_TERMINAL_H
#define CIUKI_TERMINAL_H
#include "../libdeskui/deskui.h"
#include "../files/model.h"
#include <time.h>
#define TERM_HISTORY 200
#define TERM_SCROLLBACK 1000
#define TERM_LINE 1024
#define TERM_ARGS 32
#define TERM_CAPTURE (1024u * 1024u)
#define TERM_CELL_W 7
#define TERM_CELL_H 16
#define TERM_WIDTH (80 * TERM_CELL_W + 24)
#define TERM_HEIGHT (25 * TERM_CELL_H + 8)
struct lua_State;
struct terminal;
/* Providers return 0 or -errno, except processes (record count). Requests
 * return queue acceptance; desktop results are delivered separately. */
struct term_host {
    struct fm_vfs fs;
    void *ctx;
    int (*net)(void *, struct ciuki_net_state *);
    int (*network_command)(struct terminal *, int, char **);
    int (*mixer)(void *, struct ciuki_mixer_master *);
    int (*volume)(void *, uint32_t, uint32_t, uint32_t);
    int (*processes)(void *, void *, unsigned);
    int (*display)(void *, struct ciuki_display_info *);
    int (*launch)(void *, const char *, const char *);
    int (*kill)(void *, uint32_t);
    int (*power)(void *, unsigned);
    int (*cwd)(void *, const char *);
    time_t (*now)(void *);
    void (*pump)(void *);
    void (*quit)(void *);
};
struct terminal {
    struct term_host host;
    struct du_ui *ui;
    struct lua_State *lua;
    char cwd[FM_PATH], input[DU_TEXT_MAX], draft[DU_TEXT_MAX];
    char prompt_text[2 * DU_TEXT_MAX + 8], scratch[4][FM_PATH];
    size_t cursor, anchor;
    struct du_selection text_selection;
    char *history[TERM_HISTORY]; unsigned history_count, history_at;
    char *lines[TERM_SCROLLBACK]; unsigned first, count;
    char pending[TERM_LINE + 1]; unsigned used;
    unsigned columns, rows, top;
    unsigned view_top, output_serial, updated_serial;
    int follow, busy, interrupted, lua_mode, quit, history_error;
    char *chunk; size_t chunk_length;
    char *capture; size_t captured; int capture_error;
    int selecting, output_focus; size_t selection_a, selection_b;
    unsigned pending_power;
    struct du_menu_item menu_items[3]; struct du_menu menu;
};
int term_network_command(struct terminal *, int, char **);
int term_attach(struct terminal *, struct du_ui *, const struct term_host *);
void term_detach(struct terminal *);
void term_write(struct terminal *, const char *);
void term_printf(struct terminal *, const char *, ...);
int term_execute(struct terminal *, const char *);
char *term_capture(struct terminal *, const char *, int *);
int term_path(struct terminal *, const char *, char *);
unsigned term_visual_rows(const struct terminal *);
void term_row(const struct terminal *, unsigned, char *, size_t);
void term_submit(struct terminal *);
void term_complete(struct terminal *);
void term_event(struct du_ui *, const struct du_event *, void *);
int term_history_load(struct terminal *);
int term_history_save(struct terminal *);
int term_lua(struct terminal *, const char *, int, char **);
int term_lua_line(struct terminal *, const char *);
void term_lua_close(struct terminal *);
#endif
