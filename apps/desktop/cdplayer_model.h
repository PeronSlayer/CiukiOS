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
    char disc_key[24],artist[64],title[64],names[100][32];
};
static inline char *cdp_text(char *out,const char *text)
{ while (*text) *out++=*text++; *out=0; return out; }
static inline char *cdp_track(char *out,unsigned track)
{ *out++=(char)('0'+track/10); *out++=(char)('0'+track%10); *out=0; return out; }
static inline uint32_t cdp_load(struct cdp_player *p)
{
    char count[32]={0};
    uint32_t e=p->command(p->ctx,"status cd number of tracks",count,sizeof(count));
    p->tracks=p->track=0; p->disc_key[0]=p->artist[0]=p->title[0]=0;
    for (unsigned i=0;i<100;i++) { p->audio[i]=p->visited[i]=0; p->length[i][0]=p->names[i][0]=0; }
    if (e) return e;
    for (const char *c=count;*c;c++) {
        if (*c<'0' || *c>'9' || p->tracks>9) return 1;
        p->tracks=p->tracks*10+(unsigned)(*c-'0');
    }
    if (!p->tracks || p->tracks>99) { p->tracks=0; return 1; }
    uint32_t hash=2166136261u,hash2=p->tracks;
    for (unsigned i=1;i<=p->tracks;i++) {
        char position[32]={0},query[64];
        cdp_track(cdp_text(query,"status cd position track "),i);
        if (p->command(p->ctx,query,position,sizeof(position))) return 1;
        for (const char *t=position;*t;t++) { hash=(hash^(uint8_t)*t)*16777619u; hash2=hash2*65599u+(uint8_t)*t; }
        hash=(hash^i)*16777619u;
        char q[64],type[32]={0}; cdp_track(cdp_text(q,"status cd type track "),i);
        if (p->command(p->ctx,q,type,sizeof(type))) continue;
        cdp_track(cdp_text(q,"status cd length track "),i);
        if (p->command(p->ctx,q,p->length[i],sizeof(p->length[i]))) continue;
        for (const char *t=p->length[i];*t;t++) { hash=(hash^(uint8_t)*t)*16777619u; hash2=hash2*65599u+(uint8_t)*t; }
        hash=(hash^(uint8_t)type[0])*16777619u;
        if (type[0]!='a') continue; /* data contributes to identity, never PLAY */
        p->audio[i]=1; if (!p->track) p->track=i;
    }
    char *key=cdp_text(p->disc_key,"TOC-");
    for (unsigned i=0;i<8;i++) *key++="0123456789abcdef"[(hash>>(28-i*4))&15];
    for (unsigned i=0;i<8;i++) *key++="0123456789abcdef"[(hash2>>(28-i*4))&15];
    *key=0;
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
    char *s;
    if (p->names[track][0]) s=cdp_text(text,p->names[track]);
    else s=cdp_track(cdp_text(text,"Track "),track);
    cdp_text(s,p->audio[track] ? "  " : "  Data");
    if (p->audio[track]) { unsigned i=0; s+=2; while (i<20 && p->length[track][i]) { *s++=p->length[track][i++]; } *s=0; }
}
/* User-entered local disc database, one atomic INI section per TOC identity.
 * No title is inferred from audio data; a changed disc clears all names. */
struct cdp_database {
    void *ctx;
    void (*get)(void *,const char *,const char *,char *,unsigned);
    int (*put_section)(void *,const char *,const char *);
};
static inline void cdp_database_load(struct cdp_player *p,const struct cdp_database *db)
{
    if (!p->disc_key[0] || !p->tracks || p->tracks>99 || !db || !db->get) return;
    db->get(db->ctx,p->disc_key,"Artist",p->artist,sizeof(p->artist)); p->artist[63]=0;
    db->get(db->ctx,p->disc_key,"Title",p->title,sizeof(p->title)); p->title[63]=0;
    for (unsigned i=1;i<=p->tracks;i++) {
        char key[16]; cdp_track(cdp_text(key,"Track"),i);
        db->get(db->ctx,p->disc_key,key,p->names[i],sizeof(p->names[i])); p->names[i][31]=0;
    }
}
static inline int cdp_name_valid(const char *text,unsigned cap)
{
    for (unsigned i=0;i<cap;i++) {
        unsigned char c=(unsigned char)text[i];
        if (!c) return 1;
        if (c<32 || c==127) return 0;
    }
    return 0;
}
static inline int cdp_database_save(const struct cdp_player *p,const struct cdp_database *db,
                                   char *section,unsigned capacity)
{
    if (!p->disc_key[0] || !p->tracks || p->tracks>99 || !db || !db->put_section || capacity<8192 ||
        !cdp_name_valid(p->artist,64) || !cdp_name_valid(p->title,64)) return 0;
    char *out=cdp_text(section,"Artist="); out=cdp_text(out,p->artist)+1;
    out=cdp_text(out,"Title="); out=cdp_text(out,p->title)+1;
    for (unsigned i=1;i<=p->tracks;i++) {
        if (!cdp_name_valid(p->names[i],32)) return 0;
        out=cdp_track(cdp_text(out,"Track"),i); out=cdp_text(out,"=");
        out=cdp_text(out,p->names[i])+1;
    }
    *out=0; /* 2*72+99*(8+32)+1 < 8192 */
    return db->put_section(db->ctx,p->disc_key,section);
}
#endif
