/* SPDX-License-Identifier: MIT */
#include "terminal.h"
#include "../tasks/model.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
static unsigned lower(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int equal(const char *a, const char *b) { return fm_same_path(a,b); }
static int wildcard(const char *p, const char *s)
{
    const char *star = NULL, *retry = NULL;
    if (equal(p,"*.*")) p = "*";
    while (*s) {
        if (*p == '?' || lower((unsigned char)*p) == lower((unsigned char)*s)) { p++; s++; }
        else if (*p == '*') { star = p++; retry = s; }
        else if (star) { p = star + 1; s = ++retry; }
        else return 0;
    }
    while (*p == '*') p++;
    return !*p;
}
static int parse(char *s, char **argv)
{
    int n = 0; char *read = s, *write = s;
    while (*read) {
        while (*read == ' ' || *read == '\t') read++;
        if (!*read) break;
        if (n == TERM_ARGS) return -E2BIG;
        argv[n++] = write; int quote = 0;
        while (*read && (quote || (*read != ' ' && *read != '\t'))) {
            if (*read == '"') { quote = !quote; read++; }
            else *write++ = *read++;
        }
        if (quote) return -EINVAL;
        while (*read == ' ' || *read == '\t') read++;
        *write++ = 0;
    }
    return n;
}
__attribute__((noinline)) int term_path(struct terminal *t, const char *in, char *out)
{
    char raw[FM_PATH], normalized[FM_PATH]; int r;
    if (equal(in,"Computer")) { out[0] = 0; return 0; }
    if (*in == '/' || (in[0] && in[1] == ':')) {
        if ((r = fm_posix_path(raw,in))) return r;
    } else {
        if (!t->cwd[0]) return -EINVAL;
        const char *base = t->cwd; char root[FM_PATH];
        if (*in == '\\') {
            if (!strncmp(base,"/mnt/",5) && base[5] && (base[6] == '/' || !base[6])) {
                memcpy(root,base,6); root[6] = 0; base = root;
            } else base = "/";
        }
        int n = snprintf(raw,sizeof(raw),"%s/%s",base,in);
        if (n < 0 || (size_t)n >= sizeof(raw)) return -ENAMETOOLONG;
        for (char *p = raw; *p; p++) if (*p == '\\') *p = '/';
    }
    if (!raw[0]) { out[0] = 0; return 0; }
    /* Preserve each mounted drive root when walking .. . */
    size_t root = !strncmp(raw,"/mnt/",5) && lower((unsigned char)raw[5]) >= 'd' &&
        lower((unsigned char)raw[5]) <= 'z' && (!raw[6] || raw[6]=='/') ? 6 : 1;
    memcpy(normalized,raw,root); normalized[root] = 0;
    size_t used = root; const char *p = raw + root;
    while (*p) {
        while (*p == '/') p++;
        const char *begin = p; while (*p && *p != '/') p++;
        size_t n = (size_t)(p - begin); if (!n || (n == 1 && *begin == '.')) continue;
        if (n == 2 && begin[0] == '.' && begin[1] == '.') {
            if (used > root) { while (used > root && normalized[used - 1] != '/') used--; if (used > root) used--; }
            normalized[used] = 0; continue;
        }
        if (used > 1 && normalized[used - 1] != '/') normalized[used++] = '/';
        if (used + n >= sizeof(normalized)) return -ENAMETOOLONG;
        memcpy(normalized + used,begin,n); used += n; normalized[used] = 0;
    }
    strcpy(out,normalized); return 0;
}
struct matches { char **paths; unsigned count; const char *parent, *pattern; };
static void release(struct matches *m) { for (unsigned i=0;i<m->count;i++) free(m->paths[i]); free(m->paths); }
static int collect(const char *name, const struct fm_meta *meta, void *ctx)
{
    (void)meta; struct matches *m = ctx;
    if (!strcmp(name,".") || !strcmp(name,"..") || !wildcard(m->pattern,name)) return 0;
    if (m->count >= FM_ENTRIES) return -E2BIG;
    char path[FM_PATH]; int r = fm_join(path,m->parent,name); if (r) return r;
    char *s = malloc(strlen(path)+1); if (!s) return -ENOMEM; strcpy(s,path);
    char **a = realloc(m->paths,(m->count+1)*sizeof(*a)); if (!a) { free(s); return -ENOMEM; }
    m->paths=a; a[m->count++]=s; return 0;
}
static int expand(struct terminal *t, const char *arg, struct matches *m)
{
    char path[FM_PATH]; int r = term_path(t,arg,path); if (r) return r;
    if (!path[0]) return -EINVAL;
    if (!strpbrk(path,"*?")) {
        struct fm_meta meta; r=t->host.fs.stat_path(t->host.fs.ctx,path,&meta); if (r) return r;
        m->paths=malloc(sizeof(*m->paths)); if (!m->paths) return -ENOMEM;
        m->paths[0]=malloc(strlen(path)+1); if (!m->paths[0]) { free(m->paths); m->paths=NULL; return -ENOMEM; }
        strcpy(m->paths[0],path); m->count=1; return 0;
    }
    char parent[FM_PATH]; fm_parent(parent,path); if (strpbrk(parent,"*?")) return -EINVAL;
    m->parent=parent; m->pattern=fm_basename(path);
    r=t->host.fs.list(t->host.fs.ctx,parent,FM_LIST_SNIFF,collect,m);
    return r ? r : m->count ? 0 : -ENOENT;
}
struct listing { struct terminal *t; const char *pattern; };
/* Directory formatting uses separate calls: variadic arguments must match. */
static int directory_entry(const char *name, const struct fm_meta *m, void *ctx)
{
    struct listing *l=ctx;
    if (!strcmp(name,".") || !strcmp(name,"..") || !wildcard(l->pattern,name)) return 0;
    if (m->type==FM_DIR) term_printf(l->t,"<DIR>          %s\n",name);
    else term_printf(l->t,"%10llu     %s\n",(unsigned long long)m->size,name);
    return 0;
}
static int directory(struct terminal *t,const char *arg)
{
    char path[FM_PATH], parent[FM_PATH]; int r=term_path(t,arg,path); if(r)return r;
    struct listing l={t,"*"};
    if (!path[0]) return fm_volumes(&t->host.fs,directory_entry,&l);
    if (strpbrk(path,"*?")) { fm_parent(parent,path); l.pattern=fm_basename(path); if(strpbrk(parent,"*?"))return -EINVAL; }
    else {
        struct fm_meta m; if ((r=t->host.fs.stat_path(t->host.fs.ctx,path,&m)))return r;
        if(m.type!=FM_DIR)return directory_entry(fm_basename(path),&m,&l);
        strcpy(parent,path);
    }
    return t->host.fs.list(t->host.fs.ctx,parent,FM_LIST_SNIFF,directory_entry,&l);
}
static __attribute__((noinline)) int type_file(struct terminal *t,const char *path)
{
    struct fm_meta m; int r=t->host.fs.stat_path(t->host.fs.ctx,path,&m); if(r)return r;
    if(m.type!=FM_FILE)return -EISDIR;
    int fd=t->host.fs.open(t->host.fs.ctx,path,0); if(fd<0)return fd;
    char buf[2049];
    for (;;) {
        r=t->host.fs.read(t->host.fs.ctx,fd,buf,sizeof(buf)-1); if(r<=0)break;
        for(int i=0;i<r;i++)if(!buf[i])buf[i]='?'; buf[r]=0; term_write(t,buf);
        if(t->host.pump)t->host.pump(t->host.ctx);
        if(t->interrupted || t->capture_error) { r=t->interrupted ? -EINTR : t->capture_error; break; }
    }
    int c=t->host.fs.close(t->host.fs.ctx,fd); return r<0 ? r : c;
}
/* Delete only the private staging tree owned by this operation. */
static int cleanup_stage(struct terminal *t,const char *path)
{
    struct fm_job *j=calloc(1,sizeof(*j)); if(!j)return -ENOMEM;
    char *paths[]={(char *)path}; int r=fm_job_start(j,&t->host.fs,FM_DELETE,paths,1,NULL);
    while(!r && j->state==FM_RUNNING)fm_job_step(j);
    if(!r && j->state!=FM_DONE)r=j->error ? -j->error : -EIO;
    fm_job_destroy(j); free(j); return r;
}
static int transfer(struct terminal *t,int move,struct matches *m,const char *arg)
{
    char *destination=t->scratch[0]; int r=term_path(t,arg,destination); if(r || !*destination)return r ? r : -EINVAL;
    for(unsigned i=0;i<m->count;i++)if(fm_drive_root(m->paths[i]))return -EPERM;
    struct fm_meta meta; int exists=t->host.fs.stat_path(t->host.fs.ctx,destination,&meta);
    int dir=!exists && meta.type==FM_DIR;
    if(!dir && m->count!=1)return -ENOTDIR;
    if(exists && exists!=-ENOENT)return exists;
    if (!dir) {
        if(!exists)return -EEXIST;
        if (move) {
            r=t->host.fs.rename(t->host.fs.ctx,m->paths[0],destination);
            if(r!=-EXDEV)return r;
        }
        /* Transfer jobs target a folder. Copy with the original basename into
         * a sibling temporary directory, then rename to the requested name. */
        char *parent=t->scratch[1], *stage=t->scratch[2], *copied=t->scratch[3]; fm_parent(parent,destination);
        unsigned serial=0;
        do { if (++serial>1000)return -EEXIST; int length=snprintf(stage,FM_PATH,"%s%s.terminal-copy-%u",parent,strcmp(parent,"/") ? "/" : "",serial);
             if(length<0 || length>=FM_PATH)return -ENAMETOOLONG;
             exists=t->host.fs.stat_path(t->host.fs.ctx,stage,&meta); } while(!exists);
        if(exists!=-ENOENT)return exists;
        if((r=t->host.fs.mkdir(t->host.fs.ctx,stage)))return r;
        struct fm_job *j=calloc(1,sizeof(*j)); if(!j) { t->host.fs.rmdir(t->host.fs.ctx,stage); return -ENOMEM; }
        r=fm_job_start(j,&t->host.fs,FM_COPY,m->paths,1,stage);
        while(!r && j->state==FM_RUNNING) {
            fm_job_step(j); if(t->host.pump)t->host.pump(t->host.ctx); if(t->interrupted)fm_job_cancel(j);
        }
        if(!r && j->state!=FM_DONE)r=j->error ? -j->error : -EIO;
        fm_job_destroy(j); free(j);
        int joined=fm_join(copied,stage,fm_basename(m->paths[0])); if(!r)r=joined;
        if(!r) {
            int existing=t->host.fs.stat_path(t->host.fs.ctx,destination,&meta);
            r=!existing ? -EEXIST : existing!=-ENOENT ? existing : t->host.fs.rename(t->host.fs.ctx,copied,destination);
        }
        int cleanup=cleanup_stage(t,stage);
        if(cleanup) { int retry=cleanup_stage(t,stage); if(!retry || retry==-ENOENT)cleanup=0; }
        if(cleanup)term_printf(t,"Staging cleanup: %s (error %d)\n",strerror(-cleanup),-cleanup);
        if(!r && cleanup)r=cleanup;
        if(!r && move) {
            struct fm_job *remove=calloc(1,sizeof(*remove)); if(!remove)return -ENOMEM;
            r=fm_job_start(remove,&t->host.fs,FM_DELETE,m->paths,1,NULL);
            while(!r && remove->state==FM_RUNNING)fm_job_step(remove);
            if(!r && remove->state!=FM_DONE)r=remove->error ? -remove->error : -EIO;
            fm_job_destroy(remove); free(remove);
        }
        return r;
    }
    struct fm_job *j=calloc(1,sizeof(*j)); if(!j)return -ENOMEM;
    r=fm_job_start(j,&t->host.fs,move ? FM_MOVE : FM_COPY,m->paths,m->count,destination);
    while(!r && j->state==FM_RUNNING) {
        fm_job_step(j); if(t->host.pump)t->host.pump(t->host.ctx); if(t->interrupted)fm_job_cancel(j);
    }
    if(!r && j->state!=FM_DONE)r=j->state==FM_REPLACE ? -EEXIST : j->error ? -j->error : t->interrupted ? -EINTR : -EIO;
    if(j->state==FM_REPLACE) { fm_job_cancel(j); while(j->state==FM_RUNNING || j->state==FM_REPLACE)fm_job_step(j); }
    fm_job_destroy(j); free(j); return r;
}
static int number(const char *s,unsigned long max,unsigned long *v)
{ char *end; if(!*s || *s=='-')return -EINVAL; errno=0; *v=strtoul(s,&end,10); return *end || errno || *v>max ? -EINVAL : 0; }
static int system_cmd(struct terminal *t,int n,char **a)
{
    const char *c=a[0]; int r;
    if(equal(c,"NET")) {
        if(n!=1)return -EINVAL;
        struct ciuki_net_state s={.version=1,.size=sizeof(s)};
        r=t->host.net ? t->host.net(t->host.ctx,&s) : -ENODEV; if(r)return r;
        if(s.version!=1 || s.size!=sizeof(s))return -EIO;
        term_printf(t,"Driver: %.16s  present: %u\nMAC: %02x:%02x:%02x:%02x:%02x:%02x\nCarrier: %s  state: %s (%.24s)\n",
            s.driver,s.present,s.mac[0],s.mac[1],s.mac[2],s.mac[3],s.mac[4],s.mac[5],
            s.link==CIUKI_NET_LINK_UP ? "up" : s.link==CIUKI_NET_LINK_DOWN ? "down" : "unknown",
            s.state==CIUKI_NET_RUNNING ? "running" : s.state==CIUKI_NET_QUARANTINED ? "quarantined" : "stopped",s.reason); return 0;
    }
    if(equal(c,"AUDIO")) {
        struct ciuki_mixer_master m={.version=1,.size=sizeof(m)};
        r=t->host.mixer ? t->host.mixer(t->host.ctx,&m) : -ENODEV; if(r)return r;
        if(m.version!=1 || m.size!=sizeof(m) || m.left>100 || m.right>100 || m.mute>1)return -EIO;
        if(n==1) { term_printf(t,"Volume: L %u R %u  mute: %s\n",m.left,m.right,m.mute ? "on" : "off"); return 0; }
        if(n==3 && equal(a[1],"volume")) { unsigned long v; if((r=number(a[2],100,&v)))return r; m.left=m.right=(uint32_t)v; }
        else if(n==2 && (equal(a[1],"mute") || equal(a[1],"unmute")))m.mute=equal(a[1],"mute");
        else return -EINVAL;
        r=t->host.volume ? t->host.volume(t->host.ctx,m.left,m.right,m.mute) : -ENODEV;
        if(!r)term_write(t,"Volume change requested.\n"); return r;
    }
    if(equal(c,"TASKS") || equal(c,"MEM")) {
        if(equal(c,"TASKS") && n==3 && equal(a[1],"KILL")) {
            unsigned long pid; if((r=number(a[2],INT_MAX,&pid)) || !pid)return -EINVAL;
            r=t->host.kill ? t->host.kill(t->host.ctx,(uint32_t)pid) : -ENOSYS;
            if(!r)term_write(t,"End Task requested.\n"); return r;
        }
        if(n!=1)return -EINVAL;
        void *buffer=malloc(TM_LIST_BYTES); struct tm_model *m=calloc(1,sizeof(*m));
        if(!buffer || !m) { free(buffer); free(m); return -ENOMEM; }
        tm_init(m); r=t->host.processes ? t->host.processes(t->host.ctx,buffer,TM_LIST_BYTES) : -ENOSYS;
        if(r>=0)r=tm_update(m,buffer,TM_LIST_BYTES,r);
        if(!r && equal(c,"MEM"))term_printf(t,"Usable: %llu KiB  Free: %llu KiB\nKernel heap: %llu KiB (%u bytes)\n",
            (unsigned long long)m->system.usable_pages*4,(unsigned long long)m->system.free_pages*4,
            (unsigned long long)m->system.kheap_pages*4,m->system.kheap_bytes);
        else if(!r) { term_write(t,"PID     PPID    Pages   Process\n"); for(unsigned i=0;i<m->count;i++)term_printf(t,"%-7u %-7u %-7u %s\n",m->processes[i].pid,m->processes[i].ppid,m->processes[i].pages,m->processes[i].name); }
        free(buffer); free(m); return r;
    }
    if(equal(c,"DISPLAY")) {
        if(n!=1)return -EINVAL;
        struct ciuki_display_info d={.version=CIUKI_DISPLAY_VERSION,.size=sizeof(d)};
        r=t->host.display ? t->host.display(t->host.ctx,&d) : -ENOSYS; if(r)return r;
        if(d.version!=CIUKI_DISPLAY_VERSION || d.size!=sizeof(d) || d.mode_count>32)return -EIO;
        term_printf(t,"Current: %ux%u %u bpp\nAvailable modes:\n",d.width,d.height,d.bpp);
        for(unsigned i=0;i<d.mode_count;i++)term_printf(t,"%04x  %ux%u %u bpp%s\n",d.modes[i].id,d.modes[i].width,d.modes[i].height,d.modes[i].bpp,d.modes[i].current ? " *" : ""); return 0;
    }
    if(equal(c,"DATE") || equal(c,"TIME")) {
        if(n!=1)return -EINVAL; time_t now=t->host.now ? t->host.now(t->host.ctx) : time(NULL);
        struct tm *tm=localtime(&now); char s[64]; if(!tm || !strftime(s,sizeof(s),equal(c,"DATE") ? "%Y-%m-%d\n" : "%H:%M:%S\n",tm))return -EIO;
        term_write(t,s); return 0;
    }
    if(equal(c,"SHUTDOWN") || equal(c,"REBOOT")) {
        if(n!=1)return -EINVAL;
        if(t->capture)return -EPERM; /* confirmations must be visible */
        t->pending_power=equal(c,"REBOOT") ? CIUKI_POWER_REBOOT : CIUKI_POWER_OFF;
        static const char *const buttons[]={"&Yes","&No"};
        du_message(t->ui,50,"Terminal",equal(c,"REBOOT") ? "Close programs and reboot CiukiOS?" : "Close programs and shut down CiukiOS?",DU_DLG_ICON_QUESTION,buttons,2,1,1); return 0;
    }
    return -ENOSYS;
}
static const char help[]=
"DIR/ls [path]          CD/cd [path|Computer]\n"
"COPY/cp src dst        MOVE/mv src dst     REN src dst\n"
"DEL/rm files           MD/mkdir dir       RD/rmdir dir\n"
"TYPE/cat files         CLS/clear          ECHO text\n"
"VER  HELP  EXIT        START program [file]\n"
"NET                   AUDIO [volume 0..100|mute|unmute]\n"
"TASKS [KILL pid]       DISPLAY            MEM\n"
"DATE  TIME            SHUTDOWN            REBOOT\n"
"LUA [script.lua args]  script.lua [args]   RUN (unavailable)\n"
"REG is reserved. Quote paths with spaces; * and ? match names.\n"
"Tab: complete path; Up/Down: history; PgUp/PgDn: scroll.\n"
"Ctrl+C: copy selection or interrupt; Ctrl+V: paste.\n"
"Interactive Lua: .exit returns to the command prompt.\n";
int term_execute(struct terminal *t,const char *line)
{
    if(strlen(line)>=DU_TEXT_MAX) { term_write(t,"Command too long.\n"); return -E2BIG; }
    char text[DU_TEXT_MAX]; strcpy(text,line); char *a[TERM_ARGS]; int n=parse(text,a),r=0;
    if(n<0)r=n;
    else if(!n)return 0;
    else if(equal(a[0],"HELP")) { if(n!=1)r=-EINVAL; else term_write(t,help); }
    else if(equal(a[0],"ECHO")) { for(int i=1;i<n;i++) { if(i>1)term_write(t," "); term_write(t,a[i]); } term_write(t,"\n"); }
    else if(equal(a[0],"VER")) { if(n!=1)r=-EINVAL; else term_write(t,"CiukiOS native Terminal\nClassic computing, one woof at a time.\n"); }
    else if(equal(a[0],"EXIT")) { if(n!=1)r=-EINVAL; else if(t->capture)r=-EPERM; else { t->quit=1; if(t->host.quit)t->host.quit(t->host.ctx); } }
    else if(equal(a[0],"CLS") || equal(a[0],"clear")) {
        if(n!=1)r=-EINVAL; else if(!t->capture) { for(unsigned i=0;i<t->count;i++)free(t->lines[(t->first+i)%TERM_SCROLLBACK]); t->first=t->count=t->used=t->top=0; t->pending[0]=0; t->selection_a=t->selection_b=0; }
    }
    else if(equal(a[0],"RUN"))term_write(t,"not available in this release\n");
    else if(equal(a[0],"REG"))term_write(t,"REG is reserved for a later release.\n");
    else if(equal(a[0],"DIR") || equal(a[0],"ls"))r=n>2 ? -EINVAL : directory(t,n==2 ? a[1] : t->cwd[0] ? t->cwd : "Computer");
    else if(equal(a[0],"CD") || (n==1 && strlen(a[0])==2 && a[0][1]==':')) {
        if(equal(a[0],"CD") && n==1) { char display[FM_PATH]; if(fm_display_path(display,t->cwd))snprintf(display,sizeof(display),"%s",t->cwd); term_printf(t,"%s\n",display); }
        else if(n>2)r=-EINVAL;
        else { char path[FM_PATH]; r=term_path(t,n==2 ? a[1] : a[0],path); struct fm_meta meta;
            if(!r && path[0]) { r=t->host.fs.stat_path(t->host.fs.ctx,path,&meta); if(!r && meta.type!=FM_DIR)r=-ENOTDIR; }
            if(!r && path[0] && t->host.cwd)r=t->host.cwd(t->host.ctx,path);
            if(!r)strcpy(t->cwd,path);
        }
    }
    else if(equal(a[0],"MD") || equal(a[0],"mkdir") || equal(a[0],"RD") || equal(a[0],"rmdir")) {
        if(n!=2)r=-EINVAL; else { char path[FM_PATH]; r=term_path(t,a[1],path); if(!r && (!path[0] || fm_drive_root(path)))r=-EPERM;
            if(!r)r=(equal(a[0],"MD") || equal(a[0],"mkdir")) ? t->host.fs.mkdir(t->host.fs.ctx,path) : t->host.fs.rmdir(t->host.fs.ctx,path); }
    }
    else if(equal(a[0],"TYPE") || equal(a[0],"cat") || equal(a[0],"DEL") || equal(a[0],"rm")) {
        if(n<2)r=-EINVAL;
        for(int k=1;!r && k<n;k++) { struct matches m={0}; r=expand(t,a[k],&m);
            for(unsigned i=0;!r && i<m.count;i++)r=(equal(a[0],"TYPE") || equal(a[0],"cat")) ? type_file(t,m.paths[i]) : t->host.fs.unlink(t->host.fs.ctx,m.paths[i]); release(&m); }
    }
    else if(equal(a[0],"COPY") || equal(a[0],"cp") || equal(a[0],"MOVE") || equal(a[0],"mv") || equal(a[0],"REN")) {
        if(n!=3)r=-EINVAL; else { struct matches m={0}; r=expand(t,a[1],&m);
            if(!r && equal(a[0],"REN")) { char path[FM_PATH]; r=term_path(t,a[2],path); struct fm_meta meta;
                if(!r && (m.count!=1 || !path[0] || fm_drive_root(m.paths[0])))r=-EINVAL;
                if(!r) { int s=t->host.fs.stat_path(t->host.fs.ctx,path,&meta); r=!s ? -EEXIST : s!=-ENOENT ? s : t->host.fs.rename(t->host.fs.ctx,m.paths[0],path); } }
            else if(!r)r=transfer(t,equal(a[0],"MOVE") || equal(a[0],"mv"),&m,a[2]); release(&m); }
    }
    else if(equal(a[0],"START")) {
        if(n<2 || n>3)r=-EINVAL; else { const char *name=a[1]; char path[FM_PATH];
            if(!strncmp(name,"/Programs/",10))name+=10;
            if(!*name || strlen(name)>DESK_TITLE_MAX || strpbrk(name," /\\\t"))r=-EINVAL;
            if(!r && n==3) { r=term_path(t,a[2],path); if(!r && !path[0])r=-EINVAL; }
            if(!r)r=t->host.launch ? t->host.launch(t->host.ctx,name,n==3 ? path : NULL) : -ENOSYS;
            if(!r)term_write(t,"Launch requested.\n"); }
    }
    else if(equal(a[0],"LUA") || (strlen(a[0])>4 && equal(a[0]+strlen(a[0])-4,".lua"))) {
        if(t->busy==2)r=-EBUSY;
        else if(equal(a[0],"LUA") && n==1) { t->lua_mode=1; term_write(t,"Lua 5.4.8. Type .exit to return.\n"); }
        else r=term_lua(t,equal(a[0],"LUA") ? a[1] : a[0],n-(equal(a[0],"LUA") ? 2 : 1),a+(equal(a[0],"LUA") ? 2 : 1));
    }
    else r=system_cmd(t,n,a);
    if(r)term_printf(t,"%s: %s (error %d)\n",n>0 ? a[0] : "Syntax",strerror(-r),-r);
    if(t->ui)t->ui->dirty=1; return r;
}
char *term_capture(struct terminal *t,const char *line,int *error)
{
    if(t->capture) { *error=-EBUSY; return NULL; }
    t->capture=malloc(TERM_CAPTURE+1); if(!t->capture) { *error=-ENOMEM; return NULL; }
    t->captured=0; t->capture_error=0; t->capture[0]=0;
    *error=term_execute(t,line); if(t->capture_error)*error=t->capture_error;
    char *out=t->capture; t->capture=NULL; return out;
}
