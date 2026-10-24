/* SPDX-License-Identifier: MIT
 * Files: Windows 98 class file manager for CiukiOS (f2-31). The controller
 * is independent of the desktop channel so host tests can drive it. */
#ifndef CIUKI_FILES_APP_H
#define CIUKI_FILES_APP_H
#include "model.h"
#include "operations.h"
#include <ciuki/abi.h>
#include "../libdeskui/deskui.h"

#define FILES_PLACES 12
#define FILES_TREE 256
#define FILES_CACHE 64

struct files_row { char label[80], size[32], type[40], modified[24], attributes[8], location[FM_PATH]; };
/* G2: the Recycle Bin view's extra columns. */
struct files_bin_row { char name[FM_NAME], where[RB_PATH], deleted[24]; };
struct files_place { char name[64], path[FM_PATH]; };
struct files_node { char name[FM_NAME], path[FM_PATH]; unsigned depth; uint8_t expanded, children; };

/* Cheap label sources observed on media ticks, never selection/layout. */
struct files_label_key {
    char volume[33];
    uint64_t size, ino;
    int64_t modified;
    int inf_status;
};
struct files_drive_label {
    uint32_t generation;
    int valid;
    char text[33];
    struct files_label_key key;
};
struct files_app {
    unsigned job_publication;
    uint64_t job_ui_ns;
    struct files_worker *worker;
    struct files_transfer *transfer;
    uint64_t media_poll_ns;
    int media_polled, poll_clipboard;
    int loading, reading, games_available, select_created, refresh_checkpoint, pending_command;
    char loading_path[FM_PATH];
    struct du_ui *ui;
    struct fm_model model;
    struct fm_job job;
    struct du_item *rows; struct files_row *texts; unsigned capacity;
    struct files_place places[FILES_PLACES]; struct du_item place_items[FILES_PLACES]; unsigned place_count;
    struct files_node *tree; struct du_item *tree_items; unsigned tree_count;
    char expanded[FILES_CACHE][FM_PATH]; unsigned expanded_count;
    char collapsed[FILES_CACHE][FM_PATH]; unsigned collapsed_count;
    char leaf[FILES_CACHE][FM_PATH]; unsigned leaf_count;
    uint32_t ciuki16[256], ciuki32[1024]; int ciuki;
    int width, height, split, test, stage, rename_index, rename_new, job_active, close_pending, context_folder, tree_dirty;
    char note[160], rename_original[FM_NAME];
    struct du_menu menus[6];
    struct du_menu_item tools_items[2];
    struct fm_search_query find_edit;
    int find_page, find_initial, find_again, find_running;
    char find_from[16], find_to[16], find_size[32];
    struct du_menu_item file_items[20], edit_items[12], view_items[13], go_items[14], help_items[3];
    struct du_menu_item popup[64]; unsigned popup_count;
    struct files_operations *operations;
    int shell_action; char shell_action_path[FM_PATH];
    char **shell_paths; unsigned shell_count;
    int (*shell_open)(struct files_app *, const char *);
    int (*shell_record)(struct files_app *, const struct desk_launch_record *);
    int (*spawn)(struct files_app *, const char *program, char *const argv[]);
    void (*close)(struct files_app *);
    void (*close_declined)(struct files_app *);
    void (*checkpoint)(struct files_app *, int stage);
    int (*portrait)(uint32_t *pixels256);
    void (*drop)(struct files_app *, int x, int y, int copy);
    /* fx-19 drag and drop: source (drag_begin queues DRAG_BEGIN after the
     * data object is saved) and target (drag_status answers DRAG_OVER). */
    int (*drag_begin)(struct files_app *, int button, unsigned count);
    void (*drag_status)(struct files_app *, uint32_t serial, unsigned effect, unsigned allowed);
    uint32_t drag_serial; char **drag_paths; char *drag_types; unsigned drag_count;
    int drag_widget, drag_index, drag_hover_node; uint64_t drag_hover_ns, drag_refresh_until;
    unsigned drag_effect, drag_allowed;
    uint8_t *band_original, *band_hits; unsigned band_capacity, band_modifiers;
    struct ciuki_media_state media;
    struct files_drive_label drive_labels[26];
    int (*media_label)(unsigned drive, char out[33]);
    int (*media_label_key)(unsigned drive, struct files_label_key *);
    int (*media_read)(struct ciuki_media_state *);
    int (*media_remove)(unsigned drive, uint32_t generation);
    int (*media_blockers)(unsigned drive, uint32_t generation, struct ciuki_media_blockers *);
    /* fx-17b: read the diskette in floppy drive A:/B: (MEDIA_FLOPPY_LOAD);
     * 0 mounted, -ENXIO no disk. space: volume capacity and free count. */
    int (*media_load)(unsigned drive);
    int (*media_space)(unsigned drive, struct ciuki_media_space *);
    int (*media_format)(int write, struct ciuki_media_format *);
    struct files_disk *disk;
    struct files_maintenance *maintenance;
    int (*media_defrag)(int step, struct ciuki_media_defrag *);
    int (*media_convert)(int execute, struct ciuki_media_convert *);
    const struct fm_vfs *cleanup_fs;
    int disk_pending, disk_drive;
    uint32_t disk_low_seen;
    int (*file_info)(const char *, struct ciuki_file_info *);
    int (*set_attributes)(const char *, uint32_t);
    int (*set_label)(unsigned, const char *);
    int (*version_info)(const char *, char *, size_t);
    char property_version[768];
    int (*volume_label)(unsigned, char out[33]);
    struct ciuki_file_info property_info;
    int property_attributes[4];
    int property_have_info, property_have_version;
    uint32_t property_common, property_either;
    int view_loaded;
    struct ciuki_media_space property_space;
    int property_drive, property_have_space, property_options, attribute_column;
    char property_text[DU_DIALOG_TEXT], property_label[12], property_request[FM_PATH];
    char insert_path[1040]; int insert_drive;
    char removal_label[96];
    /* G2 Recycle Bin: the policy read when a delete starts, the rows of the
     * bin view, its File menu, the Properties dialog's edit state. */
    struct rb_policy policy, bin_edit;
    struct files_bin_row *bin_rows; unsigned bin_row_capacity;
    struct du_menu_item bin_items[8];
    int bin_menu, bin_scope, delete_flags, show_bin_properties, bin_op;
    char **drop_paths; unsigned drop_count;   /* items dropped on the bin window, awaiting the confirmation */
    void *user;
};
/* G8 shell actions can be requested by desktop icons without duplicating UI. */

