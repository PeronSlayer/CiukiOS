/* Frozen f1-01 VFS interface, v1. All failures are negative FS_E*.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VFS_H
#define CIUKI_VFS_H
#include "fat.h"
#define VFS_DRIVE_REMOVABLE 1u
#include <ciuki/fs_ro.h>
#define VFS_NODES 64u
#define VFS_DESCRIPTIONS 128u
#define VFS_HANDLES 64u
#define VFS_TABLES 16u
#define VFS_READ 1u
#define VFS_WRITE 2u
#define VFS_CREATE 4u
#define VFS_EXCLUSIVE 8u
#define VFS_TRUNCATE 16u
#define VFS_FAIL_EXISTING 32u /* extended-open action 00, atomic existence error */
#define VFS_FIND_DOS 128u
/* Bitmask of access denied to other opens, checked in BOTH directions. */
enum vfs_share { VFS_DENY_NONE=0, VFS_DENY_READ=1, VFS_DENY_WRITE=2, VFS_DENY_ALL=3 };
enum vfs_owner_type { VFS_OWNER_NATIVE, VFS_OWNER_WIN32, VFS_OWNER_DOS, VFS_OWNER_TYPES };
struct vfs_lock_owner { enum vfs_owner_type type; uint64_t id; };
#define VFS_IDENTITIES 256u
#define VFS_PAGE_IDENTITIES 256u
struct vfs_file_identity {
    struct fat_volume *volume;
    uint64_t serial, generation;
    uint64_t unlinked_id; /* exported tombstone generation, not cache serial */
    uint32_t parent, index, refs, pages;
    bool valid, linked, writer;
};
struct vfs_page_key { uint64_t generation, file, index; };
struct vfs_page_identity {
    struct vfs_file_identity *file;
    struct vfs_page_key key;
    uint32_t refs;
    bool valid;
};
/* f3-51c write generations: the latest change sequence of recently changed
 * file locations (write, truncate, rename from/to, unlink), bounded; a
 * location no longer listed reports the floor, the largest sequence ever
 * evicted. Volume expiry raises the floor. Session-only, never on disk. */
#define VFS_CHANGES 64u
struct vfs_change { struct fat_volume *volume; uint64_t mount, sequence; uint32_t parent, index; };
/* Content version of an open FAT file. Equal versions denote the same bytes
 * for this session: write_generation advances on every change of the file's
 * location, mount on every attachment or media change of its volume. */
struct vfs_file_version {
    uint64_t mount, write_generation;
    uint32_t drive, drive_generation, parent, index, first, size;
    uint16_t write_date, write_time;
};
/* Bounded content-version watches used by the launch digest LRU. They pin
 * only a change counter, never an open file, volume reservation or cache page. */
struct vfs;
struct vfs_table;
#define VFS_VERSION_PINS 16u
/* Counter watches retain no file/volume handle. The VFS must outlive every
 * token; unpin before destroying a nonresident VFS. */
struct vfs_version_pin { struct vfs *vfs; uint64_t serial; };
struct vfs_version_watch {
    struct fat_volume *volume;
    uint64_t mount, sequence, serial;
    uint32_t parent, index, refs;
};
int vfs_version_pin(struct vfs_table *,int,const struct vfs_file_version *,struct vfs_version_pin *);
void vfs_version_unpin(struct vfs_version_pin *);
enum vfs_whence { VFS_SEEK_SET, VFS_SEEK_CUR, VFS_SEEK_END };
/* Internal DOS bridge errors; never user syscall errno values. */
#define VFS_ESHARE 201
#define VFS_ECRITICAL_SHARE 202
#define VFS_ELOCK 203
#define VFS_ELOCKFULL 204
int vfs_request_check(const struct vfs_request *);
int vfs_dos_share(unsigned old_mode, unsigned old_access, uint64_t old_owner,
                  unsigned new_mode, unsigned new_access, uint64_t new_owner, bool readonly);
struct vfs_node { struct fat_volume *volume; struct fat_entry entry; unsigned refs;
    struct fs_ro_volume *ro; uint64_t ro_id, ro_size; uint32_t drive, generation;
    uint32_t log_capacity, pins; struct vfs_file_identity *identity; };
