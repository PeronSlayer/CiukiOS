/* SPDX-License-Identifier: MIT
 * Files model: navigation, selection, sorting and incremental file jobs over
 * a replaceable VFS (POSIX in the guest, a fake tree in host tests). */
#ifndef CIUKI_FILES_MODEL_H
#define CIUKI_FILES_MODEL_H
#include <stddef.h>
#include <stdint.h>
#define FM_PATH 1040
#define FM_NAME 766
#define FM_ENTRIES 4096
#define FM_HISTORY 32
#define FM_DEPTH 32
#define FM_CHUNK 65536
#define FM_SORT_LOCATION 6u /* G7: after main's Attributes (4) and bin Date Deleted (5). */
#define FM_FIND_KEY 0x3d /* F3, AT set-1 key code; private Files/shell shortcut. */
/* fx-17: a copy reads FM_CHUNK at a time (cancel and progress stay
 * responsive) but writes up to FM_WRITE_BATCH per write(): each write() on
 * FAT is one durable growth group (data, chain, link, entry, each closed by
 * a device cache flush), so 64 KiB writes cost a stick 64 flushes per MiB
 * with 8 KiB clusters, 1 MiB writes four. */
#define FM_WRITE_BATCH (1024u * 1024u)
#define FM_CREATED 64
/* Empty is a virtual location, never a filesystem pathname. */
#define FM_COMPUTER ""
enum fm_type { FM_FILE, FM_DIR, FM_OTHER };
enum fm_kind { FM_KIND_FOLDER, FM_KIND_TEXT, FM_KIND_APP, FM_KIND_IMAGE, FM_KIND_AUDIO, FM_KIND_SCRIPT, FM_KIND_FILE };
struct fm_meta { enum fm_type type; uint64_t size, ino; uint32_t dev, mode; int64_t modified; uint32_t exec; uint32_t attributes; uint64_t allocated; int allocated_known; };
/* bin: 0, or index + 1 of the entry's record in fm_model.bin (G2: the Recycle
 * Bin view lists stored items under their stored names; the record has the
 * original name, location and deletion date). */
struct fm_entry { char name[FM_NAME]; struct fm_meta meta; unsigned selected; uint32_t bin; unsigned result; char registered_type[80]; };
typedef int (*fm_emit)(const char *, const struct fm_meta *, void *);
/* Cheap enumeration metadata (type/size/mtime/ino); METADATA fills missing
 * portable-host metadata without opening contents. SNIFF also classifies ELF.
 * Browser listings use NAMES_ONLY; transfer/counting passes use METADATA. */
