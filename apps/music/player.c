/* SPDX-License-Identifier: MIT */
#include "player.h"
#include <errno.h>
#include <string.h>
static void close_device(struct music_player *p)
{
    if (p->device) p->output->close(p->output->ctx);
    p->device=0; p->head=p->count=0; p->eof=0; p->tail_deadline=0;
}
static int fail(struct music_player *p, int error)
{ close_device(p); music_decoder_close(p->decoder); p->decoder=NULL; p->view.state=MUSIC_ERROR; p->view.error=error; return error; }
static int position(struct music_player *p, uint64_t *tail)
{
    if (!p->device) { *tail=0; return 0; }
    uint64_t consumed; unsigned queued;
    int r=p->output->status(p->output->ctx,&consumed,&queued,tail); if (r) return r;
    p->view.position=p->base+consumed;
    if (p->view.position>p->view.duration) p->view.position=p->view.duration;
    return (int)queued;
}
static int open_device(struct music_player *p)
{
    if (p->device) return 0;
    unsigned rate=0; int r=p->output->open(p->output->ctx,&rate);
    if (r) return r;
    p->device=1;
    if (!rate || (p->view.rate && p->view.rate!=rate)) { close_device(p); return -EIO; }
    p->view.rate=rate; p->base=p->view.position; return 0;
}
static int load(struct music_player *p)
{
    close_device(p); music_decoder_close(p->decoder); p->decoder=NULL;
    p->view.position=p->view.duration=0; p->view.error=0; p->view.state=MUSIC_LOADING;
    if (!p->view.playlist.count) { p->view.state=MUSIC_STOPPED; return 0; }
    int r=open_device(p); if (r) return fail(p,r);
    r=music_decoder_open(&p->decoder,p->view.playlist.path[p->view.current],p->view.rate,p->cancel,p->ctx);
    if (r) return fail(p,r);
    p->view.duration=music_decoder_length(p->decoder); p->view.state=MUSIC_PLAYING; return 0;
}
void music_player_init(struct music_player *p, const struct music_output *output, music_cancel cancel, void *ctx)
{ memset(p,0,sizeof(*p)); p->output=output; p->cancel=cancel; p->ctx=ctx; }
int music_player_command(struct music_player *p, const struct music_command *c)
{
    int r=0;
    switch (c->action) {
    case MUSIC_OPEN:
        close_device(p); music_decoder_close(p->decoder); p->decoder=NULL;
        p->view.playlist.count=0; p->view.current=0; p->view.position=p->view.duration=0;
        p->view.revision++;
        r=music_playlist_add(&p->view.playlist,c->path,p->cancel,p->ctx);
        if (r) return fail(p,r);
        return load(p);
    case MUSIC_ADD:
        r=music_playlist_add(&p->view.playlist,c->path,p->cancel,p->ctx);
        if (!r) p->view.revision++;
        p->view.error=r; return r;
    case MUSIC_SAVE:
        r=music_playlist_save(&p->view.playlist,c->path,p->cancel,p->ctx); p->view.error=r; return r;
    case MUSIC_CLEAR:
        close_device(p); music_decoder_close(p->decoder); p->decoder=NULL;
        p->view.playlist.count=0; p->view.current=0; p->view.position=p->view.duration=0;
        p->view.revision++; p->view.state=MUSIC_STOPPED; p->view.error=0; return 0;
    case MUSIC_SELECT:
        if (c->value>=p->view.playlist.count) return -EINVAL;
        p->view.current=(unsigned)c->value; return load(p);
    case MUSIC_PREV: case MUSIC_NEXT:
        if (!p->view.playlist.count) return 0;
        p->view.current=(p->view.current+p->view.playlist.count+(c->action==MUSIC_NEXT ? 1 : -1))%p->view.playlist.count;
        return load(p);
    case MUSIC_PLAY:
        if (p->view.state==MUSIC_PLAYING) return 0;
        if (!p->decoder || p->view.position>=p->view.duration) return load(p);
        r=open_device(p); if (r) return fail(p,r);
        p->view.state=MUSIC_PLAYING; p->view.error=0; return 0;
    case MUSIC_PAUSE:
        if (p->view.state!=MUSIC_PLAYING) return 0;
        { uint64_t tail; r=position(p,&tail); }
        if (r<0) return fail(p,r);
        close_device(p); r=music_decoder_seek(p->decoder,p->view.position);
        if (r) return fail(p,r);
        p->view.state=MUSIC_PAUSED; return 0;
    case MUSIC_STOP:
        close_device(p); p->view.position=0; p->view.error=0;
        if (p->decoder && (r=music_decoder_seek(p->decoder,0))) return fail(p,r);
        p->view.state=MUSIC_STOPPED; return 0;
    case MUSIC_SEEK: {
        if (!p->decoder) return 0;
        enum music_state state=p->view.state;
        close_device(p); p->view.position=c->value>p->view.duration ? p->view.duration : c->value;
        r=music_decoder_seek(p->decoder,p->view.position); if (r) return fail(p,r);
        if (state==MUSIC_PLAYING && (r=open_device(p))) return fail(p,r);
        return 0;
    }
    }
    return -EINVAL;
}
int music_player_pump(struct music_player *p)
{
    if (p->view.state!=MUSIC_PLAYING) return 0;
    uint64_t tail; int queued=position(p,&tail);
    if (queued<0) return fail(p,queued);
    if (!p->eof && p->count<MUSIC_RING-512) {
        unsigned end=(p->head+p->count)%MUSIC_RING, space=MUSIC_RING-end;
        if (space>512) space=512;
        int n=music_decoder_read(p->decoder,p->ring+2*end,space);
        if (n<0) return fail(p,n);
        if (!n) p->eof=1; else p->count+=(unsigned)n;
    }
    if (p->count) {
        unsigned n=MUSIC_RING-p->head; if (n>p->count) n=p->count; if (n>512) n=512;
        int r=p->output->write(p->output->ctx,p->ring+2*p->head,n);
        if (r<0 && r!=-EAGAIN && r!=-EINTR) return fail(p,r);
        if (r>0) {
            if ((unsigned)r>n) return fail(p,-EIO);
            p->head=(p->head+(unsigned)r)%MUSIC_RING; p->count-=(unsigned)r;
        }
    } else if (p->eof && !queued) {
        uint64_t now=p->output->now(p->output->ctx);
        if (!p->tail_deadline) p->tail_deadline=now+tail+1;
        if (now>=p->tail_deadline) {
            p->view.position=p->view.duration;
            close_device(p);
            if (p->view.current+1<p->view.playlist.count) { p->view.current++; return load(p); }
            p->view.state=MUSIC_STOPPED;
        }
    }
    return 0;
}
void music_player_destroy(struct music_player *p)
{ close_device(p); music_decoder_close(p->decoder); p->decoder=NULL; p->view.state=MUSIC_CLOSING; }
