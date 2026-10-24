/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "users.h"
#include "settings_io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __i386__
#include <ciuki/entropy.h>
#else
#include <sys/random.h>
#endif
#define DB_BYTES 1400u
static int dirs(const char *path)
{
    char buf[512]; if (strlen(path)>=sizeof(buf)) return -ENAMETOOLONG;
    strcpy(buf,path);
    for (char *s=buf+1;;s++) if (!*s || *s=='/') {
        char end=*s; *s=0;
        if (mkdir(buf,0755) && errno!=EEXIST) return -errno;
        *s=end; if (!end) break;
    }
    return 0;
}
static int copy_default(const char *root,const char *destination)
{
    char from[512], to[512], temp[512]; struct stat st;
    snprintf(from,sizeof(from),"%s/system/settings/desktop.cfg",root);
    snprintf(to,sizeof(to),"%s%s",root,destination);
    if (!stat(to,&st)) return 0;
    if (errno!=ENOENT) return -errno;
    FILE *f=fopen(from,"rb"); if (!f) return errno==ENOENT ? 0 : -errno;
    char *text=malloc(CFG_TEXT_MAX+1); if (!text) { fclose(f); return -ENOMEM; }
    size_t n=fread(text,1,CFG_TEXT_MAX+1,f); int e=ferror(f) ? -EIO : n>CFG_TEXT_MAX ? -E2BIG : 0;
    fclose(f); snprintf(temp,sizeof(temp),"%s%s.new",root,destination);
    if (!e) e=-cfg_write_at(to,temp,text,n);
    free(text); return e;
}
static __attribute__((noinline)) int prepare(const char *root,const struct users_record *r,unsigned flags)
{
    if (!(flags&USERS_PROFILES)) return 0;
    struct user_profile p; users_profile(&p,r->name,r->id,flags);
    char path[512];
    const char *const paths[]={p.desktop,p.programs,"Favorites","Documents/Music","Documents/Pictures"};
    for (unsigned i=0;i<5;i++) {
        if (i<2) snprintf(path,sizeof(path),"%s%s",root,paths[i]);
        else snprintf(path,sizeof(path),"%s%s/%s",root,p.root,paths[i]);
        int e=dirs(path); if (e) return e;
    }
    return copy_default(root,p.settings);
}
int users_execute(const char *root,const struct users_request *q,struct users_view *v)
{
    memset(v,0,sizeof(*v));
    char path[512], temp[512], lock[512];
    if (!root || strlen(root)>300) return -EINVAL;
    snprintf(path,sizeof(path),"%s/system/settings/users.dat",root);
    snprintf(temp,sizeof(temp),"%s/system/settings/users.new",root);
    snprintf(lock,sizeof(lock),"%s/system/settings/users.lock",root);
    int mutation=q->operation!=USERS_READ && q->operation!=USERS_AUTH, locked=0, e=0;
    if (mutation) {
        char parent[512]; snprintf(parent,sizeof(parent),"%s/system/settings",root);
        if ((e=dirs(parent))) return e;
        int fd=open(lock,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        if (fd<0) return errno==EEXIST ? -EBUSY : -errno;
        locked=1; close(fd);
    }
    struct users_db *db=calloc(1,sizeof(*db)); unsigned char *bytes=malloc(DB_BYTES+1);
    if (!db || !bytes) { e=-ENOMEM; goto done; }
    db->next_id=1;
    FILE *f=fopen(path,"rb");
    if (f) {
        size_t n=fread(bytes,1,DB_BYTES+1,f);
        e=ferror(f) ? -EIO : users_decode(db,bytes,n); fclose(f);
    } else if (errno!=ENOENT) e=-errno;
    if (e) goto done;
    uint8_t salt[16]={0};
    if (q->operation==USERS_CREATE || q->operation==USERS_CHANGE) {
#ifdef __i386__
        if (getentropy(salt,sizeof(salt))) e=-errno;
#else
        if (getrandom(salt,sizeof(salt),0)!=(ssize_t)sizeof(salt)) e=-EIO;
#endif
    }
    if (!e) e=users_apply(db,q,salt,v);
    users_erase(salt,sizeof(salt));
    if (!e && (q->operation==USERS_CREATE || q->operation==USERS_AUTH || q->operation==USERS_POLICY))
        for (unsigned i=0;i<db->count && !e;i++)
            if (q->operation!=USERS_AUTH || db->records[i].id==v->authenticated) e=prepare(root,&db->records[i],db->flags);
    if (!e && mutation) {
        size_t n=users_encode(db,bytes,DB_BYTES);
        e=n ? -cfg_write_at(path,temp,(const char *)bytes,n) : -E2BIG;
    }
done:
    if (db) { users_erase(db,sizeof(*db)); free(db); }
    if (bytes) { users_erase(bytes,DB_BYTES+1); free(bytes); }
    if (locked) unlink(lock);
    if (e) memset(v,0,sizeof(*v));
    return e;
}