enum { FM_LIST_NAMES_ONLY = 0, FM_LIST_SNIFF = 1u, FM_LIST_METADATA = 2u };
/* Every error is -errno; reads/writes return a positive short count or zero. */
struct fm_vfs {
    void *ctx;
    int (*list)(void *, const char *, unsigned flags, fm_emit, void *);
    int (*stat_path)(void *, const char *, struct fm_meta *);
    int (*mkdir)(void *, const char *);
    int (*rename)(void *, const char *, const char *);
    int (*unlink)(void *, const char *);
    int (*rmdir)(void *, const char *);
    int (*open)(void *, const char *, int); /* 0 read, 1 exclusive create */
    int (*read)(void *, int, void *, unsigned);
    int (*write)(void *, int, const void *, unsigned);
    int (*sync)(void *, int);
    int (*close)(void *, int);
    int (*metadata)(void *, const char *, const struct fm_meta *); /* optional: no setter in the F2 ABI */
};
/* G7: private Files search criteria, never a public ABI record. */
struct fm_search_query {
    char named[FM_NAME], containing[256], look_in[FM_PATH], type[80];
    int recursive, date_from_set, date_to_set, size_mode;
    int64_t date_from, date_to;
    uint64_t size_bytes;
};
struct rb_item;
struct rb_job;
struct fm_undo_log;
struct fm_model {
    struct fm_vfs fs; char path[FM_PATH], history[FM_HISTORY][FM_PATH]; unsigned history_count, history_at;
    struct fm_entry *entries; unsigned count, capacity, active, anchor, sort; int descending;
    char find[FM_NAME]; size_t find_length; uint64_t find_deadline;
    char **clipboard; unsigned clipboard_count; int cut;
    int show_hidden, hide_system, hide_extensions;
    /* G2: records behind the Recycle Bin view's entries, and the drives whose
     * volumes are fixed disks (bit 0 = A:; C: always counts). */
    struct rb_item *bin; unsigned bin_count; uint32_t fixed;
    struct fm_search_query query;
    int searching, search_incomplete, search_error;
    char (*result_paths)[FM_PATH]; unsigned result_count;
    uint32_t clipboard_sequence;
    uint32_t drive_slots; /* fx-17b: drives listed in Computer without a medium (floppies) */
};
enum fm_job_kind { FM_COPY, FM_MOVE, FM_DELETE, FM_PROPERTIES };
enum fm_job_state { FM_IDLE, FM_RUNNING, FM_REPLACE, FM_DONE, FM_CANCELLED, FM_FAILED };
enum fm_replace { FM_YES, FM_YES_ALL, FM_NO, FM_CANCEL };
struct fm_frame {
    char source[FM_PATH], target[FM_PATH]; struct fm_meta meta, existing;
    struct fm_entry *entries; unsigned count, next; int phase, approved, exists;
};
struct fm_job {
    struct fm_vfs fs; enum fm_job_kind kind; enum fm_job_state state;
    char **sources; unsigned count, next, depth, completed, skipped, metadata_warnings;
    char destination[FM_PATH], temporary[FM_PATH]; unsigned serial;
    int trash; char trash_name[FM_NAME]; /* trash 2: a free name per item (fx-19) */
    struct fm_frame frames[FM_DEPTH];
    int input, output, error, cleanup_error, yes_all, cancel, skip_seen, prompt_dir;
    unsigned used, written, chunk, batch; int eof; unsigned char *buffer; /* FM_WRITE_BATCH bytes while copying */
    uint64_t bytes, files, folders, total_bytes;
    uint64_t allocated; int allocated_unknown;
    uint64_t file_bytes;
    unsigned total_items, items_done;
    int counting;
    struct fm_totals { uint64_t bytes; unsigned items; } *totals;
    struct fm_meta property_meta;
    char created[FM_CREATED][FM_NAME]; unsigned created_count;
    /* fx-17: a move publishes the complete copy before removing its source.
     * A source that cannot then be removed (read-only or write-protected
     * medium, in use, access denied) keeps both copies: the item counts as
     * copied, never as a failed copy. kept_error/kept_source name the first. */
    unsigned kept, kept_folders; int kept_error; char kept_source[FM_PATH];
    /* Destination volume identity (stat of the destination folder), so a
     * cross-volume move falls back to copy+delete whatever error the
     * source volume's rename reports first (EXDEV, EROFS, EACCES). */
    uint32_t destination_dev; int destination_known;
    /* Drive letter ('A'..'Z') whose medium disappeared during the job, or 0. */
    char gone;
    /* G2 Recycle Bin bookkeeping (recycle.inc), or NULL for an ordinary job. */
    struct rb_job *bin;
    /* G8: private transfer bookkeeping; never part of the SDK ABI. */
    struct fm_undo_log *undo;
    char **targets;
    int record_undo, undoing, undo_saved, undo_unsafe, undo_error;
};
int fm_join(char *, const char *, const char *);
int fm_beneath(const char *, const char *);
int fm_same_path(const char *, const char *);
int fm_display_path(char *out, const char *posix);
int fm_posix_path(char *out, const char *display);
int fm_drive_root(const char *);
int fm_hidden(const char *parent, const char *name);
int fm_volumes(const struct fm_vfs *, fm_emit, void *);
int fm_volumes_slots(const struct fm_vfs *, uint32_t slots, fm_emit, void *);
int fm_entry_path(const struct fm_model *, unsigned, char *out);
int fm_parent(char *out, const char *path);
int fm_valid_name(const char *);
const char *fm_basename(const char *);
const char *fm_type_name(const struct fm_entry *, char *buffer, size_t size);
enum fm_kind fm_kind(const struct fm_entry *);
int fm_canonical(const struct fm_vfs *, const char *in, char *out);
void fm_init(struct fm_model *, const struct fm_vfs *);
void fm_destroy(struct fm_model *);
int fm_navigate(struct fm_model *, const char *);
int fm_refresh(struct fm_model *);
int fm_search(struct fm_model *, const struct fm_search_query *);
int fm_search_name(const char *pattern, const char *name);
int fm_search_date(const char *text, int end_of_day, int64_t *out);
int fm_search_size(const char *text, uint64_t *out);
int fm_history(struct fm_model *, int);
int fm_up(struct fm_model *);
void fm_sort(struct fm_model *, unsigned, int);
void fm_select(struct fm_model *, unsigned, unsigned modifiers); /* 1 Shift range, 2 Ctrl toggle */
void fm_focus(struct fm_model *, unsigned);
void fm_select_all(struct fm_model *, int invert);
void fm_select_none(struct fm_model *);
int fm_select_name(struct fm_model *, const char *, int add);
unsigned fm_selected(const struct fm_model *, uint64_t *bytes);
int fm_find(struct fm_model *, unsigned, uint64_t now);
int fm_clipboard(struct fm_model *, int cut);
void fm_clipboard_clear(struct fm_model *);
int fm_clipboard_consume(struct fm_model *);
int fm_clipboard_save(struct fm_model *);
int fm_clipboard_load(struct fm_model *);
int fm_view_load(struct fm_model *, int *attributes);
int fm_view_save(struct fm_model *, int attributes);
int fm_unique(struct fm_model *, const char *base, const char *suffix, char *out);
int fm_create(struct fm_model *, const char *, int directory);
int fm_rename(struct fm_model *, unsigned, const char *);
/* fx-17: the drive a POSIX path lives on ('A'..'Z'; 0 for Computer or a
 * relative path), by fm_display_path's /mnt/<letter> rule. */