/* f3-51c: cursor is this open file's FAT cluster cursor. Only the
 * description's own serialized I/O (io_active or the handle reservation)
 * reads or writes it; fat.c validates it against the node entry. */
struct vfs_description { struct vfs_node *node; uint64_t position; unsigned refs, access, deny;
    uint64_t owner; unsigned dos_mode; enum vfs_owner_type owner_type; bool io_active; struct cache_stream stream;
    struct fat_cursor cursor; };
struct vfs_range_lock { struct vfs_description *description; struct vfs_node *node;
    uint64_t owner, start, end, length; enum vfs_owner_type owner_type; bool shared; };
#define VFS_RANGE_LOCKS 128u
struct vfs_waiter;
struct vfs_reader;
struct vfs_table;
struct vfs_hold_info {
    struct fs_task_identity task;
    char op[12];
    char site[12];
    uint32_t started;
    uint8_t attribution; /* explicit operation/site; fallback text is not a tag */
};
#define VFS_ATTR_OPERATION 1u
#define VFS_ATTR_SITE 2u
#define VFS_ATTR_COMPLETE (VFS_ATTR_OPERATION | VFS_ATTR_SITE)
#define VFS_URGENT_OVERTAKE_MS 100u /* C3: close admission for an aged ticket */
#define VFS_URGENT_DRAIN_MS 10u /* bounded drain, then an equal reader turn */
/* One namespace wait and one volume/file admission wait per locked turn.
 * These values follow the owner across sleeps and VFS_IO, never pointers
 * into another task's request or the shared holder table. */
