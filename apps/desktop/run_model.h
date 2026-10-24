/* G9 bounded Run parsing and RunMRU; no command interpreter. SPDX-License-Identifier: MIT */
#ifndef DESK_RUN_MODEL_H
#define DESK_RUN_MODEL_H
#include "../files/operations.h"
#include "../../sdk/win32/include/w32/server.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define DESK_RUN_MAX 1024
#define DESK_RUN_MRU 26
#define DESK_RUN_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\RunMRU"
struct desk_run_history {
    char order[DESK_RUN_MRU+1];
    char commands[DESK_RUN_MRU][DESK_RUN_MAX];
};
typedef const char *(*desk_run_environment)(void *,const char *);
static inline void desk_run_trim(char *text)
{
    char *start=text; while (*start==' ' || *start=='\t') start++;
    size_t n=strlen(start); while (n && (start[n-1]==' ' || start[n-1]=='\t')) n--;
    memmove(text,start,n); text[n]=0;
}
static inline int desk_run_expand(const char *in,char *out,unsigned cap,desk_run_environment env,void *ctx)
{
    unsigned at=0;
    for (const char *p=in;*p;) {
        const char *value=p; unsigned n=1;
        if (*p=='%') {
            const char *end=strchr(p+1,'%');
            if (end && end-p<256) {
                char name[256]; unsigned size=(unsigned)(end-p-1);
                memcpy(name,p+1,size); name[size]=0;
                const char *found=env ? env(ctx,name) : NULL;
                if (found) { value=found; n=(unsigned)strlen(found); }
                else n=(unsigned)(end-p+1);
                p=end+1;
            } else p++;
        } else p++;
        if (at+n>=cap) return -E2BIG;
        memcpy(out+at,value,n); at+=n;
    }
    if (!cap) return -E2BIG;
    out[at]=0; return 0;
}
/* Preserve the argument spelling for the shared native/DOS/PE bridge. */
static __attribute__((unused,noinline)) int desk_run_split(const char *line,char target[260],char arguments[DESK_RUN_MAX])
{
    while (*line==' ' || *line=='\t') line++;
    int quoted=*line=='"'; if (quoted) line++;
    unsigned at=0;
    while (*line && (quoted ? *line!='"' : *line!=' ' && *line!='\t')) {
        if (at+1>=260) return -ENAMETOOLONG;
        target[at++]=*line++;
    }
    target[at]=0;
    if (!at || (quoted && *line++!='"') || (*line && *line!=' ' && *line!='\t')) return -EINVAL;
    while (*line==' ' || *line=='\t') line++;
    if (strlen(line)>=DESK_RUN_MAX) return -E2BIG;
    /* Validate quoting and bounds even for DOS/Win32 commands. */
    char *scratch=malloc(strlen(line)+1); if (!scratch) return -ENOMEM;
    strcpy(scratch,line); char *words[128]; int count=fm_launch_arguments(scratch,words,128); free(scratch);
    if (count<0) return count;
    strcpy(arguments,line); return 0;
}
static inline int desk_run_history_load(struct desk_run_history *h)
{
    memset(h,0,sizeof(*h)); char order[DESK_RUN_MRU+1];
    int e=fm_registry_root_get(W32R_HKCU,DESK_RUN_KEY,"MRUList",order,sizeof(order));
    if (e) return e==-ENOENT || e==-EINVAL || e==-EOVERFLOW ? 0 : e;
    unsigned used=0,seen=0;
    for (unsigned i=0;order[i];i++) {
        unsigned slot=(unsigned)(order[i]-'a');
        if (slot>=DESK_RUN_MRU || (seen&(1u<<slot))) continue;
        seen|=1u<<slot; char name[2]={(char)('a'+slot),0},value[DESK_RUN_MAX+2];
        e=fm_registry_root_get(W32R_HKCU,DESK_RUN_KEY,name,value,sizeof(value));
        if (e) { if (e==-ENOENT || e==-EINVAL || e==-EOVERFLOW) continue; return e; }
        size_t n=strlen(value);
        if (n<3 || strcmp(value+n-2,"\\1")) continue;
        value[n-2]=0; strcpy(h->commands[slot],value); h->order[used++]=name[0];
    }
    return 0;
}
static inline int desk_run_history_add(struct desk_run_history *h,const char *command)
{
    size_t size=strlen(command); if (!size || size>=DESK_RUN_MAX) return -E2BIG;
    unsigned count=(unsigned)strlen(h->order),at=0,slot=0,used=0;
    for (;at<count;at++) {
        slot=(unsigned)(h->order[at]-'a'); used|=1u<<slot;
        if (!strcasecmp(command,h->commands[slot])) break;
    }
    if (at==count) {
        if (count==DESK_RUN_MRU) { at=count-1; slot=(unsigned)(h->order[at]-'a'); }
        else { for (slot=0;used&(1u<<slot);slot++) {} at=count++; }
    }
    char value[DESK_RUN_MAX+2],order[DESK_RUN_MRU+1],name[2]={(char)('a'+slot),0};
    snprintf(value,sizeof(value),"%s\\1",command);
    memcpy(order,h->order,sizeof(order)); memmove(order+1,order,at); order[0]=name[0]; order[count]=0;
    int e=fm_registry_root_set(W32R_HKCU,DESK_RUN_KEY,name,value);
    if (!e) e=fm_registry_root_set(W32R_HKCU,DESK_RUN_KEY,"MRUList",order);
    if (!e) { strcpy(h->commands[slot],command); strcpy(h->order,order); }
    return e;
}
static __attribute__((unused,noinline)) int desk_run_candidate(const struct fm_vfs *fs,const char *directory,const char *name,char out[260])
{
    static const char *const suffixes[]={"",".exe",".com",".bat",".pif"};
    for (unsigned i=0;i<5;i++) {
        char raw[FM_PATH]; int n=snprintf(raw,sizeof(raw),"%s%s%s%s",directory,
            directory[0] && directory[strlen(directory)-1]!='/' && directory[strlen(directory)-1]!='\\'?"/":"",name,suffixes[i]);
        if (n<0 || (unsigned)n>=sizeof(raw)) return -ENAMETOOLONG;
        char path[FM_PATH]; int e=fm_posix_path(path,raw); if (e) return e;
        struct fm_meta meta; e=fs->stat_path(fs->ctx,path,&meta);
        if (!e) { if (strlen(path)>=260) return -ENAMETOOLONG; strcpy(out,path); return 0; }
        if (e!=-ENOENT && e!=-ENOTDIR) return e;
    }
    return -ENOENT;
}
static __attribute__((unused,noinline)) int desk_run_resolve(const struct fm_vfs *fs,const char *line,desk_run_environment env,void *ctx,struct desk_launch_record *r)
{
    char expanded[DESK_RUN_MAX],target[260],arguments[DESK_RUN_MAX];
    int e=desk_run_expand(line,expanded,sizeof(expanded),env,ctx); if (e) return e;
    desk_run_trim(expanded);
    /* An unquoted, complete existing path with spaces is an object too. */
    struct fm_meta meta; char native[FM_PATH];
    if (expanded[0]!='"' && strlen(expanded)<260 && !fm_posix_path(native,expanded) && !fs->stat_path(fs->ctx,native,&meta)) {
        strcpy(target,native); arguments[0]=0;
    } else { e=desk_run_split(expanded,target,arguments); if (e) return e; }
    if (!strncasecmp(target,"http://",7)) memcpy(target,"http://",7);
    else if (!strncasecmp(target,"https://",8)) memcpy(target,"https://",8);
    if (target[0]!='/' && !(target[0] && target[1]==':') && strncmp(target,"http://",7) && strncmp(target,"https://",8)) {
        char found[260]; e=desk_run_candidate(fs,"/",target,found);
        const char *path=env ? env(ctx,"PATH") : NULL;
        for (const char *p=path;e==-ENOENT && p && *p;) {
            const char *end=strchr(p,';'); if (!end) end=p+strlen(p);
            size_t n=(size_t)(end-p); char directory[260];
            if (n>=sizeof(directory)) return -ENAMETOOLONG;
            if (n) { memcpy(directory,p,n); directory[n]=0; e=desk_run_candidate(fs,directory,target,found); }
            p=*end ? end+1 : end;
        }
        static const char *const defaults[]={"/Programs","/WINDOWS","/WINDOWS/SYSTEM","/DOS"};
        for (unsigned i=0;e==-ENOENT && i<4;i++) e=desk_run_candidate(fs,defaults[i],target,found);
        if (e) return e;
        strcpy(target,found);
    } else if (target[0]=='/' || (target[0] && target[1]==':')) {
        char found[260]; e=desk_run_candidate(fs,"",target,found);
        if (e) return e;
        strcpy(target,found);
    }
    e=fm_launch_resolve(fs,target,NULL,r); if (e<0) return e;
    if (arguments[0]) {
        size_t a=strlen(r->arguments),b=strlen(arguments);
        if (a+b+2>sizeof(r->arguments)) return -E2BIG;
        if (a) r->arguments[a++]=' ';
        memcpy(r->arguments+a,arguments,b+1);
    }
    return 0;
}
#endif
