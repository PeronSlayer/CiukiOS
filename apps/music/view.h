/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_MUSIC_VIEW_H
#define CIUKI_MUSIC_VIEW_H
#include "player.h"
#include "../accessories/acc.h"
struct music_ui_backend {
    int (*send)(void *, const struct music_command *);
    int (*master_get)(void *, unsigned *volume);
    int (*master_set)(void *, unsigned volume);
    void (*quit)(void *);
    void *ctx;
};
struct music_app {
    struct du_ui *ui; const struct music_ui_backend *backend;
    struct music_snapshot snapshot; struct du_item rows[MUSIC_TRACKS];
    struct du_menu menus[3]; struct du_menu_item file_items[6], play_items[5], help_items[1];
    int volume, volume_error, waiting, local_error, seek_waiting;
    uint64_t seek_target;
};
enum { M_OPEN=1, M_ADD, M_FOLDER, M_SAVE, M_CLEAR, M_EXIT, M_PLAY, M_PAUSE, M_STOP, M_PREV, M_NEXT, M_ABOUT,
       M_TITLE=100, M_TIME, M_SEEK, M_VOLUME, M_LEVEL, M_LIST, M_STATUS,
       M_D_OPEN=200, M_D_ADD, M_D_FOLDER, M_D_SAVE, M_POLL, M_APPLY, M_SEEK_APPLY };
void music_attach(struct music_app *, struct du_ui *, const struct music_ui_backend *);
void music_update(struct music_app *, const struct music_snapshot *);
void music_event(struct du_ui *, const struct du_event *, void *);
#endif