char fm_drive_letter(const char *posix);
int fm_job_start(struct fm_job *, const struct fm_vfs *, enum fm_job_kind, char *const *, unsigned, const char *);
int fm_job_step(struct fm_job *);
void fm_job_answer(struct fm_job *, enum fm_replace);
void fm_job_cancel(struct fm_job *);
void fm_job_destroy(struct fm_job *);
const struct fm_vfs *fm_native_vfs(void);
int fm_native_version(const char *, char *, size_t);
/* fx-19 drag and drop: the one shared effect rule (Windows 95/98 Explorer,
 * DROPEFFECT values). Drag sources and drop targets, in Files and in the
 * desktop shell, call only fm_drop_effect; fx-17's transfer engine performs
 * the chosen copy or move. Keys are FM_KEY_* bits sampled at each DragOver. */
enum { FM_EFFECT_NONE = 0, FM_EFFECT_COPY = 1, FM_EFFECT_MOVE = 2, FM_EFFECT_LINK = 4, FM_EFFECTS = 7 };
enum { FM_KEY_SHIFT = 1, FM_KEY_CTRL = 2, FM_KEY_ALT = 4 };
/* Drive identity of an absolute path: 'C' for /, 'D'..'Z' for /mnt/d.. ;
 * 0 for the virtual Computer location or a relative path. */
int fm_volume(const char *path);
/* Effect for a drag of items from source_volume onto a target folder on
 * target_volume. allowed: effects both the source and the target permit.
 * Ctrl+Shift or Alt link, Shift moves, Ctrl copies; without modifiers the
 * same drive moves and another drive copies. An explicit request the
 * target cannot perform is NONE; a default falls back to copy, then move. */
unsigned fm_drop_effect(int source_volume, int target_volume, unsigned keys, unsigned allowed);
/* Whether dropping these sources into the destination folder can do
 * anything with this effect: -EINVAL for a folder into itself or a
 * descendant, -EEXIST for a move into the folder that already holds every
 * item (a no-op), 0 otherwise. */
int fm_drop_check(char *const *sources, unsigned count, const char *destination, unsigned effect);
/* Drag data object (CF_HDROP role): the source's item list in its own
 * file, never the shared clipboard. types[i] is 'D' folder or 'F' other. */
int fm_drag_save(const struct fm_vfs *, char *const *paths, const char *types, unsigned count);
int fm_drag_load(const struct fm_vfs *, char ***paths, char **types, unsigned *count);
void fm_drag_free(char **paths, char *types, unsigned count);

/* ---- G2: the Windows 98 Recycle Bin (recycle.inc, compiled into model.c) ----
 * One RECYCLED folder at the root of every fixed FAT volume holds the deleted
 * items under the names D<drive><n>[.ext]; its INFO2 file is Windows 9x's
 * record list (original path, record number, drive, deletion time, size).
 * Delete moves an item there with a rename; Shift+Delete, drives set to "Do
 * not move files to the Recycle Bin", removable media and the bin's own items
 * delete for good. The work is done by the ordinary fm_job engine (a delete job
 * whose sources carry a destination); see docs/design/recycle-bin.md. */
