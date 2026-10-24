/* CD Player's MCI controller, shared with the host harness.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_CDPLAYER_MODEL_H
#define CIUKI_CDPLAYER_MODEL_H
#include <stdint.h>
struct cdp_player {
    uint32_t (*command)(void *,const char *,char *,uint32_t);
    void *ctx;
    unsigned tracks,track,repeat,random,seed;
    uint8_t audio[100],visited[100];
    char length[100][32];
};
static inline char *cdp_text(char *out,const char *text)
{ while (*text) *out++=*text++; *out=0; return out; }
static inline char *cdp_track(char *out,unsigned track)
{ *out++=(char)('0'+track/10); *out++=(char)('0'+track%10); *out=0; return out; }
static inline uint32_t cdp_load(struct cdp_player *p)
{
    char count[32]={0};
    uint32_t e=p->command(p->ctx,"status cd number of tracks",count,sizeof(count));
    p->tracks=p->track=0;
    for (unsigned i=0;i<100;i++) { p->audio[i]=p->visited[i]=0; p->length[i][0]=0; }
    if (e) return e;
    for (const char *c=count;*c;c++) {
        if (*c<'0' || *c>'9' || p->tracks>9) return 1;
        p->tracks=p->tracks*10+(unsigned)(*c-'0');
    }
    if (!p->tracks || p->tracks>99) { p->tracks=0; return 1; }
    for (unsigned i=1;i<=p->tracks;i++) {
        char q[64],type[32]={0}; cdp_track(cdp_text(q,"status cd type track "),i);
        if (p->command(p->ctx,q,type,sizeof(type))) continue;
        if (type[0]!='a') continue; /* no data track is ever selected for PLAY */
        cdp_track(cdp_text(q,"status cd length track "),i);
        if (p->command(p->ctx,q,p->length[i],sizeof(p->length[i]))) continue;
        p->audio[i]=1; if (!p->track) p->track=i;
    }
    return p->track ? 0 : 1;
}
static inline uint32_t cdp_play(struct cdp_player *p)
{
    if (!p->track || p->track>p->tracks || !p->audio[p->track]) return 1;
    char q[128]; char *s=cdp_track(cdp_text(q,"play cd from "),p->track);
    s=cdp_track(cdp_text(s," to "),p->track); *s++=':';
    s=cdp_text(s,p->length[p->track]); cdp_text(s," notify");
    uint32_t e=p->command(p->ctx,q,0,0);
    if (!e) p->visited[p->track]=1;
    return e;
}
static inline int cdp_advance(struct cdp_player *p,int direction,int automatic)
{
    if (!p->tracks) return 0;
    if (automatic && p->random) {
        unsigned choices[99],count=0;
        for (unsigned pass=0;pass<2;pass++) {
            for (unsigned i=1;i<=p->tracks;i++) if (p->audio[i] && !p->visited[i]) choices[count++]=i;
            if (count) break;
            if (!p->repeat || pass) return 0;
            for (unsigned i=1;i<=p->tracks;i++) p->visited[i]=0;
        }
        p->seed=p->seed*1664525u+1013904223u;
        p->track=choices[p->seed%count]; return 1;
    }
    int i=(int)p->track;
    for (unsigned n=0;n<p->tracks;n++) {
        i+=direction;
        if (i<1 || (unsigned)i>p->tracks) {
            if (automatic && !p->repeat) return 0;
            i=direction<0 ? (int)p->tracks : 1;
        }
        if (p->audio[i]) { p->track=(unsigned)i; return 1; }
    }
    return 0;
}
static inline void cdp_label(const struct cdp_player *p,unsigned track,char text[64])
{
    char *s=cdp_track(cdp_text(text,"Track "),track);
    cdp_text(s,p->audio[track] ? "  " : "  Data");
    if (p->audio[track]) cdp_text(s+2,p->length[track]);
}
#endif
