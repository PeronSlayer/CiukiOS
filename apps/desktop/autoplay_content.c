/* SPDX-License-Identifier: MIT
 * Bounded media discovery and shell state; all I/O uses the Files VFS seam. */
#define _POSIX_C_SOURCE 200809L
#include "autoplay.h"
#include "../dosbox/autorun.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
struct scan {
    struct ap_content *c; const char *dir; unsigned count, limit;
    struct ap_workspace *workspace; const char *budget;
    char inf[FM_PATH], music[FM_PATH], pictures[FM_PATH];
};
struct ap_workspace {
    struct scan s, sub; unsigned char data[AP_INF_MAX+1];
    uint64_t start; struct ap_content *content;
    void (*phase)(void *, const char *); void *observer;
};
static uint64_t scan_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
static int within_time(struct ap_workspace *w)
{
    uint64_t elapsed=scan_now()-w->start;
    w->content->ms=(unsigned)(elapsed/1000000ull);
    if (elapsed>=AP_SCAN_NS) w->content->budget="time";
    return !w->content->budget;
}
static void scan_phase(struct ap_workspace *w,const char *phase)
{ if (w->phase) w->phase(w->observer,phase); }
static char *trim(char *s)
{
    while (*s==' ' || *s=='\t' || *s=='\r') s++;
    size_t n=strlen(s); while (n && (s[n-1]==' ' || s[n-1]=='\t' || s[n-1]=='\r')) s[--n]=0;
    return s;
}
int ap_parse(struct ap_inf *out,const char *text,size_t size)
{
    memset(out,0,sizeof(*out));
    if (!text || size>AP_INF_MAX || memchr(text,0,size)) return -EINVAL;
    int section=0; size_t at=0;
    while (at<size) {
        size_t end=at; while (end<size && text[end]!='\n' && text[end]!='\r') end++;
        char line[FM_PATH+128]; size_t n=end-at;
        size_t next=end; if (next<size && text[next]=='\r') next++;
        if (next<size && text[next]=='\n') next++;
        if (n>=sizeof(line)) {
            fprintf(stderr,"[autoplay] field=line bytes=%zu action=ignore reason=capacity\n",n);
            size_t first=at; while (first<end && (text[first]==' ' || text[first]=='\t')) first++;
            if (first<end && text[first]=='[') section=0;
            at=next; continue;
        }
        memcpy(line,text+at,n); line[n]=0; at=next;
        char *s=trim(line);
        if (*s==';' || !*s) continue;
        if (*s=='[') {
            char *close=strchr(s+1,']');
            if (close) *close=0;
            section=close && !strcasecmp(trim(s+1),"autorun"); continue;
        }
        if (!section) continue;
        char *eq=strchr(s,'='); if (!eq) continue; *eq++=0;
        s=trim(s); eq=trim(eq);
        char *dst=NULL; size_t cap=0;
        if (!strcasecmp(s,"open")) { dst=out->open; cap=sizeof(out->open); }
        if (!strcasecmp(s,"icon")) { dst=out->icon; cap=sizeof(out->icon); }
        if (!strcasecmp(s,"label")) { dst=out->label; cap=sizeof(out->label); }
        if (dst) {
            /* Unknown keys and unrelated malformed lines cannot discard a
             * valid open/icon/label. Preserve native UTF-8 path bytes. */
            int bad=0;
            for (const char *p=eq;*p;p++) if ((unsigned char)*p<32 && *p!='\t') bad=1;
            if (bad) { fprintf(stderr,"[autoplay] field=%s action=ignore reason=control_byte\n",s); continue; }
            /* Quotes group the executable token for open; only metadata
             * values strip an enclosing quote pair here. */
            size_t value_size=strlen(eq);
            if (dst!=out->open && value_size>=2 && eq[0]=='"' && eq[value_size-1]=='"') { eq[value_size-1]=0; eq++; }
            if (strlen(eq)>=cap && dst!=out->label) {
                fprintf(stderr,"[autoplay] field=%s action=ignore reason=capacity\n",s); continue;
            }
            if (strlen(eq)>=cap) {
                size_t length=cap-1;
                while (length && ((unsigned char)eq[length]&0xc0)==0x80) length--;
                memcpy(dst,eq,length); dst[length]=0;
            } else snprintf(dst,cap,"%s",eq);
        }
    }
    return 0;
}
static int read_prefix(const struct fm_vfs *fs,const char *path,void *out,unsigned cap,struct ap_workspace *w)
{
    if (!within_time(w)) return -ETIMEDOUT;
    if (w->content->sniffed==AP_SNIFF_FILES) { w->content->budget="sniff"; return -E2BIG; }
    w->content->sniffed++;
    int fd=fs->open(fs->ctx,path,0); if (fd<0) return fd;
    unsigned used=0; int error=0;
    while (used<cap && within_time(w)) { int n=fs->read(fs->ctx,fd,(char *)out+used,cap-used); if (n<0) { error=n; break; } if (!n) break; used+=(unsigned)n; }
    int e=fs->close(fs->ctx,fd);
    if (!within_time(w)) return -ETIMEDOUT;
    return error ? error : e ? e : (int)used;
}
static int relative(char *out,const char *root,const char *name)
{
    while (*name=='/' || *name=='\\') name++;
    if (!*name || strlen(name)>=FM_PATH-16) return -EINVAL;
    char path[FM_PATH]; snprintf(path,sizeof(path),"%s",name);
    for (char *p=path;*p;p++) { if (*p=='\\') *p='/'; if (*p==':' || (unsigned char)*p<32) return -EINVAL; }
    for (char *p=path;;) { char *end=strchr(p,'/'); size_t n=end ? (size_t)(end-p) : strlen(p);
        if (!n || (n==1 && *p=='.') || (n==2 && !memcmp(p,"..",2))) return -EINVAL;
        if (!end) break;
        p=end+1;
    }
    int n=snprintf(out,FM_PATH,"%s/%s",root,path);
    return n<0 || n>=FM_PATH ? -ENAMETOOLONG : 0;
}
static int ext(const char *name,const char *types)
{
    const char *e=strrchr(name,'.'); if (!e) return 0;
    char key[16]; if (strlen(e)>=sizeof(key)-2) return 0;
    snprintf(key,sizeof(key),"%s|",e);
    for (char *p=key;*p;p++) if (*p>='A' && *p<='Z') *p=(char)(*p+32);
    return strstr(types,key)!=NULL;
}
static int entry(const char *name,const struct fm_meta *meta,void *ctx)
{
    struct scan *s=ctx;
    if (!within_time(s->workspace)) return -ETIMEDOUT;
    s->count++; s->c->entries++;
    char path[FM_PATH]; if (fm_join(path,s->dir,name)) {
        if (s->count>=s->limit) { s->c->budget=s->budget; return -E2BIG; }
        return 0;
    }
    if (meta->type==FM_DIR) {
        if (!strcasecmp(name,"music")) snprintf(s->music,sizeof(s->music),"%s",path);
        if (!strcasecmp(name,"pictures")) snprintf(s->pictures,sizeof(s->pictures),"%s",path);
    } else if (meta->type==FM_FILE) {
        if (!strcasecmp(name,"autorun.inf")) snprintf(s->inf,sizeof(s->inf),"%s",path);
        if (ext(name,".wav|.mp3|.ogg|.flac|") && !s->c->music[0]) snprintf(s->c->music,sizeof(s->c->music),"%s",s->dir);
        if (ext(name,".png|.jpg|.jpeg|.jpe|.bmp|.gif|.xrgb|") && (!s->c->picture[0] || strcmp(path,s->c->picture)<0)) snprintf(s->c->picture,sizeof(s->c->picture),"%s",path);
    }
    if (s->count>=s->limit) { s->c->budget=s->budget; return -E2BIG; }
    return 0;
}
struct ap_workspace *ap_workspace_create(void) { return calloc(1,sizeof(struct ap_workspace)); }
void ap_workspace_observe(struct ap_workspace *w,void (*phase)(void *,const char *),void *ctx)
{ w->phase=phase; w->observer=ctx; }
int ap_drive_label(const struct fm_vfs *fs,unsigned drive,char out[33])
{
    out[0]=0; if (drive<3 || drive>=26) return -EINVAL;
    struct ap_content *c=calloc(1,sizeof(*c));
    struct ap_workspace *w=calloc(1,sizeof(*w));
    if (!c || !w) { free(c); free(w); return -ENOMEM; }
    snprintf(c->root,sizeof(c->root),"/mnt/%c",'a'+drive);
    w->content=c; w->start=scan_now();
    w->s=(struct scan){.c=c,.dir=c->root,.workspace=w,.limit=AP_ROOT_ENTRIES,.budget="root"};
    int e=fs->list(fs->ctx,c->root,FM_LIST_NAMES_ONLY,entry,&w->s);
    if (within_time(w) && !e && w->s.inf[0]) {
        int n=read_prefix(fs,w->s.inf,w->data,sizeof(w->data),w);
        if (n>=0 && n<=AP_INF_MAX && !ap_parse(&c->inf,(char *)w->data,(size_t)n)) memcpy(out,c->inf.label,33);
    }
    free(c); free(w); return e;
}
static int probe(const struct fm_vfs *fs,unsigned drive,struct ap_content *c,struct ap_workspace *w)
{
    memset(c,0,sizeof(*c)); if (drive<3 || drive>=26) return -EINVAL;
    snprintf(c->root,sizeof(c->root),"/mnt/%c",'a'+drive);
    w->content=c; w->start=scan_now(); scan_phase(w,"media-scan:root");
    w->s=(struct scan){.c=c,.dir=c->root,.workspace=w,.limit=AP_ROOT_ENTRIES,.budget="root"};
    struct scan *s=&w->s; int e=fs->list(fs->ctx,c->root,FM_LIST_NAMES_ONLY,entry,s);
    if (!within_time(w) || e) return e;
    if (s->music[0] && !c->music[0]) {
        scan_phase(w,"media-scan:music");
        w->sub=(struct scan){.c=c,.dir=s->music,.workspace=w,.limit=AP_SUB_ENTRIES,.budget="music"};
        (void)fs->list(fs->ctx,s->music,FM_LIST_NAMES_ONLY,entry,&w->sub);
        if (!within_time(w)) return 0;
    }
    if (s->pictures[0]) {
        scan_phase(w,"media-scan:pictures");
        w->sub=(struct scan){.c=c,.dir=s->pictures,.workspace=w,.limit=AP_SUB_ENTRIES,.budget="pictures"};
        (void)fs->list(fs->ctx,s->pictures,FM_LIST_NAMES_ONLY,entry,&w->sub);
        if (!within_time(w)) return 0;
    }
    scan_phase(w,"media-scan:inf");
    if (s->inf[0]) { int n=read_prefix(fs,s->inf,w->data,sizeof(w->data),w);
        if (n>=0 && n<=AP_INF_MAX) (void)ap_parse(&c->inf,(char *)w->data,(size_t)n); }
    if (!within_time(w)) return 0;
    if (!c->inf.open[0]) return 0;
    c->run_kind=3;
    char name[97]; const char *args=c->inf.open; unsigned n=0;
    if (*args=='"') { args++; while (*args && *args!='"' && n<96) name[n++]=*args++; if (*args!='"') return 0; args++; }
    else while (*args && *args!=' ' && n<96) name[n++]=*args++;
    name[n]=0; while (*args==' ') args++;
    if (relative(c->program,c->root,name)) return 0;
    if (!ext(name,".exe|.com|.bat|")) return 0;
    struct fm_meta meta; if (fs->stat_path(fs->ctx,c->program,&meta) || !within_time(w) || meta.type!=FM_FILE) return 0;
    unsigned char *h=w->data; int got=read_prefix(fs,c->program,h,4096,w); if (got<0) return 0;
    if (ext(name,".exe|")) {
        if (got<28 || memcmp(h,"MZ",2)) return 0;
        uint32_t pe=got>=64 ? (uint32_t)h[60]|(uint32_t)h[61]<<8|(uint32_t)h[62]<<16|(uint32_t)h[63]<<24 : 0;
        if (pe>=64 && pe<(uint64_t)meta.size) {
            if (pe>(unsigned)got-4u) return 0;
            if (!memcmp(h+pe,"PE\0\0",4) || !memcmp(h+pe,"NE",2) || !memcmp(h+pe,"LE",2) || !memcmp(h+pe,"LX",2)) { c->run_kind=2; return 0; }
        }
    }
    /* The DOS typist splits on spaces and cannot quote executable paths. */
    char dos[97]; if (strlen(name)>90 || strchr(name,' ')) return 0;
    while (*name=='/' || *name=='\\') memmove(name,name+1,strlen(name));
    for (char *p=name;*p;p++) if (*p=='/') *p='\\';
    int len=snprintf(dos,sizeof(dos),"%c:\\%s%s%s",'A'+drive,name,*args ? " " : "",args);
    if (len<0 || len>96) return 0;
    char lines[DOSBOX_LINES_MAX][DOSBOX_COMMAND_MAX+4]; if (dosbox_command_lines(dos,lines)<0) return 0;
    snprintf(c->command,sizeof(c->command),"%s",dos); c->run_kind=1; return 0;
}
int ap_probe(const struct fm_vfs *fs,unsigned drive,struct ap_content *c)
{
    struct ap_workspace *w=ap_workspace_create();
    int result=ap_probe_workspace(fs,drive,c,w); free(w); return result;
}
int ap_probe_workspace(const struct fm_vfs *fs,unsigned drive,struct ap_content *c,struct ap_workspace *w)
{
    if (!w) { memset(c,0,sizeof(*c)); return -ENOMEM; }
    int result=probe(fs,drive,c,w);
    if (drive>=3 && drive<26) (void)within_time(w);
    if (c->budget) {
        const char *budget=c->budget; unsigned entries=c->entries, sniffed=c->sniffed, ms=c->ms;
        memset(c,0,sizeof(*c)); snprintf(c->root,sizeof(c->root),"/mnt/%c",'a'+drive);
        c->budget=budget; c->entries=entries; c->sniffed=sniffed; c->ms=ms;
        return 0;
    }
    return result;
}