#define FM_RECYCLE "/::{645FF040-5081-101B-9F08-00AA002F954E}"   /* the bin's virtual location; ':' cannot occur in a FAT name */
#define RB_DIR "RECYCLED"
#define RB_INFO "INFO2"
#define RB_PATH 260                    /* Windows 9x MAX_PATH: the INFO2 original-path field */
#define RB_NAME 64
#define RB_DIRLEN 32                   /* "/mnt/x/RECYCLED" */
#define RB_RECORD 280                  /* INFO2 record size of Windows 95/98/ME */
#define RB_HEADER 20
#define RB_POLICY_FILE "/system/settings/recycle.cfg"
#define RB_DEFAULT_PERCENT 10          /* Windows' default maximum size of the bin, percent of the drive */
#define RB_ASSUMED_CAPACITY (512ull << 20)  /* until the ABI reports volume capacity (G6) */
#define RB_LEGACY_TRASH "/Documents/Trash"  /* the pre-G2 desktop Trash folder, migrated into the bin */
struct rb_item {
    uint32_t index;                    /* INFO2 record number, also in the stored name */
    char drive;                        /* 'C'..'Z' */
    uint8_t directory;
    int64_t deleted, modified;         /* Unix seconds: deletion time; the stored item's own time */
    uint64_t size;
    char stored[RB_NAME];              /* D<drive><index>[.ext] in <drive>:\\RECYCLED */
    char original[RB_PATH];            /* DOS path before the deletion; empty when unknown (an orphan) */
};
struct rb_policy {
    int confirm;                       /* Display delete confirmation dialog */
    int independent;                   /* Configure drives independently */
    int nuke;                          /* Do not move files to the Recycle Bin (global) */
    unsigned percent;                  /* maximum size, percent of the drive (global) */
    int drive_nuke[26]; unsigned drive_percent[26];
};
enum rb_mode { RB_NONE, RB_RECYCLE, RB_PURGE, RB_RESTORE };
enum { RB_REFUSE_NONE, RB_REFUSE_LARGE, RB_REFUSE_LONG, RB_REFUSE_UNKNOWN };
/* Delete flags. */
enum { RB_PERMANENT = 1 };
struct rb_job {
    enum rb_mode mode; struct rb_policy policy; uint32_t fixed; int64_t now;
    char **target;                     /* per source: POSIX destination in the bin (or the restore path); NULL: ordinary delete */
    struct rb_item *item;              /* per source: the record to add */
    uint8_t *recycle, *done, *refused;
    unsigned refused_count, recycled, start_skips; int refused_reason; char refused_name[FM_NAME];
    unsigned limit_percent; uint64_t limit_bytes; char limit_drive;
    int begun, finished;
};
int fm_is_recycle(const char *path);
void rb_policy_default(struct rb_policy *);
int rb_policy_load(const struct fm_vfs *, struct rb_policy *);
int rb_policy_save(const struct fm_vfs *, const struct rb_policy *);
unsigned rb_policy_percent(const struct rb_policy *, char drive);
int rb_policy_nuke(const struct rb_policy *, char drive);
/* Test seams: the clock (Unix seconds) and the volume capacity in bytes. */
extern int64_t (*rb_clock_hook)(void);
extern uint64_t (*rb_capacity_hook)(char drive);
uint64_t rb_capacity(char drive);
uint64_t rb_limit(const struct rb_policy *, char drive);
int rb_dir(char drive, char out[RB_DIRLEN]);
const struct rb_item *rb_entry_item(const struct fm_model *, const struct fm_entry *);
void rb_item_name(const struct rb_item *, char out[FM_NAME]);
const char *rb_item_leaf(const struct rb_item *);   /* the original file name in place, or the stored name */
int rb_item_compare_location(const struct rb_item *, const struct rb_item *);          /* original file name (stored name for an orphan) */
void rb_item_location(const struct rb_item *, char out[RB_PATH]);      /* DOS folder it was deleted from */
/* Whether an item at this path is recycled under this policy, and how many of
 * these paths are (the rest delete permanently). */
int rb_would_recycle(const struct rb_policy *, uint32_t fixed, unsigned flags, const char *path);
unsigned rb_classify(const struct rb_policy *, uint32_t fixed, unsigned flags, char *const *paths, unsigned count);
/* Delete confirmation wording (Windows 98 shell32 strings). recycle: the
 * items go to the bin; folder: a single item that is a folder. */
void rb_delete_prompt(unsigned count, int folder, const char *name, int recycle, char *title, size_t title_size, char *text, size_t text_size);
/* Jobs. All return 0 or -errno and leave *j untouched on failure. */
int rb_delete_job(struct fm_job *, const struct fm_vfs *, char *const *paths, unsigned count,
                  const struct rb_policy *, uint32_t fixed, unsigned flags);
int rb_purge_job(struct fm_job *, const struct fm_vfs *, char *const *stored_paths, unsigned count);
int rb_restore_job(struct fm_job *, const struct fm_vfs *, char *const *stored_paths, unsigned count);
int rb_empty_job(struct fm_job *, const struct fm_vfs *, uint32_t fixed);
/* State of the bins on the fixed drives, for the desktop icon and Properties. */
struct rb_usage { unsigned items; uint64_t bytes; };
int rb_usage(const struct fm_vfs *, char drive, struct rb_usage *);
int rb_describe(const struct fm_vfs *, uint32_t fixed, unsigned *count, char name[FM_NAME], int *folder);  /* items in all bins; the only item's name */
int rb_full(const struct fm_vfs *, uint32_t fixed);             /* 1 when any bin or the legacy Trash holds something */
int rb_migrate(const struct fm_vfs *);                           /* /Documents/Trash -> C:\RECYCLED */
unsigned rb_drives(const struct fm_vfs *, uint32_t fixed, char out[26]);  /* fixed drives that exist */
const char *rb_refusal_text(int reason);
#endif