struct vfs_wait_record {
    struct fs_task_identity task;
    struct vfs_hold_info owner;
    uint32_t ms;
    char drive, holder;
    const char *reason; /* static tag */
};
struct vfs_pending_waits { unsigned count; struct vfs_wait_record records[2]; };
struct vfs_contention_budget {
    uint32_t start, hold_max_ms, wait_max_ms, hold_suppressed, wait_suppressed;
    unsigned holds, waits;
    bool valid;
};
void vfs_wait_observer(char,uint32_t,char,const char *,const struct fs_task_identity *,const struct vfs_hold_info *) __attribute__((weak));
void vfs_hold_observer(char,uint32_t,const struct vfs_hold_info *,uint32_t) __attribute__((weak));
struct vfs {
    /* Order: namespace (1) -> volumes (2, ascending letter) -> cache (3)
     * -> device (4). Never wait for a reserved volume with namespace held.
     * Bookkeeping/identity phases hold namespace and the operation's volumes.
     * VFS_IO drops namespace and volume mutexes during filesystem work,
     * then reacquires namespace -> volumes. Reservations
     * retire before transport. A separate mutation guard excludes other
     * metadata operations/removal until bookkeeping finishes;
     * per-file readers can pass an identified unrelated writer. Snapshots need only
     * namespace and never join reservations. No IRQ/interrupts-off I/O. */
    fs_lock lock, volume_lock[26];
    struct fs_event volume_changed;
    uint32_t reserved, active; /* namespace protected; active belongs to its owner */
    uint32_t mutating, active_tickets; /* publication guard versus short tickets */
    uint32_t next[26], serving[26]; /* FIFO reservation tickets per letter */
    /* Data readers pin their open description, not the volume ticket. An
     * identified writer may coexist only with readers of other nodes. Other
     * metadata operations wait for these short per-file I/O phases. */
    uint32_t readers[26];
    uint32_t transaction_users[26]; /* lifetime only; never reader exclusion */
    /* f1-89 writer turn: one FAT-mutating operation per letter plans and
     * publishes at a time. Namespace operations keep it for their whole
     * life; data operations (write, extend, truncate, publish, attributes)
     * give it up around device waits and merge on return (Amendment 1 item
     * 4). Taken BEFORE volume admission (order: turn -> namespace ->
     * volumes); a waiter holds no ticket, reservation or cache pin. Readers,
     * lookups, stat and open-file reads never take or wait for it. */
    uintptr_t turn_owner[26];
    uint32_t turn_depth[26], turn_waiters[26];
    /* f1-89 Amendment 1 item 4: writer-writer waits per letter, measured
     * from the wait's start to the instant the turn became free. */
    uint32_t turn_waits[26], turn_wait_max_ms[26], turn_free_since[26];
    /* Publication steps waiting for in-flight readers (f1-89 A1 item 4):
     * while set, NEW open-file readers of that letter wait, so a reader
     * stream cannot starve a short publication step. */
    uint32_t publish_waiting[26];
    uint32_t gate_ticket[26]; /* fx-23: first ticket issued after a publication step was refused */
    uint32_t lookup_restarts, lookup_guarded; /* diagnostics: reader restarts */
    /* f1-93 device windows (logical providers). io_owner/io_mode name the
     * task whose VFS_IO_MODE holds the drive's guard and how it may give
     * it up around a device call; io_sequence is a VALIDATE reader's
     * mutation_sequence snapshot. Namespace protected; the owner reads its
     * own entries without it. Counters are diagnostics: windows opened,
     * reader restarts after a concurrent change, guarded/cached steps that
     * had to warm a sector, and the bounded fallbacks that still read cold
     * under the guard (zero in every host gate). */
    uintptr_t io_owner[26];
    uint8_t io_mode[26];
    /* f1-98: the task (fs_task_token) holding each drive's reservation or
     * guard, from admission to vfs_unlock or a device-window pause; waiters
     * lend it their priority (fs_depend). Namespace protected. */
    uintptr_t hold_token[26];
    uint64_t io_sequence[26];
    uint32_t window_pauses, window_restarts, warm_restarts, cold_fallbacks;
    /* f1-93b: cache-only sections ended by their work bound (the guard is
     * given up before the preemption point), and resumed walks restarted
     * from the start because something was published meanwhile. */
    uint32_t step_yields, step_restarts;
    struct vfs_reader *reader_owners; /* stack records, namespace protected */
    uint32_t urgent_gate_start[26];
    bool urgent_gate_valid[26];
    struct vfs_node *io_file[26];
    char holder; /* atomic diagnostic snapshot of namespace owner's drive */
    struct vfs_hold_info holds[26], namespace_owner;
    uint32_t diagnostic_waiters[26]; /* existing namespace lock, no new lock */
    struct vfs_pending_waits diagnostic_pending; /* namespace owner's values */
    struct vfs_contention_budget diagnostic_budget[27]; /* identity_lock; '-' is 26 */
    unsigned default_drive; /* operation's legacy path-resolution snapshot */
    struct fat_volume *volumes[26];
    uint32_t drive_flags[26];
    struct fs_ro_volume *ro_volumes[26];
    uint32_t generation[26];
    struct vfs_node nodes[VFS_NODES];
    struct vfs_description descriptions[VFS_DESCRIPTIONS];
    struct vfs_table *tables[VFS_TABLES];
    struct vfs_range_lock range_locks[VFS_RANGE_LOCKS];
    struct vfs_waiter *waiters;
    fs_lock identity_lock; /* short table updates; no I/O or namespace acquisition */
    uint64_t next_identity, next_unlinked_id;
    uint32_t next_identity_generation;
    struct vfs_file_identity identities[VFS_IDENTITIES];
    struct vfs_page_identity page_identities[VFS_PAGE_IDENTITIES];
    uint64_t change_sequence, change_floor; /* identity_lock */
    struct vfs_version_watch version_watches[VFS_VERSION_PINS];
    uint64_t next_version_watch;
    struct vfs_change changes[VFS_CHANGES];
};
struct vfs_table {
    struct vfs *vfs;
    struct vfs_description *handles[VFS_HANDLES];
    unsigned drive; /* 0=A, 2=C */
    uint32_t cwd_cluster[26]; /* zero denotes the volume root */
    char cwd[26][FS_PATH_BYTES];
    const struct vfs_request *request; /* caller-owned, one operation at a time */
    uint64_t owner; /* context id within owner_type, not a global PID namespace */
    enum vfs_owner_type owner_type;
    bool owner_registered; /* legacy DOS tables still set owner directly until bridge adoption */
};
struct vfs_find {
    unsigned drive; uint32_t generation, directory, cursor;
    uint8_t attr_mask; char pattern[FS_NAME_BYTES];
    bool ro; uint64_t ro_directory;
};
/* Caller owns tables, mounted volumes and cache. Cold FAT metadata reads
 * may use a temporary <=64 KiB run buffer, with demand-sector fallback on
 * allocation failure. Node/description capacity exhaustion is explicit.
 * Initialize once; destroy after tables/volumes are detached. */
