/* SPDX-License-Identifier: MIT
 * sec-user-05: the Windows XP merged desktop. In a profile session the
 * desktop shows the current user's Desktop folder (CSIDL_DESKTOPDIRECTORY,
 * <profile>/Documents/Desktop) merged with the folder every user sees
 * (/Documents/Desktop, the machine desktop that holds the Quake III Arena
 * Demo anchor; the Win32 common desktop C:\WINDOWS\Desktop keeps reaching
 * the desktop through the shortcut scan). Microsoft documents the two
 * folders behind the one desktop (CSIDL, CSIDL_COMMON_DESKTOPDIRECTORY:
 * "files and folders that appear on the desktop for all users").
 *
 * The view's path is the per-user folder. Its listing is the per-user
 * entries followed by the shared entries whose names the user has not
 * taken. Every other operation addresses one physical item: a path beneath
 * the view maps to the per-user item when it exists, else to the shared
 * item when that exists, else (a new item) to the per-user folder, created
 * on first use. fm_entry_path hands out physical paths (desk_union_physical,
 * no I/O: the shared names of the last listing), so clipboards, jobs and
 * launches never see the merged spelling. Locations only, never an
 * authority: the kernel checks every access (a Standard user cannot change
 * an administrator-owned shared item). Header-only: folder.c and its host
 * test include it. */
