/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_DESKTOP_FOLDER_H
#define CIUKI_DESKTOP_FOLDER_H
#include "../files/model.h"
#include "../files/transfer.h"
#include "../libdeskui/deskui.h"
#define DESK_FOLDER "/Documents/Desktop"
#define DESK_TRASH "/Documents/Trash"
#define DESK_FOLDER_POLL_SLOTS (2u * FM_ENTRIES)
enum { DF_NONE, DF_REFRESH, DF_FOLDER, DF_TEXT, DF_BITMAP, DF_RENAME, DF_DELETE, DF_PASTE, DF_CUT, DF_COPY, DF_PROPERTIES, DF_EMPTY };
struct desk_folder {
    struct fm_model model;
    struct fm_job job;
    struct files_transfer *transfer;
    struct fm_model result;
    int result_error, clipboard_job, trash_job, dialog_window;
    enum fm_job_kind display_kind;
    char destination[FM_PATH];
    struct du_ui *dialog;
    uint32_t *dialog_pixels;
    uint64_t progress_ns;
    int busy, command, index, ready, editing, select_all;
    unsigned cursor;
    char edit[FM_NAME];
    uint64_t poll_ns;
    unsigned revision;
    struct folder_worker *worker;
};
struct desktop;
struct desk_box;
int desk_folder_init(struct desktop *, const struct fm_vfs *);
void desk_folder_destroy(struct desktop *);
void desk_folder_service(struct desktop *);
void desk_folder_rename(struct desktop *, int);
void desk_folder_drop(struct desktop *, int source, int copy);
void desk_folder_dialog_paint(struct desktop *, struct desk_box, struct desk_box);
void desk_folder_dialog_click(struct desktop *, int x, int y);
void desk_folder_dialog_key(struct desktop *, int code, int value);
void desk_folder_cancel(struct desktop *);
void desk_folder_changed(struct desktop *);
int desk_folder_busy(struct desktop *);
const char *desk_folder_activity(const struct desktop *);
#endif
