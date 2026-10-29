/* SPDX-License-Identifier: MIT -- private Files view and address state. */
#ifndef CIUKI_FILES_NAVIGATION_H
#define CIUKI_FILES_NAVIGATION_H
#include "model.h"
#define FM_ADDRESSES 25
#define FM_FOLDER_VIEWS 64
#define FM_VIEW_COLUMNS 7
struct fm_folder_view {
    char path[FM_PATH];
    unsigned search, auto_arrange;
    int widths[FM_VIEW_COLUMNS]; /* Name, Size, Type, Modified, Attributes, Location; bin uses its own order */
};
struct fm_navigation {
    unsigned count, folders;
    char addresses[FM_ADDRESSES][FM_PATH]; /* newest first, canonical paths */
    struct fm_folder_view folder[FM_FOLDER_VIEWS];
};
struct fm_completions { unsigned count; char paths[FM_ADDRESSES][FM_PATH]; };
struct fm_folder_view *fm_folder_view(struct fm_navigation *, const char *, int search);
void fm_address_visit(struct fm_navigation *, const char *);
int fm_navigation_load(const struct fm_vfs *, struct fm_navigation *);
int fm_navigation_save(const struct fm_vfs *, const struct fm_navigation *);
int fm_complete(const struct fm_model *, const char *, struct fm_completions *);
int fm_address_resolve(const struct fm_model *, const char *, char *, int *directory);
int fm_extension_changes(const struct fm_model *, unsigned, const char *);
#endif