#ifndef DESK_UNION_H
#define DESK_UNION_H
#include "../files/model.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
struct desk_union {
    struct fm_vfs vfs;              /* the view's table; ctx is this record */
    const struct fm_vfs *base;
    char user[FM_PATH], shared[FM_PATH];
    pthread_mutex_t lock;
    char (*names)[FM_NAME];         /* shared entries of the last merged listing */
    unsigned count;
};
/* The remainder of path beneath the view ("" for the view itself), or NULL. */
static inline const char *desk_union_rest(const struct desk_union *u, const char *path)
{
    size_t n=strlen(u->user);
    if (strncasecmp(path,u->user,n)) return NULL;   /* FAT names compare without case */
    if (!path[n]) return path+n;
    return path[n]=='/' ? path+n+1 : NULL;
}
static inline int desk_union_join(char *out, const char *root, const char *rest)
{
    int n=*rest ? snprintf(out,FM_PATH,"%s/%s",root,rest) : snprintf(out,FM_PATH,"%s",root);
    return n<0 || n>=FM_PATH ? -ENAMETOOLONG : 0;
}
/* The first component of rest exists beneath root. */
static inline int desk_union_has(const struct desk_union *u, const char *root, const char *rest)
{
    char p[FM_PATH]; size_t k=strcspn(rest,"/");
    int n=snprintf(p,sizeof(p),"%s/%.*s",root,(int)k,rest);
    struct fm_meta meta;
    return n>0 && n<FM_PATH && !u->base->stat_path(u->base->ctx,p,&meta);
}
/* The per-user folder, created on first use with any missing parent. */
static inline int desk_union_ensure(const struct desk_union *u)
{
    struct fm_meta meta; char p[FM_PATH];
    if (!u->base->stat_path(u->base->ctx,u->user,&meta)) return 0;
    snprintf(p,sizeof(p),"%s",u->user);
    for (char *s=p+1;;s++) {
        if (*s && *s!='/') continue;
        char c=*s; *s=0;
        if (u->base->stat_path(u->base->ctx,p,&meta)) {
            int r=u->base->mkdir(u->base->ctx,p);
            if (r && r!=-EEXIST) return r;
        }
        *s=c; if (!c) return 0;
    }
}
/* Physical path of path; create: a new item may be made there. */
static inline int desk_union_map(const struct desk_union *u, const char *path, char *out, int create)
{
    const char *rest=desk_union_rest(u,path);
    if (!rest) return snprintf(out,FM_PATH,"%s",path)>=FM_PATH ? -ENAMETOOLONG : 0;
    if (!*rest) return desk_union_join(out,u->user,"");
    if (!desk_union_has(u,u->user,rest) && desk_union_has(u,u->shared,rest)) return desk_union_join(out,u->shared,rest);
    if (create && !strchr(rest,'/')) { int r=desk_union_ensure(u); if (r) return r; }
    return desk_union_join(out,u->user,rest);
}
struct desk_union_listing {
    struct desk_union *u; fm_emit emit; void *arg; int stop, shared;
    char (*mine)[FM_NAME]; unsigned mine_count, mine_cap;
    char (*theirs)[FM_NAME]; unsigned theirs_count, theirs_cap;
};
static inline int desk_union_keep(char (**names)[FM_NAME], unsigned *count, unsigned *cap, const char *name)
{
    if (*count==*cap) {
        unsigned next=*cap ? *cap*2 : 16;
        if (next>FM_ENTRIES) return -E2BIG;
        void *grown=realloc(*names,(size_t)next*FM_NAME); if (!grown) return -ENOMEM;
        *names=grown; *cap=next;
    }
    snprintf((*names)[(*count)++],FM_NAME,"%s",name); return 0;
}
static inline int desk_union_emit(const char *name, const struct fm_meta *meta, void *arg)
{
    struct desk_union_listing *l=arg;
    if (l->shared) {
        for (unsigned i=0;i<l->mine_count;i++) if (!strcasecmp(l->mine[i],name)) return 0; /* the user's item wins */
        int r=desk_union_keep(&l->theirs,&l->theirs_count,&l->theirs_cap,name); if (r) return l->stop=r;
    } else {
        int r=desk_union_keep(&l->mine,&l->mine_count,&l->mine_cap,name); if (r) return l->stop=r;
    }
    int r=l->emit(name,meta,l->arg);
    if (r) l->stop=r;
    return r;
}
static inline int desk_union_list(void *ctx, const char *path, unsigned flags, fm_emit emit, void *arg)
{
    struct desk_union *u=ctx;
    if (strcasecmp(path,u->user)) {
        char p[FM_PATH]; int r=desk_union_map(u,path,p,0);
        return r ? r : u->base->list(u->base->ctx,p,flags,emit,arg);
    }
    struct desk_union_listing l={.u=u,.emit=emit,.arg=arg};
    int r=u->base->list(u->base->ctx,u->user,flags,desk_union_emit,&l);
    if (l.stop) r=l.stop;
    else if (r==-ENOENT) r=0;          /* the per-user folder is made on first use */
    if (!r) {
        l.shared=1;
        int e=u->base->list(u->base->ctx,u->shared,flags,desk_union_emit,&l);
        if (l.stop) r=l.stop;
        else (void)e;                  /* an unreadable shared folder adds nothing */
    }
    if (!r) {
        pthread_mutex_lock(&u->lock);
        free(u->names); u->names=l.theirs; u->count=l.theirs_count; l.theirs=NULL;
        pthread_mutex_unlock(&u->lock);
    }
    free(l.mine); free(l.theirs);
    return r;
}
static inline int desk_union_physical(void *ctx, const char *path, char *out)
{
    struct desk_union *u=ctx; const char *rest=desk_union_rest(u,path);
    if (!rest || !*rest) return snprintf(out,FM_PATH,"%s",path)>=FM_PATH ? -ENAMETOOLONG : 0;
    size_t k=strcspn(rest,"/"); int shared=0;
    pthread_mutex_lock(&u->lock);
    for (unsigned i=0;i<u->count && !shared;i++) shared=strlen(u->names[i])==k && !strncmp(u->names[i],rest,k);
    pthread_mutex_unlock(&u->lock);
    return desk_union_join(out,shared ? u->shared : u->user,rest);
}
static inline int desk_union_stat(void *ctx, const char *path, struct fm_meta *meta)
{
    struct desk_union *u=ctx; char p[FM_PATH];
    if (!strcasecmp(path,u->user)) {
        int r=u->base->stat_path(u->base->ctx,u->user,meta);
        return r==-ENOENT ? u->base->stat_path(u->base->ctx,u->shared,meta) : r;
    }
    int r=desk_union_map(u,path,p,0);
    return r ? r : u->base->stat_path(u->base->ctx,p,meta);
}
static inline int desk_union_mkdir(void *ctx, const char *path)
{
    struct desk_union *u=ctx; char p[FM_PATH];
    if (!strcasecmp(path,u->user)) { int r=desk_union_ensure(u); return r ? r : -EEXIST; }
    int r=desk_union_map(u,path,p,1);
    return r ? r : u->base->mkdir(u->base->ctx,p);
}
static inline int desk_union_rename(void *ctx, const char *from, const char *to)
{
    struct desk_union *u=ctx; char a[FM_PATH], b[FM_PATH];
    int r=desk_union_map(u,from,a,0); if (!r) r=desk_union_map(u,to,b,1);
    return r ? r : u->base->rename(u->base->ctx,a,b);
}
static inline int desk_union_unlink(void *ctx, const char *path)
{
    struct desk_union *u=ctx; char p[FM_PATH]; int r=desk_union_map(u,path,p,0);
    return r ? r : u->base->unlink(u->base->ctx,p);
}
static inline int desk_union_rmdir(void *ctx, const char *path)
{
    struct desk_union *u=ctx; char p[FM_PATH]; int r=desk_union_map(u,path,p,0);
    return r ? r : u->base->rmdir(u->base->ctx,p);
}
static inline int desk_union_open(void *ctx, const char *path, int create)
{
    struct desk_union *u=ctx; char p[FM_PATH]; int r=desk_union_map(u,path,p,create);
    return r ? r : u->base->open(u->base->ctx,p,create);
}
static inline int desk_union_read(void *ctx, int fd, void *p, unsigned n)
{ struct desk_union *u=ctx; return u->base->read(u->base->ctx,fd,p,n); }
static inline int desk_union_write(void *ctx, int fd, const void *p, unsigned n)
{ struct desk_union *u=ctx; return u->base->write(u->base->ctx,fd,p,n); }
static inline int desk_union_sync(void *ctx, int fd)
{ struct desk_union *u=ctx; return u->base->sync(u->base->ctx,fd); }
static inline int desk_union_close(void *ctx, int fd)
{ struct desk_union *u=ctx; return u->base->close(u->base->ctx,fd); }
static inline int desk_union_metadata(void *ctx, const char *path, const struct fm_meta *meta)
{
    struct desk_union *u=ctx; char p[FM_PATH]; int r=desk_union_map(u,path,p,0);
    return r ? r : u->base->metadata(u->base->ctx,p,meta);
}
/* user: the view (per-user folder); shared: the folder every user sees. */
static inline struct desk_union *desk_union_create(const struct fm_vfs *base, const char *user, const char *shared)
{
    if (!base || !base->list || !base->stat_path || !base->mkdir || !user || !shared || user[0]!='/' || shared[0]!='/' ||
        strlen(user)>=FM_PATH || strlen(shared)>=FM_PATH || !strcmp(user,shared)) return NULL;
    struct desk_union *u=calloc(1,sizeof(*u)); if (!u) return NULL;
    if (pthread_mutex_init(&u->lock,NULL)) { free(u); return NULL; }
    u->base=base; snprintf(u->user,sizeof(u->user),"%s",user); snprintf(u->shared,sizeof(u->shared),"%s",shared);
    u->vfs=(struct fm_vfs){u,desk_union_list,desk_union_stat,desk_union_mkdir,
        base->rename ? desk_union_rename : NULL,base->unlink ? desk_union_unlink : NULL,base->rmdir ? desk_union_rmdir : NULL,
        base->open ? desk_union_open : NULL,base->read ? desk_union_read : NULL,base->write ? desk_union_write : NULL,
        base->sync ? desk_union_sync : NULL,base->close ? desk_union_close : NULL,
        base->metadata ? desk_union_metadata : NULL,desk_union_physical};
    return u;
}
static inline void desk_union_destroy(struct desk_union *u)
{
    if (!u) return;
    pthread_mutex_destroy(&u->lock); free(u->names); free(u);
}
#endif
