/* SPDX-License-Identifier: MIT
 * Internal, shared shell/Control Panel storage adapter. No external symbols.
 * Readers publish complete snapshots; mutations never run in shell painters. */
#ifndef CIUKI_CUSTOMIZE_IO_H
#define CIUKI_CUSTOMIZE_IO_H
#include "customize.h"
#include "shell_link.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
static inline const char *sh_basename(const char *p)
{ const char *b=strrchr(p,'/'); return b ? b+1 : p; }
static inline int sh_join(char *out,size_t cap,const char *dir,const char *name)
{
    int n=snprintf(out,cap,"%s%s%s",dir,dir[0] && dir[strlen(dir)-1]=='/' ? "" : "/",name);
    return n<0 || (size_t)n>=cap ? -ENAMETOOLONG : 0;
}
static inline int sh_path_valid(const char *path)
{
    if (!path || path[0]!='/' || strlen(path)>=CFG_PATH_MAX) return 0;
    for (const char *p=path;*p;p++) {
        if ((unsigned char)*p<32 || *p==127) return 0;
        if (*p=='/' && p[1]=='.' && (p[2]=='/' || !p[2] || (p[2]=='.' && (p[3]=='/' || !p[3])))) return 0;
    }
    return 1;
}
static inline int sh_mkdirs(const char *path)
{
    char p[CFG_PATH_MAX];
    if (!sh_path_valid(path)) return -EINVAL;
    strcpy(p,path);
    for (char *q=p+1;;q++) if (*q=='/' || !*q) {
        char ch=*q; *q=0;
        if (mkdir(p,0777) && errno!=EEXIST) return -errno;
        *q=ch; if (!ch) break;
    }
    return 0;
}
static inline int sh_write_atomic(const char *path,const void *data,size_t n)
{
    char temp[CFG_PATH_MAX+32];
    int len=snprintf(temp,sizeof(temp),"%s.g20-%ld",path,(long)getpid());
    if (len<0 || (size_t)len>=sizeof(temp)) return -ENAMETOOLONG;
    int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0666);
    if (fd<0) return -errno;
    int err=0; size_t at=0;
    while (at<n) { ssize_t w=write(fd,(const char *)data+at,n-at); if (w<=0) { err=w<0 ? errno : EIO; break; } at+=(size_t)w; }
    if (!err && fsync(fd)) err=errno;
    if (close(fd) && !err) err=errno;
    if (!err && rename(temp,path)) err=errno;
    if (err) unlink(temp);
    return -err;
}
static inline int sh_shortcut_read(const char *path,struct desk_launch_record *r)
{
    FILE *f=fopen(path,"rb"); if (!f) return -errno;
    uint8_t *buf=malloc(DESK_LINK_BYTES+1); if (!buf) { fclose(f); return -ENOMEM; }
    size_t n=fread(buf,1,DESK_LINK_BYTES+1,f); int err=ferror(f) ? EIO : 0;
    if (fclose(f)) err=EIO;
    if (!err && !desk_link_decode(buf,(unsigned)n,r)) err=EINVAL;
    free(buf); return -err;
}
static inline int sh_url_read(const char *path,char *out,size_t cap)
{
    FILE *f=fopen(path,"rb");if (!f) return -errno;
    char line[1024];unsigned total=0;int result=-EINVAL,section=0;
    while (fgets(line,sizeof(line),f) && total<8192) {
        total+=(unsigned)strlen(line);line[strcspn(line,"\r\n")]=0;
        if (line[0]=='[') section=!strcasecmp(line,"[InternetShortcut]");
        if (section && !strncasecmp(line,"URL=",4)) {
            const char *url=line+4;size_t n=strlen(url);
            if (n>=cap) { result=-ENAMETOOLONG;break; }
            if (!strncasecmp(url,"http://",7) || !strncasecmp(url,"https://",8) || !strncasecmp(url,"ftp://",6)) { memcpy(out,url,n+1);result=0;break; }
        }
    }
    if (ferror(f)) result=-EIO;
    if (fclose(f)) result=-EIO;
    return result;
}
static inline int sh_shortcut_add(const char *folder,const char *target,const char *label,const char *args)
{
    struct desk_launch_record *r=calloc(1,sizeof(*r));
    uint8_t *bytes=malloc(DESK_LINK_BYTES);
    if (!r || !bytes) { free(r); free(bytes); return -ENOMEM; }
    int err=0;
    if (strchr(label,'/') || strchr(label,'\\') || !*label || !strcmp(label,".") || !strcmp(label,"..") || strlen(label)>58 ||
        strlen(target)>=sizeof(r->target) || (args && strlen(args)>=sizeof(r->arguments))) err=-EINVAL;
    if (!err) {
        snprintf(r->label,sizeof(r->label),"%s",label); snprintf(r->target,sizeof(r->target),"%s",target);
        if (args) snprintf(r->arguments,sizeof(r->arguments),"%s",args);
        r->show=1;
        if (!strncasecmp(target,"http://",7) || !strncasecmp(target,"https://",8) || !strncasecmp(target,"ftp://",6)) {
            if (strchr(target,'"')) err=-EINVAL;
            snprintf(r->arguments,sizeof(r->arguments),"\"%s\"",target);strcpy(r->target,"/Programs/netsurf");
        }
        /* Dragging a shortcut copies its actual target, arguments, icon and show mode. */
        size_t l=strlen(target);
        if (l>4 && !strcasecmp(target+l-4,".lnk")) err=sh_shortcut_read(target,r);
        snprintf(r->label,sizeof(r->label),"%s",label);
        unsigned n=err ? 0 : desk_link_encode(r,bytes,DESK_LINK_BYTES);
        if (!err && !n) err=-EINVAL;
        if (!err) err=sh_mkdirs(folder);
        char name[64],path[CFG_PATH_MAX]; snprintf(name,sizeof(name),"%s.lnk",label);
        if (!err) err=sh_join(path,sizeof(path),folder,name);
        if (!err) {
            /* Reserve the destination rather than replacing an existing shortcut. */
            int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0666);
            if (fd<0) err=-errno;
            else {
                if (close(fd)) err=-errno;
                if (!err) err=sh_write_atomic(path,bytes,n);
                if (err) unlink(path);
            }
        }
    }
    free(r); free(bytes); return err;
}
static inline int sh_name_valid(const char *name)
{
    if (!*name || name[0]=='.' || strlen(name)>=CFG_PATH_MAX || strchr(name,'/') || strchr(name,'\\')) return 0;
    for (;*name;name++) if ((unsigned char)*name<32 || *name==127) return 0;
    return 1;
}
static inline int sh_order_write(const struct sh_edit_request *r)
{
    char path[CFG_PATH_MAX];size_t n=0,cap=SH_FOLDER_ITEMS*(CFG_PATH_MAX+1)+1;
    if (r->count>SH_FOLDER_ITEMS || sh_join(path,sizeof(path),r->path,".order")) return -EINVAL;
    char *buf=malloc(cap);if (!buf) return -ENOMEM;
    int error=0;
    for (unsigned k=0;k<r->count;k++) {
        if (!sh_name_valid(r->order[k])) { error=-EINVAL;break; }
        int len=snprintf(buf+n,cap-n,"%s\n",r->order[k]);
        if (len<0 || (size_t)len>=cap-n) { error=-ENOSPC;break; }
        n+=(size_t)len;
    }
    if (!error) error=sh_write_atomic(path,buf,n);
    free(buf);return error;
}
/* The icon object of a Start menu / toolbar entry (libdeskui icon_map.c keys): folders by their well-known
 * name, shortcuts by their target (/Programs/<name> or a Windows path), documents by extension. */
