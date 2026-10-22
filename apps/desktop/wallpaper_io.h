/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_WALLPAPER_IO_H
#define CIUKI_WALLPAPER_IO_H
#include "wallpaper.h"
#include "settings.h"
#include <pthread.h>
#define DESK_WALL_CACHE "/System/cache/wallpaper"
#define DESK_WALL_NAME_MAX 64
struct desk_wall_request {
    char path[CFG_PATH_MAX];
    uint32_t width, height, top, bottom, colour;
    int mode;
    const uint32_t *portrait; /* immutable approved asset, valid until worker_stop */
    int caption_font;
    uint32_t caption_colour;
    int caption_aa;
};
struct desk_wall_memory;
/* Single worker, one latest pending request and one owned completed frame.
 * Neither disk I/O nor rendering holds lock. UI collection uses a short mailbox lock. */
struct desk_wall_worker {
    pthread_mutex_t lock; pthread_cond_t changed; pthread_t thread;
    struct desk_wall_request request;
    unsigned serial, completed_serial;
    int pending, busy, ready, stopping, error, cache_hit;
    uint32_t *pixels;
    const char *cache_dir;
    void (*stage)(const char *);
    void (*notify)(void *);
    void *notify_ctx;
    struct desk_wall_memory *memory; /* worker-only, bounded reusable frames/source */
    uint64_t prepare_ns; /* completed request, published with ready under lock */
    const char *activity; /* atomic diagnostic: literal name of the current operation */
};
void desk_wall_cache_name(char out[DESK_WALL_NAME_MAX], const struct desk_wall_request *,
                          uint64_t size, int64_t mtime);
int desk_wall_prepare(const struct desk_wall_request *, const char *cache_dir,
                      uint32_t **pixels, int *hit, void (*stage)(const char *));
/* Synchronous start-up read of the seeded frame; 0 with *pixels owned by the
 * caller, else non-zero and *pixels NULL (solid-colour fallback). */
int desk_wall_preload(const struct desk_wall_request *, const char *cache_dir,
                      uint32_t **pixels, void (*stage)(const char *));
/* Runtime reads disk seeds but never persists frames/journals: native FAT
 * full-file COW on short writes can hold the shared filesystem for seconds. */
int desk_wall_worker_start(struct desk_wall_worker *, const char *, void (*stage)(const char *));
unsigned desk_wall_worker_submit(struct desk_wall_worker *, const struct desk_wall_request *);
/* submit serializes only the bounded mailbox copy. */
/* 0: pending, 1: ownership transferred (pixels NULL on failure). */
int desk_wall_worker_take(struct desk_wall_worker *, unsigned, uint32_t **pixels, int *error, int *hit);
int desk_wall_worker_busy(struct desk_wall_worker *);
/* Shutdown only, never on an input/service turn. */
void desk_wall_worker_stop(struct desk_wall_worker *);
#endif
