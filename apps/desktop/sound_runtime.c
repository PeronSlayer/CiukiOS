/* SPDX-License-Identifier: MIT
 * Initialisation reserves PCM storage and reads the small scheme before the
 * first frame. The worker owns WAV/audio I/O; the compositor only queues events
 * and reads completion, with no AUDIO_DRAIN or audio writes in its thread. */
#include "desktop.h"
#include "sounds.h"
#include "sound_runtime.h"
#include "sound_storage.h"
#include <ciuki/audio.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define QMAX 16
#define CMD_RELOAD 100
#define CMD_HARDWARE 101
#define CMD_STOP 102
#define CMD_SHUTDOWN 103
#define CMD_TEST 104
struct command { unsigned kind, event; };
static struct {
    pthread_mutex_t lock; pthread_cond_t changed;
    struct command q[QMAX]; unsigned head, count;
    int live, busy, hardware_pending, hardware_done, hardware_result;
    int client; uint32_t window, serial;
    int shutting; uint64_t shutdown_deadline;
    struct sound_throttle queued;
} rt = {.lock=PTHREAD_MUTEX_INITIALIZER, .changed=PTHREAD_COND_INITIALIZER};
static uint64_t now_ns(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec; }
static int push(unsigned kind, unsigned event)
{
    pthread_mutex_lock(&rt.lock);
    int r = !rt.live ? -ENODEV : rt.count == QMAX ? -EBUSY : 0;
    if (!r && (kind == SOUND_VOLUME || (kind == CMD_TEST && event == SOUND_VOLUME)) &&
        !sound_coalesce(&rt.queued,SOUND_VOLUME,now_ns())) { pthread_mutex_unlock(&rt.lock); return 0; }
    if (!r) {
        rt.q[(rt.head+rt.count++)%QMAX] = (struct command){kind,event};
        rt.busy = 1; pthread_cond_signal(&rt.changed);
    }
    pthread_mutex_unlock(&rt.lock); return r;
}
static void notify(struct desktop *d, unsigned event)
{ (void)d; if (!rt.shutting && event < SOUND_EVENTS) (void)push(event,0); }
static int request(struct desktop *d, int client, unsigned op, unsigned event, uint32_t serial)
{
    if (op == DESK_SETTINGS_SOUND_RELOAD) return push(CMD_RELOAD,0);
    if (op == DESK_SETTINGS_SOUND_TEST) return event < SOUND_EVENTS ? push(CMD_TEST,event) : -EINVAL;
    if (op != DESK_SETTINGS_AUDIO_TEST) return -EINVAL;
    pthread_mutex_lock(&rt.lock);
    int r = !rt.live ? -ENODEV : rt.hardware_pending || rt.count == QMAX ? -EBUSY : DESK_SOUND_PENDING;
    if (r == DESK_SOUND_PENDING) {
        rt.hardware_pending = 1; rt.hardware_done = 0; rt.client = client;
        rt.window = d->clients[client].window_id; rt.serial = serial;
        rt.q[(rt.head+rt.count++)%QMAX] = (struct command){CMD_HARDWARE,0}; rt.busy = 1;
        pthread_cond_signal(&rt.changed);
    }
    pthread_mutex_unlock(&rt.lock); return r;
}
static void hardware_result(int result)
{
    pthread_mutex_lock(&rt.lock);
    if (rt.hardware_pending) { rt.hardware_result = result; rt.hardware_done = 1; }
    pthread_mutex_unlock(&rt.lock);
}
void desk_sound_results(struct desktop *d)
{
    pthread_mutex_lock(&rt.lock);
    int ready = rt.hardware_done, i = rt.client, result = rt.hardware_result;
    uint32_t window = rt.window, serial = rt.serial;
    if (ready) rt.hardware_pending = rt.hardware_done = 0;
    pthread_mutex_unlock(&rt.lock);
    if (ready && i >= 0 && i < DESK_CLIENTS && d->clients[i].window && d->clients[i].window_id == window) {
        struct ciuki_message reply; desk_message(&reply, DESK_TASK_RESULT, 16);
        desk_put32(reply.data+4, serial); desk_put32(reply.data+8, DESK_SETTINGS);
        desk_put32(reply.data+12, (uint32_t)result); desk_queue(d,i,&reply);
    }
}
static int shutdown_wait(struct desktop *d)
{
    if (!rt.shutting) {
        rt.shutting = 1; rt.shutdown_deadline = d->now_ns + 2000000000ull;
        pthread_mutex_lock(&rt.lock); rt.count = rt.head = 0; pthread_mutex_unlock(&rt.lock);
        if (push(CMD_SHUTDOWN,0)) return 1;
    }
    pthread_mutex_lock(&rt.lock); int busy = rt.busy; pthread_mutex_unlock(&rt.lock);
    if (busy && d->now_ns < rt.shutdown_deadline) return 0;
    if (busy) (void)push(CMD_STOP,0);
    return 1;
}
static void cancel(struct desktop *d)
{ (void)d; rt.shutting = 0; (void)push(CMD_STOP,0); }
struct worker {
    struct sound_scheme scheme;
    uint8_t files[SOUND_EVENTS][SOUND_FILE_MAX+1]; struct sound_clip clips[SOUND_EVENTS];
    struct sound_player player;
    int16_t tone[SOUND_RATE];
    int fd, hardware; uint64_t hardware_deadline, drain_until;
    uint32_t drain_ms;
    int mixer; struct sound_throttle played;
};
static void reload(struct worker *w, int settings)
{
    if (settings) (void)sound_load_at(&w->scheme,SOUND_CFG,SOUND_LEGACY_CFG,REG_DIRECTORY);
    FILE *f;
    sound_reset(&w->player);
    if (w->fd >= 0) { close(w->fd); w->fd = -1; }
    w->drain_until = 0;
    for (unsigned k = 0; k < SOUND_EVENTS; k++) {
        w->clips[k] = (struct sound_clip){0};
        char path[128]; if (sound_path(&w->scheme,k,path,sizeof(path))) continue;
        f = fopen(path,"rb"); if (!f) continue;
        uint8_t *data = w->files[k];
        size_t n = fread(data,1,SOUND_FILE_MAX+1,f);
        int bad = ferror(f); fclose(f);
        if (bad || sound_wav(data,n,&w->clips[k])) w->clips[k] = (struct sound_clip){0};
    }
}
static int device(struct worker *w)
{
    if (w->fd >= 0) return 0;
    w->fd = open("/dev/audio",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
    if (w->fd < 0) return -errno;
    struct ciuki_audio_caps caps = {.version=1,.size=sizeof(caps)};
    struct ciuki_audio_format fmt = {.version=1,.size=sizeof(fmt),.rate=SOUND_RATE,.format=CIUKI_AUDIO_S16LE,.channels=1};
    if (ciuki_devctl(w->fd,AUDIO_GET_CAPS,&caps,sizeof(caps)) || ciuki_devctl(w->fd,AUDIO_SET_FORMAT,&fmt,sizeof(fmt))) {
        int e = -errno; close(w->fd); w->fd = -1; return e;
    }
    w->drain_ms = caps.drain_ms;
    return 0;
}
static int sink(void *ctx, const int16_t *samples, unsigned n)
{
    struct worker *w = ctx;
    ssize_t r = write(w->fd,samples,n*2);
    if (r < 0) return -errno;
    if (r % 2) return -EIO;
    return (int)r/2;
}
static void stop(struct worker *w)
{
    if (w->fd >= 0) close(w->fd);
    w->fd = -1; w->drain_until = 0; sound_reset(&w->player);
    if (w->hardware) { hardware_result(-ECANCELED); w->hardware = 0; }
}
/* Read-only policy checks stay on the worker, outside the queue lock. Fail
 * closed if the device cannot be observed: never retain a deferred UI sound. */
static int permitted(struct worker *w, int preview)
{
    if (w->mixer < 0) w->mixer = open("/dev/mixer",O_RDONLY|O_CLOEXEC);
    if (w->mixer < 0) return 0;
    struct ciuki_mixer_master m = {.version=1,.size=sizeof(m)};
    struct ciuki_audio_diagnostics d = {.version=1,.size=sizeof(d)};
    if (ciuki_devctl(w->mixer,MIXER_GET_MASTER,&m,sizeof(m)) ||
        ciuki_devctl(w->mixer,MIXER_GET_DIAGNOSTICS,&d,sizeof(d))) {
        close(w->mixer); w->mixer = -1; return 0;
    }
    return sound_allowed(&w->scheme,preview,m.mute != 0,!!(d.flags & CIUKI_AUDIO_DIAG_VM_ACTIVE));
}
static void *worker(void *arg)
{
    struct worker *w = arg;
    w->fd = w->mixer = -1; sound_tone(w->tone); reload(w,0);
    for (;;) {
        struct command cmd = {0}; int have;
        pthread_mutex_lock(&rt.lock);
        have = rt.count != 0;
        if (have) { cmd = rt.q[rt.head]; rt.head = (rt.head+1)%QMAX; rt.count--; }
        pthread_mutex_unlock(&rt.lock);
        if (have) {
            if (cmd.kind == CMD_STOP) stop(w);
            else if (cmd.kind == CMD_RELOAD) { stop(w); reload(w,1); }
            else if (cmd.kind == CMD_HARDWARE) {
                stop(w); w->hardware = 1; w->hardware_deadline = now_ns()+2000000000ull;
                sound_play(&w->player,(struct sound_clip){(const uint8_t *)w->tone,SOUND_RATE});
            } else if (cmd.kind == CMD_SHUTDOWN) {
                stop(w); if (permitted(w,0)) { w->player.volume = (unsigned)w->scheme.volume; sound_play(&w->player,w->clips[SOUND_SHUTDOWN]); }
            } else if (!w->hardware && (w->scheme.enabled || cmd.kind == CMD_TEST)) {
                unsigned e = cmd.kind == CMD_TEST ? cmd.event : cmd.kind;
                if (e < SOUND_EVENTS && permitted(w,cmd.kind == CMD_TEST) &&
                    sound_coalesce(&w->played,e,now_ns())) {
                    w->player.volume = (unsigned)w->scheme.volume;
                    (void)sound_play(&w->player,w->clips[e]);
                }
            }
        }
        if (!w->hardware && w->fd >= 0 && !permitted(w,1)) stop(w);
        if (sound_active(&w->player)) {
            w->drain_until = 0;
            int err = device(w);
            if (!err) {
                struct ciuki_audio_status s = {.version=1,.size=sizeof(s)};
                if (ciuki_devctl(w->fd,AUDIO_GET_STATUS,&s,sizeof(s))) err = -errno;
                else if (s.last_error) err = s.last_error;
                else if (s.queued_frames < 256) err = sound_pump(&w->player,sink,w);
            }
            if (err < 0) { if (w->hardware) hardware_result(err); w->hardware = 0; stop(w); }
        }
        if (w->hardware && (!sound_active(&w->player) || now_ns() >= w->hardware_deadline)) {
            hardware_result(sound_active(&w->player) ? -ETIMEDOUT : 0); w->hardware = 0;
            if (sound_active(&w->player)) stop(w);
        }
        if (!sound_active(&w->player) && w->fd >= 0) {
            struct ciuki_audio_status s = {.version=1,.size=sizeof(s)};
            if (ciuki_devctl(w->fd,AUDIO_GET_STATUS,&s,sizeof(s)) || s.last_error) stop(w);
            else if (s.consumed_frames >= s.accepted_frames) {
                if (!w->drain_until) w->drain_until = now_ns() + (uint64_t)w->drain_ms*1000000ull +
                    (uint64_t)s.transport_frames*1000000000ull/s.output_rate;
                if (now_ns() >= w->drain_until) stop(w);
            }
        }
        pthread_mutex_lock(&rt.lock);
        rt.busy = sound_active(&w->player) || w->fd >= 0 || rt.count;
        if (!rt.count) {
            if (rt.busy) {
                struct timespec until; clock_gettime(CLOCK_REALTIME,&until); until.tv_nsec += 2000000;
                if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&rt.changed,&rt.lock,&until);
            } else pthread_cond_wait(&rt.changed,&rt.lock);
        }
        pthread_mutex_unlock(&rt.lock);
    }
    return NULL;
}
void desk_sound_init(struct desktop *d)
{
    struct worker *w=calloc(1,sizeof(*w));
    if (!w) { fprintf(stderr,"desktop: sound storage unavailable\n"); return; }
    (void)sound_load_at(&w->scheme,SOUND_CFG,SOUND_LEGACY_CFG,REG_DIRECTORY);
    pthread_t thread; rt.live = 1;
    int r = pthread_create(&thread,NULL,worker,w);
    if (r) { rt.live = 0; free(w); fprintf(stderr,"desktop: sound worker unavailable (%d)\n",r); return; }
    pthread_detach(thread);
    d->sound_event = notify; d->sound_request = request;
    d->sound_shutdown = shutdown_wait; d->sound_cancel = cancel;
}