void vfs_init(struct vfs *);
void vfs_destroy(struct vfs *);
/* Every namespace entry (native callers included) ends with vfs_unlock.
 * Reserve the complete drive set BEFORE borrowing identities across I/O.
 * _locked helpers may reserve a set while active is zero; extending an
 * operation must not wait (multi-path callers preflight the union). */
#define VFS_ALL_DRIVES ((1u << 26) - 1)
static inline uint32_t vfs_drive_mask(unsigned d) { return d < 26 ? 1u << d : 0; }
void vfs_lock(struct vfs *, uint32_t drives);
void vfs_reserve_locked(struct vfs *, uint32_t drives);
void vfs_reserve_file_locked(struct vfs *, uint32_t drives,struct vfs_file_identity *);
void vfs_unlock(struct vfs *);
/* Diagnostic tag only: call under namespace before starting a mutation. */
void vfs_tag_locked(struct vfs *,const char *tag);
void vfs_site_locked(struct vfs *,const char *site);
/* Namespace held, no volume reservation on entry/return. A private FAT view
 * and the reader reference protect an immutable open chain during transport. */
int vfs_read_identity_locked(struct vfs *,struct vfs_file_identity *,struct fat_volume *,
    const struct fat_entry *,struct fat_cursor *,bool *,uint64_t *,const uint64_t *,bool,
    int (*)(void *,uint64_t,size_t),void *,void *,size_t,size_t *);
/* Namespace/volume turn on entry and return. Native callers additionally
 * retain their px_node/description and borrowed parents across this call. */
int vfs_fat_transaction_locked(struct vfs *,struct fat_volume *,struct fat_transaction_args);
int vfs_file_wait_locked(struct vfs *,const bool *busy);
/* Writer turn for FAT-mutating operations (see struct vfs). take_locked is
 * for callers already holding namespace/admission: when the turn is busy it
 * releases every reservation, waits, and readmits the same set, so it must
 * precede any borrowed name/entry. Nested takes by the owner only count. */
int vfs_turn_take_locked(struct vfs *,uint32_t drives,uint32_t *taken);
void vfs_turn_drop_locked(struct vfs *,uint32_t drives);
bool vfs_turn_held_locked(struct vfs *,uint32_t drives);
/* A data transaction gives its turn up around device waits (namespace held,
 * no reservation): suspend returns the depth to restore; resume waits as a
 * queued writer, uncancellably, and returns holding namespace only. */
uint32_t vfs_turn_suspend_locked(struct vfs *,unsigned drive);
void vfs_turn_resume_locked(struct vfs *,unsigned drive,uint32_t depth);
bool vfs_turn_try_resume_locked(struct vfs *,unsigned drive,uint32_t depth);
struct vfs_io_state { uint32_t drives; unsigned default_drive; bool logical;
    struct vfs_pending_waits diagnostics; };
/* f1-93 device windows: how a VFS_IO owner treats a logical provider's
 * device call. NONE keeps the guard across it (legacy, teardown). FREE
 * gives the guard up around the call and readmits it (FIFO): the owner
 * holds the drive's writer turn, so no other mutator can intervene, or it
 * is a barrier whose result does not depend on the volume state. VALIDATE
 * (readers without the turn) also gives it up, and the call returns
 * -FS_EAGAIN when any write was published or announced since the VFS_IO
 * began: the caller restarts from a consistent point. */
enum vfs_window { VFS_WINDOW_NONE, VFS_WINDOW_VALIDATE, VFS_WINDOW_FREE };
struct vfs_io_state vfs_io_begin_locked(struct vfs *);
struct vfs_io_state vfs_io_begin_mode_locked(struct vfs *, enum vfs_window);
void vfs_io_end_locked(struct vfs *, struct vfs_io_state);
/* Run call under the caller's VFS_IO_MODE window for vol (dos_file_volume
 * raw sectors); outside one, or for an unattached volume, call directly. */