void files_shell_action(struct files_app *, int action, const char *path);

void files_properties_path(struct files_app *, const char *);
/* G29: asynchronous disk dialogs; drive is zero-based (C: = 2). */
int files_disk_open(struct files_app *, unsigned drive, int format);
int files_disk_busy(const struct files_app *);
void files_drop(struct files_app *, int x, int y, int copy);
/* fx-19 drop target and source, driven by the desktop's drag session.
 * keys are FM_KEY_* bits; phase 0 enter/over/repeat, 1 leave. */
void files_drag_over(struct files_app *, uint32_t serial, int phase, int x, int y, unsigned keys);
void files_drag_drop(struct files_app *, uint32_t serial, int x, int y, unsigned effect);
void files_drag_end(struct files_app *, unsigned effect);
int files_setup(struct files_app *, struct du_ui *, const struct fm_vfs *, const char *path);
void files_destroy(struct files_app *);
void files_event(struct du_ui *, const struct du_event *, void *);
int files_close_request(struct files_app *);
/* The desktop refused or failed to start the program requested through LAUNCH (error is positive). */
void files_find(struct files_app *);
void files_launch_failed(struct files_app *, int error);
void files_layout(struct files_app *);
void files_update(struct files_app *);
int files_navigate(struct files_app *, const char *);
void files_size_text(char *out, size_t n, uint64_t bytes);
int files_maintenance_open(struct files_app *, unsigned drive, int convert);
#endif
