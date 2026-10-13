/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_DESKTOP_FOLDER_H
#define CIUKI_DESKTOP_FOLDER_H
#include "../files/model.h"
#define DESK_FOLDER "/Documents/Desktop"
#define DESK_TRASH "/Documents/Trash"
enum { DF_NONE, DF_REFRESH, DF_FOLDER, DF_TEXT, DF_BITMAP, DF_RENAME, DF_DELETE, DF_PASTE, DF_CUT, DF_COPY, DF_PROPERTIES, DF_EMPTY };
struct desk_folder {
    struct fm_model model;
    struct fm_job job;
    int busy, command, index, ready, editing, select_all;
    unsigned cursor;
    char edit[FM_NAME];
    uint64_t poll_ns;
};
struct desktop;
int desk_folder_init(struct desktop *, const struct fm_vfs *);
void desk_folder_destroy(struct desktop *);
void desk_folder_service(struct desktop *);
void desk_folder_rename(struct desktop *, int);
void desk_folder_drop(struct desktop *, int source, int copy);
void desk_folder_changed(struct desktop *);
#endif