static inline void sh_object_set(char *out,const char *kind,const char *name)
{
    int n=snprintf(out,sizeof(((struct sh_folder_item *)0)->icon),"%s%s",kind,name);
    if (n<0 || (size_t)n>=sizeof(((struct sh_folder_item *)0)->icon)) out[0]=0;
}
static inline void sh_object_ext(char *out,const char *name)
{
    const char *base=name,*p;
    for (p=name;*p;p++) if (*p=='/' || *p=='\\') base=p+1;
    const char *dot=strrchr(base,'.');
    if (!dot || dot==base || !dot[1] || strlen(dot)>10) { out[0]=0; return; }
    char ext[16]; size_t k=0;
    for (;dot[k] && k<sizeof(ext)-1;k++) ext[k]=(char)(dot[k]>='A' && dot[k]<='Z' ? dot[k]+32 : dot[k]);
    ext[k]=0; sh_object_set(out,"type:",ext);
}
static inline void sh_item_icon(struct sh_folder_item *e,int is_dir,size_t len)
{
    e->icon[0]=0;
    if (is_dir) {
        static const struct { const char *label,*object; } known[]={
            {"Accessories","menu:accessories"},{"System Tools","menu:system-tools"},{"Games","menu:games"},
            {"Developer","menu:development"},{"Multimedia","menu:multimedia"},{"Internet","menu:internet"},{"Office","menu:office"}};
        for (unsigned k=0;k<sizeof(known)/sizeof(*known);k++) if (!strcasecmp(e->label,known[k].label)) { strcpy(e->icon,known[k].object);return; }
        return;
    }
    const char *name=sh_basename(e->path);
    if (len<4 || strcasecmp(name+len-4,".lnk")) {
        if (len>4 && !strcasecmp(name+len-4,".url")) strcpy(e->icon,"program:netsurf"); else sh_object_ext(e->icon,name);
        return;
    }
    struct desk_launch_record *r=calloc(1,sizeof(*r));
    if (!r) return;
    if (!sh_shortcut_read(e->path,r)) {
        const char *t=r->target;
        if (!strncasecmp(t,"http://",7) || !strncasecmp(t,"https://",8) || !strncasecmp(t,"ftp://",6)) strcpy(e->icon,"program:netsurf");
        else {
            const char *base=t; for (const char *p=t;*p;p++) if (*p=='/' || *p=='\\') base=p+1;
            char program[32]; size_t k=0;
            const char *dot=strrchr(base,'.');
            int native=!strncmp(t,"/Programs/",10);
            int executable=native || (dot && (!strcasecmp(dot,".exe") || !strcasecmp(dot,".com") || !strcasecmp(dot,".elf")));
            if (!dot && !native) strcpy(e->icon,"type:folder");   /* a folder shortcut */
            else if (executable) {
                for (const char *p=base;*p && *p!='.' && k<sizeof(program)-1;p++) program[k++]=(char)(*p>='A' && *p<='Z' ? *p+32 : *p);
                program[k]=0;
                if (!strcmp(program,"dosbox") && r->arguments[0]) sh_object_set(e->icon,"menu:","game");   /* a bundled DOS game */
                else if (k) sh_object_set(e->icon,"program:",program);
            } else sh_object_ext(e->icon,base);
        }
    }
    free(r);
}
static inline void sh_folder_scan(struct sh_folder_snapshot *s,unsigned folder)
{
    const char *path=s->folders[folder]; if (!*path) return;
    DIR *dir=opendir(path); if (!dir) return;
    unsigned first=s->count;
    struct dirent *de;
    while ((de=readdir(dir))) {
        if (!sh_name_valid(de->d_name) || (folder==SH_ROOT_START && !strcasecmp(de->d_name,"Programs"))) continue;
        if (s->count>=SH_ENTRIES || s->count-first>=SH_FOLDER_ITEMS) { s->truncated=1; continue; }
        struct sh_folder_item *e=&s->entries[s->count];
        if (sh_join(e->path,sizeof(e->path),path,de->d_name)) { s->truncated=1; continue; }
        struct stat st; if (stat(e->path,&st)) continue;
        int is_dir=S_ISDIR(st.st_mode); size_t len=strlen(de->d_name);
        if ((folder==SH_ROOT_START || folder==SH_ROOT_PROGRAMS) && !is_dir && (len<4 || strcasecmp(de->d_name+len-4,".lnk"))) continue;
        e->parent=(int)folder; e->folder=-1;
        snprintf(e->label,sizeof(e->label),"%.*s",(int)(len>4 && !strcasecmp(de->d_name+len-4,".lnk") ? len-4 : len),de->d_name);
        if (is_dir) {
            if (s->folders_count>=SH_FOLDERS) { s->truncated=1; continue; }
            e->folder=(int)s->folders_count;
            strcpy(s->folders[s->folders_count++],e->path);
        }
        sh_item_icon(e,is_dir,len);
        s->count++;
    }
    closedir(dir);
    for (unsigned k=first+1;k<s->count;k++) {
        struct sh_folder_item e=s->entries[k]; unsigned j=k;
        while (j>first && strcasecmp(s->entries[j-1].label,e.label)>0) { s->entries[j]=s->entries[j-1];j--; }
        s->entries[j]=e;
    }
    char order[CFG_PATH_MAX]; if (sh_join(order,sizeof(order),path,".order")) return;
    FILE *f=fopen(order,"rb"); if (!f) return;
    unsigned pos=first; char name[CFG_PATH_MAX+1];
    while (pos<s->count && fgets(name,sizeof(name),f)) {
        name[strcspn(name,"\r\n")]=0;
        for (unsigned k=pos;k<s->count;k++) if (!strcmp(sh_basename(s->entries[k].path),name)) {
            struct sh_folder_item e=s->entries[k];
            memmove(s->entries+pos+1,s->entries+pos,(k-pos)*sizeof(e)); s->entries[pos++]=e; break;
        }
    }
    fclose(f);
}
static inline void sh_snapshot_load(struct sh_folder_snapshot *s,const struct ciuki_settings *cfg)
{
    memset(s,0,sizeof(*s)); s->folders_count=SH_ROOTS;
    struct stat seeded;s->native_seeded=!stat(SH_START_FOLDER "/.g20-seeded",&seeded);
    const char *roots[]={SH_START_FOLDER,SH_PROGRAMS_FOLDER,SH_QUICK_FOLDER,SH_LINKS_FOLDER,SH_DESKTOP_FOLDER};
    for (unsigned k=0;k<SH_ROOT_CUSTOM;k++) snprintf(s->folders[k],CFG_PATH_MAX,"%s",roots[k]);
    for (unsigned k=0;k<CFG_TOOLBARS;k++) strcpy(s->folders[SH_ROOT_CUSTOM+k],cfg->toolbar_folder[k]);
    strcpy(s->folders[SH_ROOT_FAVORITES],SH_FAVORITES_FOLDER);
    for (unsigned k=0;k<s->folders_count;k++) sh_folder_scan(s,k);
}
static inline int sh_recent_clear(const char *path)
{ return unlink(path) && errno!=ENOENT ? -errno : 0; }
static inline int sh_menu_remove(const char *path,unsigned depth)
{
    if (!sh_path_valid(path) || depth>16) return -EINVAL;
    struct stat st;if (stat(path,&st)) return -errno;
    if (!S_ISDIR(st.st_mode)) return unlink(path) ? -errno : 0;
    DIR *dir=opendir(path);if (!dir) return -errno;
    int err=0;unsigned count=0;struct dirent *de;
    while (!err && (de=readdir(dir))) {
        if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
        if (++count>SH_ENTRIES) { err=-E2BIG;break; }
        char child[CFG_PATH_MAX];err=sh_join(child,sizeof(child),path,de->d_name);
        if (!err) err=sh_menu_remove(child,depth+1);
    }
    closedir(dir);
    return err ? err : rmdir(path) ? -errno : 0;
}
static inline int sh_edit_apply(const struct sh_edit_request *r)
{
    switch (r->operation) {
    case SH_EDIT_ADD:
        if (r->sources_count) {
            if (r->sources_count>SH_FOLDER_ITEMS) return -E2BIG;
            for (unsigned k=0;k<r->sources_count;k++) {
                char label[64];snprintf(label,sizeof(label),"%.*s",58,sh_basename(r->sources[k]));
                size_t n=strlen(label);if (n>4 && !strcasecmp(label+n-4,".lnk")) label[n-4]=0;
                int err=sh_shortcut_add(r->destination,r->sources[k],label,NULL);
                if (err) return err;
            }
            return 0;
        }
        return sh_shortcut_add(r->destination,r->path,r->name,NULL);
    case SH_EDIT_DELETE: return sh_menu_remove(r->path,0);
    case SH_EDIT_RENAME: {
        char parent[CFG_PATH_MAX],path[CFG_PATH_MAX];
        if (!sh_name_valid(r->name) || strlen(r->path)>=sizeof(parent)) return -EINVAL;
        strcpy(parent,r->path); char *last=strrchr(parent,'/'); if (!last) return -EINVAL; *last=0;
        char name[68];snprintf(name,sizeof(name),"%s",r->name);
        size_t old_len=strlen(r->path),new_len=strlen(name);
        if (old_len>4 && !strcasecmp(r->path+old_len-4,".lnk") && (new_len<4 || strcasecmp(name+new_len-4,".lnk"))) {
            if (new_len>58) return -ENAMETOOLONG;
            strcat(name,".lnk");
        }
        if (!sh_path_valid(r->path) || sh_join(path,sizeof(path),parent,name)) return -EINVAL;
        struct stat st; if (!stat(path,&st)) return -EEXIST;
        return rename(r->path,path) ? -errno : 0;
    }
    case SH_EDIT_ORDER: return sh_order_write(r);
    case SH_EDIT_CLEAR: return sh_recent_clear(SH_RECENT_FILE);
    case SH_EDIT_TOOLBAR: { struct stat st; if (!sh_path_valid(r->path)) return -EINVAL;return stat(r->path,&st) ? -errno : S_ISDIR(st.st_mode) ? 0 : -ENOTDIR; }
    default: return 0;
    }
}
/* Existing shell-only entries form the older tail. Apps' list is newest first. */
static inline int sh_recent_merge(struct ciuki_settings *cfg,const char *path)
{
    FILE *f=fopen(path,"rb"); if (!f) return errno==ENOENT ? 0 : -errno;
    char (*paths)[CFG_PATH_MAX]=calloc(CFG_RECENT_MAX,CFG_PATH_MAX);
    if (!paths) { fclose(f); return -ENOMEM; }
    unsigned count=0; char line[1024];
    while (fgets(line,sizeof(line),f)) {
        size_t n=strcspn(line,"\r\n");
        if (line[n]==0 && !feof(f)) { int ch; while ((ch=fgetc(f))!=EOF && ch!='\n') {} continue; }
        line[n]=0;
        if (!n || n>=CFG_PATH_MAX || !sh_path_valid(line)) continue;
        int found=0; for (unsigned k=0;k<count;k++) if (!strcmp(paths[k],line)) found=1;
        if (!found && count<CFG_RECENT_MAX) strcpy(paths[count++],line);
    }
    int err=ferror(f) ? -EIO : 0; fclose(f);
    if (!err) {
        for (unsigned i=0;i<cfg->recent_count && count<CFG_RECENT_MAX;i++) {
            int found=0; for (unsigned k=0;k<count;k++) if (!strcmp(paths[k],cfg->recent[i])) found=1;
            if (!found) strcpy(paths[count++],cfg->recent[i]);
        }
        memcpy(cfg->recent,paths,CFG_RECENT_MAX*CFG_PATH_MAX);cfg->recent_count=count;
    }
    free(paths);return err;
}
#endif
