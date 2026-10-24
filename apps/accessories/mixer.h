/* SPDX-License-Identifier: MIT
 * Volume Control: master and device playback/recording lines.
 * Reading uses the public read-only /dev/mixer;
 * changes go to the desktop (DESK_MIXER_SET), which holds the delegated
 * control handle and the kernel saves C:\SYSTEM\AUDIO.CFG. */
#ifndef CIUKI_ACC_MIXER_H
#define CIUKI_ACC_MIXER_H
#include "acc.h"
#include <ciuki/abi.h>
struct mixer_backend {
    int (*get)(void *ctx, struct ciuki_mixer_master *);                 /* 0 or -errno */
    int (*set)(void *ctx, uint32_t left, uint32_t right, uint32_t mute); /* 0 when sent, or -errno */
    void *ctx;
    int (*line_get)(void *,struct ciuki_mixer_line *);
    int (*line_set)(void *,const struct ciuki_mixer_line *);
};
/* Volume 0..100 and balance -100 (left) .. 100 (right) to channel levels and back. */
void mixer_split(int volume, int balance, uint32_t *left, uint32_t *right);
void mixer_join(uint32_t left, uint32_t right, int *volume, int *balance);
#define MIXER_W 624
#define MIXER_H 320
struct mixer_app {
    struct du_ui *ui; const struct mixer_backend *backend;
    struct ciuki_mixer_master master; int available, error;
    int volume, balance, mute, pending, waiting;
    char status[96];
    struct ciuki_mixer_line lines[6];
    int line_volume[6],line_balance[6],recording;
    unsigned line_pending;
    struct du_menu menus[2]; struct du_menu_item options_items[1], help_items[1];
};
enum { MX_M_REFRESH = 1, MX_M_ABOUT, MX_VOLUME = 100, MX_BALANCE, MX_MUTE, MX_LEVEL, MX_STATUS, MX_GROUP, MX_BGROUP,
       MX_LEFT, MX_RIGHT, MX_MODE = 140, MX_LINE_BASE = 200, MX_T_APPLY = 1, MX_T_POLL = 2 };
void mixer_attach(struct mixer_app *, struct du_ui *, const struct mixer_backend *);
void mixer_refresh(struct mixer_app *);                    /* reads the master record */
void mixer_result(struct mixer_app *, int result);         /* the desktop's answer to a change */
void mixer_event(struct du_ui *, const struct du_event *, void *);
#endif
