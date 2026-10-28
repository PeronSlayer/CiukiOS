/* Private resolved-object policy, shared by native/DOS/Win32 adapters.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FS_SECURITY_POLICY_H
#define CIUKI_FS_SECURITY_POLICY_H
#include "fs_port.h"
struct security_subject;
struct owner_store;
struct vfs;
struct px_node;
struct px_namespace;
struct px_path;
struct vfs_table;
#define POLICY_RULES 32u
enum policy_kind {
    POLICY_PUBLIC, POLICY_ROOT, POLICY_PROGRAM, POLICY_SECRET, POLICY_SEALED,
    POLICY_PROFILES, POLICY_PROFILE, POLICY_SHARED, POLICY_CREATOR,
    POLICY_REMOVABLE, POLICY_READONLY, POLICY_DENY
};
enum policy_access {
    POLICY_READ=1u, POLICY_EXEC=2u, POLICY_TRAVERSE=4u, POLICY_WRITE=8u,
    POLICY_ADD=16u, POLICY_REMOVE=32u, POLICY_RENAME=64u, POLICY_META=128u
};
#define POLICY_MUTATE (POLICY_WRITE|POLICY_ADD|POLICY_REMOVE|POLICY_RENAME|POLICY_META)
#define POLICY_ALL (POLICY_READ|POLICY_EXEC|POLICY_TRAVERSE|POLICY_MUTATE)
/* A label is a cache, never authority supplied by a caller. Its object stamp
 * is the resolver's retained identity, not an exported FAT slot or inode. */
struct policy_label {
    uint64_t mount, epoch, object, owner_key;
    uint32_t uid;
    uint16_t trie;
    uint8_t kind, anchor;
};
struct policy_rule {
    char component[64];
    uint16_t parent;
    uint8_t kind, anchor;
};
struct owner_vfs;
struct policy_volume {
    uint64_t id, mount, epoch;
    struct owner_store *owners;
    /* sec-k03: the OWNER destination lease (owner_vfs_reserved) consulted by
     * every subject-attributed namespace mutation. Set by the integration
     * that installs the OWNER backend for this volume; NULL = no batch lease. */
    const struct owner_vfs *lease;
    struct policy_rule rules[POLICY_RULES];
    unsigned count;
    bool system, removable, readonly, ready;
};
struct policy_reference {
    struct policy_label label;
    struct px_node *node;
    struct security_subject *subject;
    uint32_t access, drive_generation;
};
int policy_init(struct policy_volume *, uint64_t id, uint64_t mount,
                bool system, bool removable, bool readonly, struct owner_store *);
/* Adding a sealed boot/service manifest prefix protects its ancestors as well.
 * Called under namespace synchronization before a mount becomes available. */
int policy_rule_add(struct policy_volume *, const char *, enum policy_kind);
int policy_bind_locked(struct vfs *, unsigned drive, struct policy_volume *);
void policy_unbind_locked(struct vfs *, unsigned drive);
int policy_root(const struct policy_volume *, uint64_t object, struct policy_label *);
uint64_t policy_epoch(const struct policy_volume *);
int policy_child(const struct policy_volume *, const struct policy_label *,
                 const char *resolved_long_name, uint64_t object, uint64_t locator,
                 struct policy_label *);
int policy_check(const struct policy_volume *, const struct policy_label *,
                 const struct security_subject *, unsigned access);
int px_policy_label_locked(struct px_node *, struct policy_label *);
int px_object_proof_locked(struct px_node *, uint8_t proof[32]);
/* These APIs preserve intermediate components and check real traversal before
 * resolving the next name (including '..'). The final operation belongs to k03. */
int px_resolve_subject_locked(struct px_namespace *, struct px_node *, const char *,
                              bool missing_leaf, const struct security_subject *, struct px_path *);
/* Caller first selects the actual mounted volume (including DOS drive maps).
 * Converts separators/Win32 trimming only; never collapses intermediate '..'. */
int px_resolve_volume_subject_locked(struct px_namespace *,unsigned drive,struct px_node *cwd,
    const char *,bool win32,bool missing_leaf,const struct security_subject *,struct px_path *);
int px_policy_check_locked(struct px_node *, const struct security_subject *, unsigned);
int vfs_resolve_subject_locked(struct vfs_table *,const struct security_subject *,const char *,
    bool win32,bool missing_leaf,struct px_path *);
int policy_reference_get_locked(struct px_node *,struct security_subject *,unsigned,struct policy_reference *);
int policy_reference_check_locked(struct policy_reference *,unsigned);
void policy_reference_put_locked(struct policy_reference *);
#endif
