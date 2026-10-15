/* SPDX-License-Identifier: MIT
 * Files: Windows 98 class file manager for CiukiOS (f2-31). The controller
 * is independent of the desktop channel so host tests can drive it. */
#ifndef CIUKI_FILES_APP_H
#define CIUKI_FILES_APP_H
#include "model.h"
#include <ciuki/abi.h>
#include "../libdeskui/deskui.h"

#define FILES_PLACES 12
#define FILES_TREE 256
#define FILES_CACHE 64

struct files_row { char label[80], size[32], type[40], modified[24]; };
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
    struct du_menu menus[5];
    struct du_menu_item file_items[12], edit_items[6], view_items[10], go_items[13], help_items[3];
    struct du_menu_item popup[18]; unsigned popup_count;
    int (*spawn)(struct files_app *, const char *program, char *const argv[]);
    void (*close)(struct files_app *);
    void (*checkpoint)(struct files_app *, int stage);
    int (*portrait)(uint32_t *pixels256);
    void (*drop)(struct files_app *, int x, int y, int copy);
    struct ciuki_media_state media;
    struct files_drive_label drive_labels[26];
    int (*media_label)(unsigned drive, char out[33]);
    int (*media_label_key)(unsigned drive, struct files_label_key *);
    int (*media_read)(struct ciuki_media_state *);
    int (*media_remove)(unsigned drive, uint32_t generation);
    int (*media_blockers)(unsigned drive, uint32_t generation, struct ciuki_media_blockers *);
    char removal_label[96];
    void *user;
};

void files_drop(struct files_app *, int x, int y, int copy);
int files_setup(struct files_app *, struct du_ui *, const struct fm_vfs *, const char *path);
void files_destroy(struct files_app *);
void files_event(struct du_ui *, const struct du_event *, void *);
int files_close_request(struct files_app *);
/* The desktop refused or failed to start the program requested through LAUNCH (error is positive). */
void files_launch_failed(struct files_app *, int error);
void files_layout(struct files_app *);
void files_update(struct files_app *);
int files_navigate(struct files_app *, const char *);
void files_size_text(char *out, size_t n, uint64_t bytes);
#endif