int vfs_io_window_call(struct vfs *, struct fat_volume *, bool write, int (*call)(void *), void *arg);
/* f1-93: directory and chain steps that never hold a volume across a cold
 * device read. Namespace and the volume's reservation held on entry and
 * return (the reservation may be released and readmitted in between).
 * Physical: resident sectors only under the guard; a miss is read with
 * nothing held and the step resumes (f1-93b: see vfs_dir_walk_locked).
 * Logical: device windows, restarted after a concurrent change. name 0:
 * next entry from *cursor (labels: fat_next_dos); otherwise lookup of name
 * in dir (cursor unused). */
int vfs_dir_step_locked(struct vfs *, struct fat_volume *, uint32_t dir, const char *name,
                        uint32_t *cursor, struct fat_entry *, bool labels);
/* f1-93b: the same step, resumable. A physical step runs in cache-only
 * sections bounded by FAT_CACHED_BUDGET; between sections (a miss to warm,
 * or the bound) the guard is given up, a preemption point is offered with
 * nothing held, and the step resumes at its first unsettled entry while the
 * volume's mutation_sequence is unchanged (else from its start). The holder
 * is never preempted while it holds the volume. hint (optional) keeps the
 * walk position across steps (a native readdir description). */
int vfs_dir_walk_locked(struct vfs *, struct fat_volume *, uint32_t dir, const char *name,
                        uint32_t *cursor, struct fat_entry *, bool labels, struct fat_dir_hint *hint);
/* Clusters in the chain starting at first (st_blocks), the same way; a
 * cached section visits at most FAT_CHAIN_CACHED_SECTORS FAT sectors and
 * gives the guard up before its preemption point (f1-93b). */
int vfs_chain_clusters_locked(struct vfs *, struct fat_volume *, uint32_t first, uint32_t *clusters);
/* f3-51c write generation of a file location (identity lock taken). */
uint64_t vfs_change_generation(struct vfs *, struct fat_volume *, uint32_t parent, uint32_t index);
/* Filesystem expressions must not call namespace/observer helpers. */
#define VFS_IO(v, expression) __extension__ ({ \
    struct vfs *io_v_ = (v); \
    if (!io_v_->active) fs_panic("filesystem without volume reservation"); \
    struct vfs_io_state io_mask_ = vfs_io_begin_locked(io_v_); \
    int io_result_ = (expression); vfs_io_end_locked(io_v_, io_mask_); io_result_; })
#define VFS_IO_MODE(v, mode, expression) __extension__ ({ \
    struct vfs *io_v_ = (v); \
    if (!io_v_->active) fs_panic("filesystem without volume reservation"); \
    struct vfs_io_state io_mask_ = vfs_io_begin_mode_locked(io_v_, (mode)); \
    int io_result_ = (expression); vfs_io_end_locked(io_v_, io_mask_); io_result_; })
#define VFS_IO_VOID(v, expression) do { \
    struct vfs *io_v_ = (v); \
    if (!io_v_->active) fs_panic("filesystem without volume reservation"); \
    struct vfs_io_state io_mask_ = vfs_io_begin_locked(io_v_); \
    (expression); vfs_io_end_locked(io_v_, io_mask_); \
} while (0)
int vfs_attach(struct vfs *, unsigned drive, struct fat_volume *);
int vfs_attach_locked(struct vfs *, unsigned drive, struct fat_volume *);
int vfs_attach_ro(struct vfs *, unsigned drive, struct fs_ro_volume *);
int vfs_attach_ro_locked(struct vfs *, unsigned drive, struct fs_ro_volume *);
bool vfs_drive_present(struct vfs *, unsigned drive);
/* Namespace lock held. EIO invalidates media cache/names/nodes/cwd and
 * removes the letter, even with live descriptions. No media writes. */
