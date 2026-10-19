/* SPDX-License-Identifier: MIT
 * Exercise the real native /dev/audio and desktop mixer adapters. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
static int fake_open(const char *, int, ...);
static ssize_t fake_write(int,const void *,size_t);
static int fake_close(int);
#define open fake_open
#define write fake_write
#define close fake_close
#define main music_native_main
#include "../../../apps/music/main.c"
#undef main
#undef close
#undef write
#undef open
static unsigned view_updates;
void music_update(struct music_app *a, const struct music_snapshot *s)
{ assert(a && s==&a->snapshot); view_updates++; }
static int audio_closed, mixer_closed, blocked, malformed, fail_control, queue_failed;
static unsigned fake_rate=48000, fake_queued=64;
static unsigned idle_transport;
static struct ciuki_message sent;
static int fake_open(const char *path, int flags, ...)
{
    if (!strcmp(path,"/dev/audio")) { assert(flags==(O_WRONLY|O_NONBLOCK|O_CLOEXEC)); return 40; }
    assert(!strcmp(path,"/dev/mixer") && flags==(O_RDONLY|O_CLOEXEC)); return 41;
}
static ssize_t fake_write(int fd, const void *pcm, size_t bytes)
{ assert(fd==40 && pcm && bytes%4==0); if (blocked) { errno=EAGAIN; return -1; } return malformed ? 3 : (ssize_t)bytes; }
static int fake_close(int fd) { if (fd==40) audio_closed++; else { assert(fd==41); mixer_closed++; } return 0; }
int ciuki_devctl(int fd, uint32_t op, void *record, uint32_t bytes)
{
    uint32_t *header=record; assert(header[0]==1 && header[1]==bytes);
    if (fail_control) { errno=ENODEV; return -1; }
    if (op==AUDIO_GET_CAPS) {
        assert(fd==40 && bytes==sizeof(struct ciuki_audio_caps)); struct ciuki_audio_caps *c=record;
        c->output_rate=fake_rate; c->drain_ms=20;
    } else if (op==AUDIO_SET_FORMAT) {
        assert(fd==40); struct ciuki_audio_format *f=record;
        assert(f->channels==2 && f->format==CIUKI_AUDIO_S16LE && f->rate==fake_rate); f->output_rate=fake_rate;
    } else if (op==AUDIO_GET_STATUS) {
        assert(fd==40); struct ciuki_audio_status *s=record;
        s->consumed_frames=5000; s->queued_frames=fake_queued; s->output_rate=fake_rate; s->transport_frames=480+idle_transport; s->latency_frames=fake_queued+480+fake_rate/50;
    } else {
        assert(fd==41 && op==MIXER_GET_MASTER); struct ciuki_mixer_master *m=record;
        m->left=40; m->right=80; m->mute=1;
    }
    return 0;
}
int du_window_queue(struct du_window *w, struct ciuki_message *msg)
{ (void)w; if (queue_failed) { errno=EAGAIN; return -1; } sent=*msg; return 0; }
int main(void)
{
    struct music_main *m=calloc(1,sizeof(*m)); assert(m); m->audio=m->mixer=-1;
    unsigned rate; assert(!audio_open(m,&rate) && rate==48000);
    int16_t pcm[8]={0}; assert(audio_write(m,pcm,4)==4);
    blocked=1; assert(audio_write(m,pcm,4)==-EAGAIN); blocked=0;
    malformed=1; assert(audio_write(m,pcm,4)==-EIO); malformed=0;
    uint64_t consumed,tail; unsigned queued;
    assert(!audio_status(m,&consumed,&queued,&tail) && consumed==3560 && queued==64 && tail==30000000);
    fake_queued=2048;
    assert(!audio_status(m,&consumed,&queued,&tail) && consumed==3560 && queued==2048 && tail==30000000);
    fake_queued=64;
    idle_transport=6400;
    assert(!audio_status(m,&consumed,&queued,&tail) && consumed==3560 && tail==30000000);
    audio_close(m); assert(m->audio==-1 && audio_closed==1);
    fail_control=1; assert(audio_open(m,&rate)==-ENODEV && m->audio==-1 && audio_closed==2); fail_control=0;
    assert(!master_get(m,&rate) && rate==80);
    assert(!master_set(m,50) && m->app.waiting);
    assert(sent.data[1]==DESK_MIXER_SET && desk_get32(sent.data+8)==25 && desk_get32(sent.data+12)==50 && desk_get32(sent.data+16)==1);
    struct desk_packet result={.opcode=DESK_TASK_RESULT,.request=DESK_MIXER_SET,.serial=m->sent,.result=-EPERM};
    message(&m->window,&result,m); assert(!m->app.waiting && m->app.local_error==-EPERM && view_updates==1);
    queue_failed=1; assert(master_set(m,50)==-EAGAIN);
    fake_close(m->mixer); assert(mixer_closed==1); free(m);
    puts("PASS native adapters: nonblocking S16 stereo, queried device rate, partial/error writes, transport tail, mixer IPC balance/mute/errors"); return 0;
}
