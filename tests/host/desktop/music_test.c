/* SPDX-License-Identifier: MIT */
#include "../../../apps/music/player.h"
#include "../../../apps/desktop/catalog.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
static int cancel_now(void *ctx) { return *(int *)ctx; }
static void path(char *out, const char *dir, const char *name) { snprintf(out,MUSIC_PATH,"%s/%s",dir,name); }
static void decoders(const char *fixtures, const char *scratch)
{
    const char *names[]={"pcm-8-1-6000.wav","pcm-8-2-6000.wav","pcm-16-1-6000.wav","pcm-16-2-6000.wav",
        "pcm-8-1-96000.wav","pcm-8-2-96000.wav","pcm-16-1-96000.wav","pcm-16-2-96000.wav","tone.mp3","tone.ogg"};
    int16_t *all=calloc(48000*2,sizeof(*all)); assert(all);
    for (unsigned i=0;i<10;i++) {
        char file[MUSIC_PATH]; path(file,fixtures,names[i]); struct music_decoder *d=NULL;
        assert(!music_decoder_open(&d,file,48000,NULL,NULL));
        uint64_t length=music_decoder_length(d); assert(length>=4800 && length<9000);
        if (i<8) assert(length==4800 && music_decoder_rate(d)==(i<4 ? 6000 : 96000));
        unsigned total=0; int n;
        while ((n=music_decoder_read(d,all+total*2,137))>0) { total+=(unsigned)n; assert(total<48000); }
        assert(!n && total==length);
        int nonzero=0, distinct=0;
        for (unsigned k=0;k<total;k++) { nonzero|=all[k*2]!=0; distinct|=all[k*2]!=all[k*2+1]; }
        assert(nonzero); if (i<8 && !(i&1)) assert(!distinct); else assert(distinct);
        assert(!music_decoder_seek(d,length/2)); int16_t samples[100];
        assert(music_decoder_read(d,samples,50)==50);
        assert(!memcmp(samples,all+length/2*2,sizeof(samples)));
        assert(!music_decoder_seek(d,length)); assert(!music_decoder_read(d,samples,50));
        assert(!music_decoder_seek(d,0)); assert(music_decoder_read(d,samples,50)==50);
        assert(!memcmp(samples,all,sizeof(samples)));
        music_decoder_close(d);
        int cancel=1; assert(music_decoder_open(&d,file,48000,cancel_now,&cancel)==-ECANCELED && !d);
    }
    /* Order-independent RIFF with odd unknown-chunk padding, 8-bit mono. */
    unsigned char wav[]={ 'R','I','F','F',50,0,0,0,'W','A','V','E',
        'J','U','N','K',1,0,0,0,42,0,'d','a','t','a',4,0,0,0,0,128,255,64,
        'f','m','t',' ',16,0,0,0,1,0,1,0,0x80,0xbb,0,0,0x80,0xbb,0,0,1,0,8,0 };
    char f[MUSIC_PATH]; path(f,scratch,"chunks.wav"); FILE *out=fopen(f,"wb"); assert(out);
    assert(fwrite(wav,1,sizeof(wav),out)==sizeof(wav)); fclose(out);
    struct music_decoder *d; assert(!music_decoder_open(&d,f,48000,NULL,NULL)); int16_t pcm[8];
    assert(music_decoder_read(d,pcm,4)==4); assert(pcm[0]==-32768 && pcm[2]==0 && pcm[4]==32512 && pcm[6]==-16384);
    music_decoder_close(d);
    wav[16]=255; out=fopen(f,"wb"); assert(out); fwrite(wav,1,sizeof(wav),out); fclose(out);
    assert(music_decoder_open(&d,f,48000,NULL,NULL)==-EINVAL && !d);
    path(f,scratch,"bad.mp3"); out=fopen(f,"wb"); assert(out); fputs("not MP3",out); fclose(out);
    assert(music_decoder_open(&d,f,48000,NULL,NULL)==-EINVAL && !d);
    path(f,scratch,"bad.ogg"); out=fopen(f,"wb"); assert(out); fputs("not Ogg",out); fclose(out);
    assert(music_decoder_open(&d,f,48000,NULL,NULL)==-EINVAL && !d);
    /* A tag larger than two compressed windows is skipped without decoding
     * artwork. Without a Xing count the duration becomes exact at EOF. */
    char original[MUSIC_PATH]; path(original,fixtures,"tone.mp3");
    FILE *in=fopen(original,"rb"); assert(in); unsigned char encoded[8192];
    size_t encoded_size=fread(encoded,1,sizeof(encoded),in); assert(feof(in)); fclose(in);
    for (size_t i=0;i+4<=encoded_size;i++) if (!memcmp(encoded+i,"Xing",4) || !memcmp(encoded+i,"Info",4)) memcpy(encoded+i,"Junk",4);
    path(f,scratch,"large-tag.mp3"); out=fopen(f,"wb"); assert(out);
    unsigned char tag[10]={'I','D','3',4,0,0,0,4,0,0}; /* 65536 bytes of payload */
    fwrite(tag,1,sizeof(tag),out); for (unsigned i=0;i<65536;i++) fputc(0,out);
    fwrite(encoded,1,encoded_size,out); fclose(out);
    assert(!music_decoder_open(&d,f,48000,NULL,NULL) && !music_decoder_length(d));
    unsigned total=0; int n;
    while ((n=music_decoder_read(d,all+total*2,137))>0) { total+=(unsigned)n; assert(total<48000); }
    assert(!n && total>4800 && music_decoder_length(d)==total);
    assert(!music_decoder_seek(d,total/2)); int16_t samples[100];
    assert(music_decoder_read(d,samples,50)==50 && !memcmp(samples,all+total/2*2,sizeof(samples)));
    music_decoder_close(d);
    assert(!music_decoder_open(&d,f,48000,NULL,NULL) && !music_decoder_length(d));
    assert(!music_decoder_seek(d,48000*3600) && music_decoder_length(d)==total);
    assert(!music_decoder_read(d,samples,50)); /* clamp a seek beyond unknown EOF */
    music_decoder_close(d);
    free(all);
}
static void playlists(const char *fixtures, const char *scratch)
{
    struct music_playlist *p=calloc(1,sizeof(*p)), *q=calloc(1,sizeof(*q)); assert(p && q);
    const char *txt="\xef\xbb\xbf#EXTM3U\r\n#EXTINF:1,Tone\r\ntone.mp3\r\n./tone.ogg\n../music/tone.mp3\n";
    char base[MUSIC_PATH]; path(base,fixtures,"list.m3u");
    assert(!music_playlist_parse(p,base,txt,strlen(txt)) && p->count==2);
    assert(strstr(p->path[0],"/tone.mp3") && strstr(p->path[1],"/tone.ogg"));
    assert(music_playlist_parse(p,base,"https://invalid/a.mp3\n",22)==-EINVAL);
    char normalized[MUSIC_PATH]; assert(!music_path(normalized,"/a/b/list.m3u","../../c/./d/../e.mp3") && !strcmp(normalized,"/c/e.mp3"));
    assert(music_path(normalized,NULL,"bad\nname.mp3")==-EINVAL);
    assert(music_playlist_parse(p,base,"a\0.mp3",6)==-EINVAL);
    path(base,scratch,"saved.m3u"); assert(!music_playlist_save(p,base,NULL,NULL));
    assert(!music_playlist_add(q,base,NULL,NULL) && q->count==2 && !memcmp(p,q,sizeof(*p)));
    assert(!music_playlist_save(p,base,NULL,NULL)); /* replace */
    int cancel=1; assert(music_playlist_save(p,base,cancel_now,&cancel)==-ECANCELED);
    q->count=0; assert(!music_playlist_add(q,base,NULL,NULL) && q->count==2); /* original survived */
    q->count=0; assert(!music_playlist_add(q,fixtures,NULL,NULL) && q->count==10);
    assert(music_playlist_add(q,fixtures,cancel_now,&cancel)==-ECANCELED && q->count==10);
    char folder[MUSIC_PATH]; path(folder,scratch,"sub"); assert(!mkdir(folder,0700));
    char link[MUSIC_PATH]; path(link,folder,"loop"); assert(!symlink(folder,link));
    assert(!music_playlist_add(q,folder,NULL,NULL)); /* symlink never recurses */
    p->count=0;
    for (unsigned i=0;i<MUSIC_TRACKS;i++) { char entry[MUSIC_PATH]; snprintf(entry,sizeof(entry),"/song-%u.mp3",i); assert(!music_playlist_append(p,entry)); }
    assert(music_playlist_append(p,"/one-too-many.mp3")==-E2BIG);
    free(p); free(q);
}
struct sink { uint64_t consumed, accepted; _Atomic uint64_t now; unsigned queued, opens, closes; int blocked, error; int16_t last[34]; };
static int sink_open(void *ctx, unsigned *rate) { struct sink *s=ctx; if (s->error) return s->error; s->opens++; s->consumed=s->accepted=s->queued=0; *rate=48000; return 0; }
static int sink_write(void *ctx, const int16_t *pcm, unsigned n) { struct sink *s=ctx; if (s->blocked) return -EAGAIN; if (n>17) n=17; memcpy(s->last,pcm,n*2*sizeof(*pcm)); s->queued+=n; s->accepted+=n; return (int)n; }
static int sink_status(void *ctx, uint64_t *consumed, unsigned *queued, uint64_t *tail)
{ struct sink *s=ctx; if (s->error) return s->error; *consumed=s->consumed; *queued=s->queued; *tail=40000000; return 0; }
static void sink_close(void *ctx) { struct sink *s=ctx; s->closes++; s->queued=0; }
static uint64_t sink_now(void *ctx) { return ((struct sink *)ctx)->now; }
static void consume(struct sink *s) { s->consumed+=s->queued; s->queued=0; s->now+=2000000; }
static void tick(struct music_player *p, struct sink *s)
{
    consume(s); assert(!music_player_pump(p));
    struct timespec delay={0,100000}; nanosleep(&delay,NULL);
}
static void ready(struct music_player *p)
{
    struct timespec delay={0,1000000};
    for (unsigned i=0;i<2000 && p->view.state==MUSIC_LOADING;i++) {
        assert(!music_player_pump(p)); nanosleep(&delay,NULL);
    }
    assert(p->view.state==MUSIC_PLAYING);
}
static void engine(const char *fixtures, const char *scratch)
{
    _Static_assert(sizeof(((struct music_player *)0)->ring)==512*1024,"reservoir budget");
    struct music_player *p=calloc(1,sizeof(*p)); assert(p);
    struct sink s={.blocked=1}; struct music_output o={sink_open,sink_write,sink_status,sink_close,sink_now,&s,NULL,NULL};
    music_player_init(p,&o,NULL,NULL); struct music_command c={.action=MUSIC_OPEN}; path(c.path,fixtures,"tone.ogg");
    assert(!music_player_command(p,&c) && p->view.state==MUSIC_LOADING && p->view.playlist.count==1);
    ready(p);
    for (unsigned i=0;i<100;i++) { assert(!music_player_pump(p)); assert(s.queued<=2048); }
    assert(p->view.reservoir_frames>=4800 && !s.accepted && !p->view.position);
    s.blocked=0;
    for(unsigned i=0;i<200;i++) { assert(!music_player_pump(p)); assert(s.queued<=2048); }
    assert(s.queued>=1792); /* device lead excludes the decoded reservoir */
    unsigned lead=s.queued;
    tick(p,&s); assert(p->view.position==lead);
    int16_t pending[34]; memcpy(pending,s.last,sizeof(pending));
    c.action=MUSIC_PAUSE; assert(!music_player_command(p,&c));
    assert(p->view.state==MUSIC_PAUSED && !p->device && p->view.reservoir_frames>0 && p->view.position==lead);
    uint64_t paused=p->view.position; music_player_pump(p); assert(p->view.position==paused);
    c.action=MUSIC_PLAY; assert(!music_player_command(p,&c) && p->device);
    assert(!music_player_pump(p) && !memcmp(s.last,pending,sizeof(pending)));
    c.action=MUSIC_SEEK; c.value=2400; assert(!music_player_command(p,&c) && p->view.position==2400);
    ready(p); tick(p,&s);
    c.action=MUSIC_STOP; assert(!music_player_command(p,&c) && !p->view.position && p->view.state==MUSIC_STOPPED && !p->device && !p->count);
    c.action=MUSIC_PLAY; assert(!music_player_command(p,&c)); ready(p); /* restart reader after stop */
    c.action=MUSIC_ADD; path(c.path,fixtures,"tone.mp3"); assert(!music_player_command(p,&c) && p->view.playlist.count==2);
    c.action=MUSIC_NEXT; assert(!music_player_command(p,&c) && p->view.current==1); ready(p);
    c.action=MUSIC_PREV; assert(!music_player_command(p,&c) && p->view.current==0); ready(p);
    /* EOF auto-advances only after queued audio and transport tail have ended. */
    for (unsigned i=0;i<3000 && p->view.current==0;i++) tick(p,&s);
    assert(p->view.current==1); ready(p);
    for (unsigned i=0;i<3000 && p->view.state==MUSIC_PLAYING;i++) tick(p,&s);
    assert(p->view.state==MUSIC_STOPPED && p->view.position==p->view.duration);
    s.error=-ENODEV; c.action=MUSIC_PLAY; assert(music_player_command(p,&c)==-ENODEV && p->view.state==MUSIC_ERROR);
    s.error=0; c.action=MUSIC_PLAY; assert(!music_player_command(p,&c)); ready(p);
    c.action=MUSIC_CLEAR; assert(!music_player_command(p,&c) && !p->view.playlist.count && !p->decoder);
    c.action=MUSIC_OPEN; path(c.path,scratch,"large-tag.mp3");
    assert(!music_player_command(p,&c));
    c.action=MUSIC_SEEK; c.value=48000*3600;
    assert(!music_player_command(p,&c) && p->view.duration && p->view.position==p->view.duration);
    ready(p);
    music_player_destroy(p); assert(p->view.state==MUSIC_CLOSING && s.opens==s.closes); free(p);
}
static void runtime(const char *fixtures)
{
    struct sink s={.blocked=1}; struct music_output o={sink_open,sink_write,sink_status,sink_close,sink_now,&s,NULL,NULL};
    struct music_runtime *rt=music_runtime_start(&o); assert(rt);
    struct music_command c={.action=MUSIC_OPEN}; path(c.path,fixtures,"tone.mp3"); assert(!music_runtime_send(rt,&c));
    struct music_snapshot *snapshot=calloc(1,sizeof(*snapshot)); assert(snapshot);
    struct timespec delay={0,1000000};
    for (unsigned i=0;i<5000;i++) { music_runtime_snapshot(rt,snapshot); if (snapshot->state==MUSIC_PLAYING) break; nanosleep(&delay,NULL); }
    assert(snapshot->state==MUSIC_PLAYING); music_runtime_close(rt);
    for (unsigned i=0;i<5000 && !music_runtime_done(rt);i++) nanosleep(&delay,NULL);
    assert(music_runtime_done(rt) && music_runtime_send(rt,&c)==-ECANCELED);
    music_runtime_destroy(rt); assert(s.opens==s.closes); free(snapshot);
}
int main(int argc, char **argv)
{
    assert(argc==3); decoders(argv[1],argv[2]); playlists(argv[1],argv[2]); engine(argv[1],argv[2]); runtime(argv[1]);
    const char *exts[]={"wav","mp3","ogg","m3u","bmp","png","jpg","gif"};
    for (unsigned i=0;i<8;i++) { char name[32]; snprintf(name,sizeof(name),"/file.%s",exts[i]); const struct desk_catalog_entry *e=desk_catalog_file(name); assert(e && !strcmp(e->name,i<4 ? "music" : "viewer")); }
    assert(!strcmp(desk_catalog_file("/Music.TONE.MP3")->name,"music"));
    puts("PASS music: decoders/resampling/seek, malformed RIFF, playlists, backpressure, transitions, EOF, worker close, catalog"); return 0;
}
