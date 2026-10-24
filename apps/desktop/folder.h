/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_DESKTOP_FOLDER_H
#define CIUKI_DESKTOP_FOLDER_H
#include "../files/operations.h"
#include "../files/transfer.h"
#include "../libdeskui/deskui.h"
#define DESK_FOLDER "/Documents/Desktop"
#define DESK_TRASH "/Documents/Trash"   /* the pre-G2 Trash folder: its items are migrated into C:\RECYCLED (RB_LEGACY_TRASH) */
#define DESK_FOLDER_POLL_SLOTS (2u * FM_ENTRIES)
enum { DF_NONE, DF_REFRESH, DF_FOLDER, DF_TEXT, DF_BITMAP, DF_RENAME, DF_DELETE, DF_PASTE, DF_CUT, DF_COPY, DF_PROPERTIES, DF_EMPTY, DF_UNDO, DF_SHORTCUT, DF_PASTE_SHORTCUT, DF_PASTE_INTO, DF_SHORTCUT_FINISH };
struct desk_folder {
    struct fm_model model;
    struct fm_job job;
    struct files_transfer *transfer;
    struct fm_model result;
    struct fm_model *picker;
    int wizard, wizard_window, wizard_focus, picker_scroll, picker_click;
    uint64_t picker_click_ns;
    int wizard_select_all;
    char shortcut_target[260], shortcut_args[1024], shortcut_name[FM_NAME];
    char picker_path[FM_PATH], wizard_error[160];
    int result_error, clipboard_job, dialog_window, undo_job;
    /* G2 Recycle Bin: the delete awaiting its confirmation, the policy read for it,
     * the running bin job (RB op) and the full/empty state of the icon. */
    struct rb_policy policy; char **pending; unsigned pending_count, delete_flags;
    int bin_op, bin_full, bin_full_job; unsigned command_flags;
    enum fm_job_kind display_kind;
    char destination[FM_PATH];
    struct du_ui *dialog;
    uint32_t *dialog_pixels;
    uint64_t progress_ns;
    int busy, command, index, ready, editing, select_all;
    int multi;                      /* fx-19: CUT/COPY/DELETE act on model.entries[].selected */
    int place; int32_t place_x, place_y; /* fx-19: position the dropped items at the drop point */
    unsigned cursor;
    char edit[FM_NAME];
    uint64_t poll_ns;
    unsigned revision;
    struct folder_worker *worker;
};
struct desk_launch_record;
struct desktop;
struct desk_box;
int desk_folder_init(struct desktop *, const struct fm_vfs *);
int desk_folder_init_async(struct desktop *, const struct fm_vfs *);
void desk_folder_destroy(struct desktop *);
void desk_folder_service(struct desktop *);
void desk_folder_rename(struct desktop *, int);
void desk_folder_drop(struct desktop *, int source, int copy);
/* G2: Delete, Shift+Delete (flags RB_PERMANENT) and drops on the Recycle Bin icon. Asks first unless the
 * policy turned the confirmation off; desk_folder_confirmed starts the job. Empty Recycle Bin likewise. */
int desk_folder_delete_request(struct desktop *, char *const *paths, unsigned count, unsigned flags);
void desk_folder_empty_request(struct desktop *);
void desk_folder_confirmed(struct desktop *, int action);
void desk_folder_cancelled(struct desktop *);
/* fx-19: copy or move dragged items into destination (FM_EFFECT_COPY or
 * FM_EFFECT_MOVE), or move them to the Trash with free names (trash=1);
 * place records the desktop drop point for the new icons. */
/* fx-19 drag data object through the folder's VFS (the shell does no I/O). */
int desk_folder_drag_save(struct desktop *, char *const *paths, const char *types, unsigned count);
int desk_folder_drag_load(struct desktop *, char ***paths, char **types, unsigned *count);
int desk_folder_transfer(struct desktop *, char *const *paths, unsigned count, const char *destination,
                         unsigned effect, int trash, int place, int32_t x, int32_t y);
void desk_folder_dialog_paint(struct desktop *, struct desk_box, struct desk_box);
void desk_folder_dialog_click(struct desktop *, int x, int y);
void desk_folder_dialog_key(struct desktop *, int code, int value);
void desk_folder_cancel(struct desktop *);
int desk_folder_link_read(struct desktop *, const char *, struct desk_launch_record *);
int desk_folder_link_create(struct desktop *, const char *, const char *, const struct desk_launch_record *);
void desk_folder_shortcut_begin(struct desktop *);
void desk_folder_shortcut_close(struct desktop *);
void desk_folder_shortcut_action(struct desktop *, unsigned);
void desk_folder_shortcut_key(struct desktop *, int, unsigned);
void desk_folder_shortcut_paint(struct desktop *, struct desk_box, struct desk_box);
void desk_folder_shortcut_click(struct desktop *, int, int);
void desk_folder_changed(struct desktop *);
int desk_folder_busy(struct desktop *);
const char *desk_folder_activity(const struct desktop *);
#endif