int vfs_ro_check_locked(struct vfs *, unsigned drive);
void vfs_ro_expire_locked(struct vfs *, unsigned drive);
int vfs_detach(struct vfs *, unsigned drive); /* durable unmount, refuses opens */
int vfs_table_init(struct vfs *, struct vfs_table *, unsigned default_drive);
void vfs_table_destroy(struct vfs_table *);
int vfs_open(struct vfs_table *, const char *path, unsigned flags, enum vfs_share, uint8_t create_attr);
/* Same atomic namespace transaction as native open; mode 0..4 is DOS sharing,
 * 255 denotes native explicit sharing. status receives 1/2/3 only on success. */
int vfs_open_claim(struct vfs_table *, const char *, unsigned flags, enum vfs_share,
                   uint8_t attr, unsigned mode, unsigned *status);
int vfs_lock_range(struct vfs_table *, int, uint32_t start, uint32_t length, bool unlock);
/* Namespace held. Native and Win32 descriptions use the same identity/owner
 * table as DOS; ranges are 64-bit, including locks beyond EOF. */
int vfs_description_lock_locked(struct vfs *, struct vfs_description *,
    struct vfs_lock_owner, uint64_t, uint64_t, bool exclusive, bool unlock);
int vfs_description_io_locked(struct vfs *, struct vfs_node *, struct vfs_description *,
    struct vfs_lock_owner, uint64_t, uint64_t, bool write);
void vfs_description_unlock_locked(struct vfs *, struct vfs_description *);
/* Register a personality/context before opening handles. DOS bridge uses this
 * interface; never infer a personality from a numeric PID/PSP. */
int vfs_table_set_owner(struct vfs_table *, enum vfs_owner_type, uint64_t);
void vfs_release_lock_owner(struct vfs *, struct vfs_lock_owner);
void vfs_release_owner(struct vfs *, uint64_t owner); /* legacy native-namespace release */
int vfs_range_check_owner(struct vfs *, struct vfs_node *, struct vfs_description *,
                          struct vfs_lock_owner, uint64_t start, uint64_t length);
int vfs_range_check(struct vfs *, struct vfs_node *, struct vfs_description *,
                    uint64_t owner, uint64_t start, uint64_t length);
int vfs_handle_stat(struct vfs_table *, int, struct fat_entry *, uint64_t *position);
/* f3-51c: the open file's content version; read-only providers (no FAT
 * write generation) return -FS_EOPNOTSUPP. Caches keyed by it must take it
 * while a deny-write open pins the content and compare it field by field. */
int vfs_handle_version(struct vfs_table *, int, struct vfs_file_version *);
static inline bool vfs_file_version_equal(const struct vfs_file_version *a, const struct vfs_file_version *b) {
    return a->mount==b->mount && a->write_generation==b->write_generation && a->drive==b->drive &&
        a->drive_generation==b->drive_generation && a->parent==b->parent && a->index==b->index &&
        a->first==b->first && a->size==b->size && a->write_date==b->write_date && a->write_time==b->write_time;
}
int vfs_handle_times(struct vfs_table *, int, uint16_t date, uint16_t time);
/* Admission observes the explicit request or the task-scoped f1-83 calldown
 * context. Reacquisition retires published references under short bookkeeping
 * locks even after cancellation; it never issues more storage on an expired
 * request. Controller buffers remain owned until DMA retirement. */
int vfs_admit(struct vfs *, uint32_t drives, const struct vfs_request *);
int vfs_close(struct vfs_table *, int handle);
/* dst=-1 selects first free handle. Duplicates/inherited handles share position.
 * Explicit dst replacement releases the previous reference atomically. */
int vfs_dup(struct vfs_table *src, int handle, struct vfs_table *dst, int dst_handle);
int vfs_read(struct vfs_table *, int, void *, size_t, size_t *done);
int vfs_write(struct vfs_table *, int, const void *, size_t, size_t *done);
int vfs_seek(struct vfs_table *, int, int64_t offset, enum vfs_whence, uint64_t *position);
int vfs_truncate(struct vfs_table *, int, uint64_t size);
int vfs_commit(struct vfs_table *, int);
int vfs_flush_volume_until(struct vfs *, unsigned, const struct vfs_request *);
int vfs_completion(struct vfs_table *, int, struct cache_completion *);
/* Identity calls serialize internally and retain no data pages. A mapping must
 * own a file/page reference independently of its descriptor. */
