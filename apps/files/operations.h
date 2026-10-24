/* SPDX-License-Identifier: MIT -- internal shell/file-manager services. */
#ifndef CIUKI_FILE_OPERATIONS_H
#define CIUKI_FILE_OPERATIONS_H
#include "model.h"
#include "../desktop/shell_link.h"
#define FM_UNDO_FILE "/system/settings/files.undo"
#define FM_SENDTO "/Documents/SendTo"
enum { FILES_UNDO=340, FILES_COPY_TO, FILES_MOVE_TO, FILES_SEND_TO, FILES_SHORTCUT,
       FILES_PASTE_SHORTCUT, FILES_NEW_SHORTCUT, FILES_NEW_BITMAP, FILES_OPEN_WITH,
       FILES_TYPES, FILES_NEW_REGISTERED };
struct fm_undo_entry { char source[FM_PATH], target[FM_PATH]; uint64_t stamp; };
struct fm_undo_log { unsigned kind, count; struct fm_undo_entry *entries; };
int fm_undo_supported(const struct fm_vfs *);
void fm_undo_free(struct fm_undo_log *);
int fm_undo_load(const struct fm_vfs *, struct fm_undo_log *);
int fm_undo_start(struct fm_job *, const struct fm_vfs *);
int fm_rename_undo(struct fm_model *, unsigned, const char *);
int fm_write_new(const struct fm_vfs *, const char *, const void *, unsigned);
int fm_bitmap(struct fm_model *, const char *);
int fm_shortcut(struct fm_model *, const struct desk_launch_record *, char out[FM_PATH]);
int fm_link_read(const struct fm_vfs *, const char *, struct desk_launch_record *);
int fm_link_decode(const uint8_t *, unsigned, const char *source, struct desk_launch_record *);
int fm_link_selection(struct fm_model *, char *const *, unsigned);
/* Shared HKCR, via the existing w32srv copied RPC. NULL in a model-only host
 * harness; production entry points install their process's transport. */
extern uint32_t (*fm_registry_exchange)(const void *, uint32_t, void *, uint32_t, uint32_t *);
int fm_registry_get(const char *key, const char *name, char *out, unsigned cap);
int fm_registry_set(const char *key, const char *name, const char *value);
int fm_registry_enum(const char *key, unsigned index, char *out, unsigned cap);
void fm_registered_types(struct fm_entry *, unsigned);
struct fm_filetype {
    char extension[64], progid[260], description[260], icon[260], verb[64];
    char commands[3][1024]; /* open, edit, print */
    uint32_t edit_flags;
};
int fm_filetype_get(struct fm_filetype *);
int fm_filetype_set(const struct fm_filetype *);
int fm_associate(const char *path, const char *program);
int fm_association(const char *path, const char *verb, struct desk_launch_record *);
/* 0 program/document/URL, 1 folder, negative errno. Re-read links on use. */
int fm_launch_resolve(const struct fm_vfs *, const char *path, const char *verb, struct desk_launch_record *);
int fm_command_record(const char *, const char *, struct desk_launch_record *);
int fm_launch_arguments(char *, char **, unsigned);
int fm_shellnew(struct fm_model *, const char *extension, char out[FM_PATH]);
int fm_shell_selection_write(const struct fm_vfs *, char *const *, unsigned, char out[FM_PATH]);
int fm_shell_selection_read(const struct fm_vfs *, const char *, char ***, unsigned *);
#endif
