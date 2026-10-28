/* SYSTEM-only schema-2 persistence and unowned legacy evidence.
 * Relative names in this interface are generated internally, never RPC paths.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_REGISTRY_RESTART_STORE_H
#define CIUKI_REGISTRY_RESTART_STORE_H
#include "restart.h"
struct rj_io {
    void *context;
    int (*read)(void *,const char *,uint8_t **,size_t *); /* malloc output; -ENOENT */
    int (*write)(void *,const char *,const void *,size_t);
    int (*patch)(void *,const char *,size_t,const void *,size_t);
    int (*sync)(void *,const char *); /* file and parent/volume metadata */
    int (*move)(void *,const char *,const char *); /* no replacement */
    int (*remove)(void *,const char *); /* -ENOENT permitted */
};
struct rj_store {
    struct rj_io io;
    unsigned scope;
    uint32_t uid,sid[3];
    uint64_t volume;
};
int rj_store_init(struct rj_store *,const struct rj_io *,unsigned,uint32_t,const uint32_t[3],uint64_t);
int rj_store_read(void *,unsigned,uint32_t,uint8_t **,size_t *);
int rj_store_write(void *,unsigned,uint32_t,const void *,size_t);
/* A protected Legacy directory owns this I/O context. The source callback
 * maps only fixed old input indices, not strings supplied by a caller. */
#define RJ_LEGACY_COUNT 9u
struct rj_quarantine {
    struct rj_io evidence;
    void *source_context;
    int (*source)(void *,unsigned,uint8_t **,size_t *);
    uint32_t sid[3];
    uint64_t volume;
};
int rj_quarantine_run(struct rj_quarantine *);
struct rj_posix { char directory[128]; };
int rj_posix_io(struct rj_posix *,const char *,struct rj_io *);
int rj_posix_legacy(void *,unsigned,uint8_t **,size_t *);
#endif
