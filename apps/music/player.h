/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_MUSIC_PLAYER_H
#define CIUKI_MUSIC_PLAYER_H
#include "playlist.h"
#include <pthread.h>
#include <stdatomic.h>
#define MUSIC_RING 131072
#define MUSIC_COMMANDS 8
/* A backend owns one nonblocking native stream; counters reset at open. */
struct music_output {
    int (*open)(void *, unsigned *rate);
    int (*write)(void *, const int16_t *, unsigned frames);
    int (*status)(void *, uint64_t *consumed, unsigned *queued, uint64_t *tail_ns);
    void (*close)(void *);
    uint64_t (*now)(void *);
    void *ctx;
};
enum music_state { MUSIC_STOPPED, MUSIC_LOADING, MUSIC_PLAYING, MUSIC_PAUSED, MUSIC_ERROR, MUSIC_CLOSING };
enum music_action { MUSIC_OPEN, MUSIC_ADD, MUSIC_SAVE, MUSIC_PLAY, MUSIC_PAUSE, MUSIC_STOP, MUSIC_PREV, MUSIC_NEXT, MUSIC_SEEK, MUSIC_SELECT, MUSIC_CLEAR };
struct music_command { enum music_action action; uint64_t value; char path[MUSIC_PATH]; };
struct music_snapshot {
    enum music_state state; int error; unsigned current, rate, revision;
    unsigned reservoir_frames, starvations; int buffering;
    char error_path[MUSIC_PATH];
    uint64_t position, duration; struct music_playlist playlist;
};
struct music_player {
    struct music_snapshot view;
    const struct music_output *output; struct music_decoder *decoder;
    int16_t ring[MUSIC_RING*2]; unsigned head, count;
    /* The reader publishes count/eof/error/length under reservoir_lock only.
     * History shares the ring: submitted PCM is retained until audible. */
    pthread_t reader; pthread_mutex_t reservoir_lock; pthread_cond_t space;
    atomic_int reader_stop; int reader_live, sync_error, reader_error;
    unsigned history, reader_rate, source_rate, source_channels; uint64_t written, audible, reader_length;
    int device, eof, started; uint64_t base, tail_deadline;
    uint64_t starve_started, starve_logged; int starve_log_valid;
    uint64_t load_started; int first_pcm;
    const char *stage;
    void (*changed)(void *, const struct music_snapshot *);
    music_cancel cancel; void *ctx;
};
void music_player_init(struct music_player *, const struct music_output *, music_cancel, void *);
int music_player_command(struct music_player *, const struct music_command *);
int music_player_pump(struct music_player *);
void music_player_destroy(struct music_player *);
/* Runtime: the UI only queues commands and reads snapshots. */
struct music_runtime;
struct music_runtime *music_runtime_start(const struct music_output *);
int music_runtime_send(struct music_runtime *, const struct music_command *);
void music_runtime_snapshot(struct music_runtime *, struct music_snapshot *);
void music_runtime_close(struct music_runtime *);
int music_runtime_done(struct music_runtime *);
void music_runtime_destroy(struct music_runtime *);
#endif
