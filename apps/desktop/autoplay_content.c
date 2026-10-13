/* SPDX-License-Identifier: MIT
 * Bounded media discovery and shell state; all I/O uses the Files VFS seam. */
#include "autoplay.h"
#include "../dosbox/autorun.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
        size_t end=at; while (end<size && text[end]!='\n') end++;
        char line[FM_PATH+128]; size_t n=end-at;
        if (n>=sizeof(line)) { memset(out,0,sizeof(*out)); return -EINVAL; }
        memcpy(line,text+at,n); line[n]=0; at=end+1;
        char *s=trim(line);
        if (*s==';' || !*s) continue;
        if (*s=='[') { section=!strcasecmp(s,"[autorun]"); continue; }
        if (!section) continue;
        char *eq=strchr(s,'='); if (!eq) continue; *eq++=0;
        s=trim(s); eq=trim(eq);
        for (const char *p=eq;*p;p++) if ((unsigned char)*p<32 || (unsigned char)*p>126) { memset(out,0,sizeof(*out)); return -EINVAL; }
        char *dst=NULL; size_t cap=0;
        if (!strcasecmp(s,"open")) { dst=out->open; cap=sizeof(out->open); }
        if (!strcasecmp(s,"icon")) { dst=out->icon; cap=sizeof(out->icon); }
        if (!strcasecmp(s,"label")) { dst=out->label; cap=sizeof(out->label); }
        if (dst) {
            /* Quotes group the executable token for open; only metadata
             * values strip an enclosing quote pair here. */
            size_t value_size=strlen(eq);
            if (dst!=out->open && value_size>=2 && eq[0]=='"' && eq[value_size-1]=='"') { eq[value_size-1]=0; eq++; }
            if (strlen(eq)>=cap && dst!=out->label) { memset(out,0,sizeof(*out)); return -EINVAL; }
            snprintf(dst,cap,"%s",eq);
        }
    }
    return 0;
}
static int read_prefix(const struct fm_vfs *fs,const char *path,void *out,unsigned cap)
{
    int fd=fs->open(fs->ctx,path,0); if (fd<0) return fd;
    unsigned used=0; int error=0;
    while (used<cap) { int n=fs->read(fs->ctx,fd,(char *)out+used,cap-used); if (n<0) { error=n; break; } if (!n) break; used+=(unsigned)n; }
    int e=fs->close(fs->ctx,fd); return error ? error : e ? e : (int)used;
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
struct scan { struct ap_content *c; const char *dir; unsigned count; char inf[FM_PATH], music[FM_PATH], pictures[FM_PATH]; };
static int entry(const char *name,const struct fm_meta *meta,void *ctx)
{
    struct scan *s=ctx; if (++s->count>2048) return -E2BIG;
    char path[FM_PATH]; if (fm_join(path,s->dir,name)) return 0;
    if (meta->type==FM_DIR) {
        if (!strcasecmp(name,"music")) snprintf(s->music,sizeof(s->music),"%s",path);
        if (!strcasecmp(name,"pictures")) snprintf(s->pictures,sizeof(s->pictures),"%s",path);
    } else if (meta->type==FM_FILE) {
        if (!strcasecmp(name,"autorun.inf")) snprintf(s->inf,sizeof(s->inf),"%s",path);
        if (ext(name,".wav|.mp3|.ogg|.flac|") && !s->c->music[0]) snprintf(s->c->music,sizeof(s->c->music),"%s",s->dir);
        if (ext(name,".png|.jpg|.jpeg|.jpe|.bmp|.gif|.xrgb|") && (!s->c->picture[0] || strcmp(path,s->c->picture)<0)) snprintf(s->c->picture,sizeof(s->c->picture),"%s",path);
    }
    return 0;
}
struct ap_workspace { struct scan s, sub; unsigned char data[AP_INF_MAX+1]; };
int ap_drive_label(const struct fm_vfs *fs,unsigned drive,char out[33])
{
    out[0]=0; if (drive<3 || drive>=26) return -EINVAL;
    struct ap_content *c=calloc(1,sizeof(*c));
    struct ap_workspace *w=calloc(1,sizeof(*w));
    if (!c || !w) { free(c); free(w); return -ENOMEM; }
    snprintf(c->root,sizeof(c->root),"/mnt/%c",'a'+drive);
    w->s=(struct scan){.c=c,.dir=c->root};
    int e=fs->list(fs->ctx,c->root,entry,&w->s);
    if (!e && w->s.inf[0]) {
        int n=read_prefix(fs,w->s.inf,w->data,sizeof(w->data));
        if (n>=0 && n<=AP_INF_MAX && !ap_parse(&c->inf,(char *)w->data,(size_t)n)) memcpy(out,c->inf.label,33);
    }
    free(c); free(w); return e;
}
static int probe(const struct fm_vfs *fs,unsigned drive,struct ap_content *c,struct ap_workspace *w)
{
    memset(c,0,sizeof(*c)); if (drive<3 || drive>=26) return -EINVAL;
    snprintf(c->root,sizeof(c->root),"/mnt/%c",'a'+drive);
    w->s=(struct scan){.c=c,.dir=c->root};
    struct scan *s=&w->s; int e=fs->list(fs->ctx,c->root,entry,s); if (e) return e;
    if (s->inf[0]) { int n=read_prefix(fs,s->inf,w->data,sizeof(w->data));
        if (n>=0 && n<=AP_INF_MAX) (void)ap_parse(&c->inf,(char *)w->data,(size_t)n); }
    if (s->music[0] && !c->music[0]) { w->sub=(struct scan){.c=c,.dir=s->music}; (void)fs->list(fs->ctx,s->music,entry,&w->sub); }
    if (s->pictures[0]) { w->sub=(struct scan){.c=c,.dir=s->pictures}; (void)fs->list(fs->ctx,s->pictures,entry,&w->sub); }
    if (!c->inf.open[0]) return 0;
    c->run_kind=3;
    char name[97]; const char *args=c->inf.open; unsigned n=0;
    if (*args=='"') { args++; while (*args && *args!='"' && n<96) name[n++]=*args++; if (*args!='"') return 0; args++; }
    else while (*args && *args!=' ' && n<96) name[n++]=*args++;
    name[n]=0; while (*args==' ') args++;
    if (relative(c->program,c->root,name)) return 0;
    struct fm_meta meta; if (fs->stat_path(fs->ctx,c->program,&meta) || meta.type!=FM_FILE) return 0;
    if (!ext(name,".exe|.com|.bat|")) return 0;
    unsigned char *h=w->data; int got=read_prefix(fs,c->program,h,4096); if (got<0) return 0;
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
    struct ap_workspace *w=calloc(1,sizeof(*w)); if (!w) { memset(c,0,sizeof(*c)); return -ENOMEM; }
    int result=probe(fs,drive,c,w); free(w); return result;
}
