/* Private layout shared by progman.c and restart.c. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32SRV_SHELL_INTERNAL_H
#define W32SRV_SHELL_INTERNAL_H
#include "shell.h"
#define WS_SESSIONS 16u
#define WS_PATH 1024u
struct ws_shell {
    struct ws_fs fs;
    char programs[WS_PATH], windows[WS_PATH];
    struct wr_server *registry;
    struct { uint32_t pid, id; uint64_t used; } sessions[WS_SESSIONS];
    char active_group[64]; /* Program Manager's shared focused group (SDK chapter 17) */
    /* sec-user-05: programs is the session user's Start Menu\Programs,
     * common the All Users one (C:\WINDOWS\Start Menu\Programs); the
     * focused group lives in common when active_common is set. */
    char common[WS_PATH];
    int administrator, active_common;
    /* Restart state (restart.c). line counts the [rename] lines already
     * applied and journalled; handed counts RunOnce entries given out. */
    unsigned phase, line, handed;
    uint32_t flags;
    int restart_error, exhausted, pending_run;
    uint32_t pending_type;
    char pending_command[2048];
    int resumed;      /* a journal from an interrupted run was found at start */
    uint8_t *wininit; /* WININIT.INI bytes while the WININIT pass runs */
    size_t wininit_size, cursor;
    unsigned cursor_line, section; /* applicable lines passed; 0 none, 1 [rename], 2 [DIRNUL] */
    int wininit_loaded;
};
void ws_log(struct ws_shell *, const char *fmt, ...);
/* "C:\X" -> "/X"; "X" (relative) -> "<windows>/X"; other drives refused. */
int ws_native(const struct ws_shell *, const char *windows_path, char *out, size_t cap);
int ws_restart_recover(struct ws_shell *);
int ws_link_store(struct ws_shell *, const char *native_path, const struct ws_link *);
#endif