int vfs_identity_get(struct vfs *, struct fat_volume *, struct fat_entry *, struct vfs_file_identity **);
void vfs_identity_put(struct vfs *, struct vfs_file_identity *);
bool vfs_identity_live(struct vfs *, struct vfs_file_identity *);
/* Atomic live FAT location/volume snapshot; zero when identity is unavailable. */
uint64_t vfs_identity_file_id(struct vfs *, struct vfs_file_identity *, uint32_t *volume_serial);
int vfs_page_get(struct vfs *, struct vfs_file_identity *, uint64_t index, struct vfs_page_identity **);
void vfs_page_put(struct vfs *, struct vfs_page_identity *);
void vfs_identity_expire(struct vfs *, struct fat_volume *);
bool vfs_identity_busy(struct vfs *, struct fat_volume *);
/* External/raw write detection expires identities; caller quiesces I/O and
 * resolves dirty block-cache data before authorizing the external writer. */
void vfs_raw_changed(struct vfs *, struct fat_volume *);
int vfs_flush_drive(struct vfs_table *,unsigned drive);
/* Internal, exclusive boot-log sink. Reserve once; write aligned data in
 * place; publish logical EOF periodically; finish releases unused clusters. */
int vfs_log_reserve(struct vfs_table *, int, uint32_t capacity);
int vfs_log_write(struct vfs_table *, int, const void *, size_t, uint32_t logical_size, size_t *done);
int vfs_log_publish(struct vfs_table *, int, bool finish);
int vfs_log_rotate(struct vfs_table *, const char *current, const char *previous,
                   const char *oldest, uint32_t capacity);
/* f0-49 pagefile, exclusive VFS_DENY_ALL read/write handle only. extend and
 * trim keep live clusters in place; map converts clusters [index,index+count)
 * to device-relative sector runs in bounded, resumable steps (mapped may be
 * below count with success: call again with the updated cursor). */
#define PAGEFILE_MAP_WORK 512u
struct vfs_pagefile_run { uint64_t lba; uint32_t sectors; };
/* f0-49b: the cursor is a hint under the f3-51c chain generation, followed
 * only while the entry still names the same first cluster and chain stamp
 * (an extension link, trim, relocation or release retires it); emptied on
 * any error. A zero cursor is empty. */
struct vfs_pagefile_cursor { uint64_t generation; uint32_t first, cluster, index; };
int vfs_pagefile_extend(struct vfs_table *, int, uint32_t size);
int vfs_pagefile_trim(struct vfs_table *, int, uint32_t size);
int vfs_pagefile_map(struct vfs_table *, int, struct vfs_pagefile_cursor *, uint32_t index,
                     uint32_t count, struct vfs_pagefile_run *, unsigned max, unsigned *used,
                     uint32_t *mapped, uint32_t *cluster_bytes);
int vfs_stat(struct vfs_table *, const char *, struct fat_entry *);
int vfs_attrib(struct vfs_table *, const char *, uint8_t attr);
int vfs_times(struct vfs_table *, const char *, const struct fat_times *);
int vfs_find_first(struct vfs_table *, const char *pattern, uint8_t mask, struct vfs_find *, struct fat_entry *);
int vfs_find_next(struct vfs_table *, struct vfs_find *, struct fat_entry *);
int vfs_rename(struct vfs_table *, const char *from, const char *to);
int vfs_delete(struct vfs_table *, const char *);
int vfs_mkdir(struct vfs_table *, const char *, uint8_t attr);
int vfs_rmdir(struct vfs_table *, const char *);
int vfs_chdir(struct vfs_table *, const char *);
int vfs_set_drive(struct vfs_table *, unsigned drive);
int vfs_getcwd(struct vfs_table *, unsigned drive, char out[FS_PATH_BYTES]);
int vfs_free_space(struct vfs_table *, unsigned drive, uint64_t *bytes, uint32_t *cluster_bytes);
/* DOS compatibility and mandatory record claims share native node identities. */
#endif
