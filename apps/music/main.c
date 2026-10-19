/* SPDX-License-Identifier: MIT
 * /Programs/music [--channel-fd=N] [--test=music] [file, playlist or folder]. */
#include "view.h"
#include <ciuki/audio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct music_main {
    struct du_window window; struct du_ui ui; struct music_app app;
    struct music_runtime *runtime; struct music_output output; struct music_ui_backend backend;
    struct music_snapshot snapshot; int audio, mixer, closing; unsigned balance_left, balance_right, mute;
    uint32_t serial, sent; uint32_t drain_ms; uint64_t last_poll;
};
static int audio_open(void *ctx, unsigned *rate)
{
    struct music_main *m=ctx;
    m->audio=open("/dev/audio",O_WRONLY|O_NONBLOCK|O_CLOEXEC); if (m->audio<0) return -errno;
    struct ciuki_audio_caps caps={.version=1,.size=sizeof(caps)};
    int err=ciuki_devctl(m->audio,AUDIO_GET_CAPS,&caps,sizeof(caps)) ? -errno : 0;
    struct ciuki_audio_format fmt={.version=1,.size=sizeof(fmt),.rate=caps.output_rate,.format=CIUKI_AUDIO_S16LE,.channels=2};
    if (!err && ciuki_devctl(m->audio,AUDIO_SET_FORMAT,&fmt,sizeof(fmt))) err=-errno;
    if (err) { close(m->audio); m->audio=-1; return err; }
    *rate=fmt.output_rate; m->drain_ms=caps.drain_ms; return 0;
}
static int audio_write(void *ctx, const int16_t *samples, unsigned frames)
{
    struct music_main *m=ctx; ssize_t n=write(m->audio,samples,frames*4);
    return n<0 ? -errno : n%4 ? -EIO : (int)n/4;
}
static int audio_status(void *ctx, uint64_t *consumed, unsigned *queued, uint64_t *tail)
{
    struct music_main *m=ctx; struct ciuki_audio_status s={.version=1,.size=sizeof(s)};
    if (ciuki_devctl(m->audio,AUDIO_GET_STATUS,&s,sizeof(s))) return -errno;
    if (s.last_error) return s.last_error;
    if (!s.output_rate) return -EIO;
    /* Format is the physical output rate. Subtract pending DMA and codec
     * allowance from mixer consumption to display audible position. */
    uint32_t pending=s.latency_frames>s.queued_frames ? s.latency_frames-s.queued_frames : 0;
    *consumed=s.consumed_frames>pending ? s.consumed_frames-pending : 0;
    *queued=s.queued_frames;
    *tail=(uint64_t)pending*1000000000/s.output_rate; return 0;
}
static void audio_close(void *ctx) { struct music_main *m=ctx; if (m->audio>=0) close(m->audio); m->audio=-1; }
static uint64_t now(void *ctx) { (void)ctx; return du_now(); }
static int send(void *ctx, const struct music_command *c) { return music_runtime_send(((struct music_main *)ctx)->runtime,c); }
static int master_get(void *ctx, unsigned *volume)
{
    struct music_main *m=ctx;
    if (m->mixer<0) m->mixer=open("/dev/mixer",O_RDONLY|O_CLOEXEC);
    if (m->mixer<0) return -errno;
    struct ciuki_mixer_master master={.version=1,.size=sizeof(master)};
    if (ciuki_devctl(m->mixer,MIXER_GET_MASTER,&master,sizeof(master))) return -errno;
    m->balance_left=master.left; m->balance_right=master.right; m->mute=master.mute;
    *volume=master.left>master.right ? master.left : master.right; return 0;
}
static int master_set(void *ctx, unsigned volume)
{
    struct music_main *m=ctx; unsigned old;
    int err=master_get(ctx,&old); if (err) return err;
    unsigned left=old ? m->balance_left*volume/old : volume, right=old ? m->balance_right*volume/old : volume;
    struct ciuki_message msg; desk_message(&msg,DESK_MIXER_SET,20);
    if (!++m->serial) ++m->serial;
    desk_put32(msg.data+4,m->serial); desk_put32(msg.data+8,left); desk_put32(msg.data+12,right); desk_put32(msg.data+16,m->mute);
    if (du_window_queue(&m->window,&msg)) return -errno;
    m->sent=m->serial; m->app.waiting=1; return 0;
}
static int close_request(struct du_window *w, void *ctx)
{
    (void)w; struct music_main *m=ctx; m->closing=1; music_runtime_close(m->runtime); return 0;
}
static void quit(void *ctx) { struct music_main *m=ctx; close_request(&m->window,m); }
static void message(struct du_window *w, const struct desk_packet *p, void *ctx)
{
    (void)w; struct music_main *m=ctx;
    if (p->opcode==DESK_TASK_RESULT && p->request==DESK_MIXER_SET && p->serial==m->sent) {
        m->app.waiting=0; m->app.local_error=p->result; music_update(&m->app,&m->app.snapshot);
    }
}
static void painted(struct du_ui *u, void *ctx)
{
    (void)u; struct music_app *a=ctx; struct music_main *m=a->backend->ctx;
    music_runtime_repaint(m->runtime);
}
static void idle(void *ctx)
{
    struct music_main *m=ctx;
    if (m->closing && music_runtime_done(m->runtime)) { du_window_close(&m->window); return; }
    uint64_t tick=du_now(); if (tick-m->last_poll<100000000ull) return; m->last_poll=tick;
    music_runtime_snapshot(m->runtime,&m->snapshot);
    if (m->snapshot.publication!=m->app.snapshot.publication) music_update(&m->app,&m->snapshot);
}
int main(int argc, char **argv)
{
    int channel,test; const char *path;
    if (acc_args(argc,argv,"music",&channel,&test,&path)) return 2;
    struct music_main *m=calloc(1,sizeof(*m)); if (!m) return 1;
    m->audio=m->mixer=-1;
    m->output=(struct music_output){audio_open,audio_write,audio_status,audio_close,now,m};
    m->backend=(struct music_ui_backend){send,master_get,master_set,quit,m};
    m->runtime=music_runtime_start(&m->output);
    if (!m->runtime) { free(m); return 1; }
    int w,h; acc_window_size(560,420,&w,&h);
    if (du_window_open_resizable(&m->window,&m->ui,channel,w,h,"Music Player",420,300)) {
        fprintf(stderr,"music: desktop connection failed: %s\n",strerror(errno)); music_runtime_destroy(m->runtime); free(m); return 1;
    }
    m->window.close_request=close_request; m->window.message=message; m->window.user=m;
    music_attach(&m->app,&m->ui,&m->backend);
    m->ui.paint=painted;
    if (path) {
        struct music_command c={.action=MUSIC_OPEN}; snprintf(c.path,sizeof(c.path),"%s",path);
        m->app.local_error=strlen(path)>=MUSIC_PATH ? -ENAMETOOLONG : send(m,&c);
        if (!m->app.local_error) m->snapshot.state=MUSIC_LOADING;
        music_update(&m->app,&m->snapshot);
    }
    if (test) du_window_move(&m->window,24,48);
    int r=du_run(&m->window,idle,m);
    music_runtime_destroy(m->runtime); if (m->mixer>=0) close(m->mixer);
    du_window_destroy(&m->window); free(m); return r<0 ? 1 : 0;
}
