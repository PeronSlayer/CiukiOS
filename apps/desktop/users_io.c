/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "users.h"
#include "settings_io.h"
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
/* abi.h also defines __CIUKIOS__ in Linux replay fixtures. Select the
 * actual native adapter by target architecture; tests can inject it explicitly. */
#if defined(__i386__) || defined(USERS_NATIVE_LOCK)
#define USERS_NATIVE_LOCK 1
#include <ciuki/proc.h>
#include <ciuki/recovery.h>
#else
#include <sys/file.h>
#endif
#include "../../sdk/win32/include/w32/server.h"
#include <unistd.h>
#ifdef __i386__
#include <ciuki/entropy.h>
#else
#include <sys/random.h>
#endif
#define DB_BYTES 1400u
/* The lease serializes recovery as well as writes. Unlinking an observed
 * stale O_EXCL lock without this guard races another stale-lock remover. */
extern uint32_t desk_w32_exchange(const void *,uint32_t,void *,uint32_t,uint32_t *) __attribute__((weak));
static int user_lease(unsigned op)
{
    if (!desk_w32_exchange) return -ENOSYS;
    struct { struct w32r_request h; char empty[2]; } in={
        .h={.version=W32R_VERSION,.op=op,.path_bytes=1,.name_bytes=1}};
    struct w32r_reply out; uint32_t used=0;
    uint32_t e=desk_w32_exchange(&in,sizeof(in.h)+2,&out,sizeof(out),&used);
    if (e) return -EIO;
    if (used!=sizeof(out) || out.version!=W32R_VERSION) return -EIO;
    return out.error==170 ? -EBUSY : out.error ? -EIO : 0;
}
int users_boot_identity(char out[64])
{
#ifdef USERS_NATIVE_LOCK
    struct ciuki_recovery_status r;
    if (ciuki_recovery_status(&r) || !(r.flags&CIUKI_RECOVERY_VALID) || !r.boots) return -EIO;
    snprintf(out,64,"boot-%u",r.boots);
#else
    FILE *f=fopen("/proc/sys/kernel/random/boot_id","r");
    if (!f) return -errno;
    if (!fgets(out,64,f)) { fclose(f); return -EIO; }
    fclose(f); out[strcspn(out,"\r\n")]=0;
#endif
    return 0;
}
static int owner_alive(unsigned pid)
{
#ifdef USERS_NATIVE_LOCK
    struct { struct ciuki_proc_system system; struct ciuki_proc_record records[CIUKI_PROCESS_MAX]; } *s=calloc(1,sizeof(*s));
    if (!s) return -ENOMEM;
    int n=ciuki_proc_list(s,sizeof(*s)),live=-EIO;
    if (n>=0 && (unsigned)n==s->system.records && (unsigned)n<=CIUKI_PROCESS_MAX) {
        live=0;
        for (int i=0;i<n;i++) if (s->records[i].pid==pid)
            live=s->records[i].state==CIUKI_PROC_LIVE || s->records[i].state==CIUKI_PROC_STOPPING;
    }
    free(s); return live;
#else
    if (!pid || pid>0x7fffffffu) return 0;
    return !kill((pid_t)pid,0) || errno!=ESRCH;
#endif
}
/* Publish the complete owner as a nonempty directory: native FAT has no
 * hard links, but rename refuses replacing a nonempty directory. A unique
 * child name also fences a stale remover against a replacement owner. */
static int recover_lock(const char *lock,const char *boot)
{
    char saved_boot[64],magic[16],entry[96]={0},path[640]; unsigned pid=0;
    DIR *dir=opendir(lock);
    if (dir) {
        struct dirent *d; int e=0;
        errno=0;
        while ((d=readdir(dir))) {
            if (!strcmp(d->d_name,".") || !strcmp(d->d_name,"..")) continue;
            if (entry[0] || strncmp(d->d_name,"owner-",6) || strlen(d->d_name)>=sizeof(entry)) { e=-EBUSY; break; }
            strcpy(entry,d->d_name);
        }
        if (!e && errno) e=-errno;
        closedir(dir);
        if (e) return e;
        /* A published directory always starts nonempty. Empty means an
         * owner/remover already released it; rmdir cannot delete a new owner. */
        if (!entry[0]) return !rmdir(lock) ? 0 : errno==ENOENT || errno==ENOTEMPTY ? -EBUSY : -errno;
        snprintf(path,sizeof(path),"%s/%s",lock,entry);
    } else {
        if (errno==ENOENT) return 0;
        if (errno!=ENOTDIR) return -errno;
        snprintf(path,sizeof(path),"%s",lock); /* Complete CUSR1 migration. */
    }
    FILE *f=fopen(path,"rb");
    if (!f) return errno==ENOENT ? -EBUSY : -errno;
    int valid=fscanf(f,"%15s %u %63s",magic,&pid,saved_boot)==3 && !strcmp(magic,"CUSR1");
    int bad=ferror(f); fclose(f);
    if (bad) return -EIO;
    if (!valid) return -EBUSY;
    if (!strcmp(boot,saved_boot)) {
        int live=owner_alive(pid); if (live) return live<0 ? live : -EBUSY;
    }
    if (unlink(path)) return errno==ENOENT ? -EBUSY : -errno;
    if (entry[0] && rmdir(lock)) return errno==ENOENT || errno==ENOTEMPTY ? -EBUSY : -errno;
    return 0;
}
static __attribute__((noinline)) int lock_users(const char *root,const char *lock,int *guard,char owner[640])
{
    char boot[64];
    int e=users_boot_identity(boot); if (e) return e;
#ifdef USERS_NATIVE_LOCK
    e=user_lease(W32R_PROFILE_ACQUIRE); if (e) return e;
    *guard=-2;
#else
    (void)user_lease;
    char path[512]; snprintf(path,sizeof(path),"%s/system/settings/users.guard",root);
    *guard=open(path,O_RDWR|O_CREAT|O_CLOEXEC,0600);
    if (*guard<0) return -errno;
    if (flock(*guard,LOCK_EX|LOCK_NB)) { e=errno==EWOULDBLOCK ? -EBUSY : -errno; close(*guard); *guard=-1; return e; }
#endif
    (void)root;
    e=recover_lock(lock,boot); if (e) return e;
    unsigned char nonce[16];
#ifdef __i386__
    if (getentropy(nonce,sizeof(nonce))) return -errno;
#else
    if (getrandom(nonce,sizeof(nonce),0)!=(ssize_t)sizeof(nonce)) return -EIO;
#endif
    char token[33],claim[576],record_path[672];
    for (unsigned i=0;i<sizeof(nonce);i++) snprintf(token+i*2,3,"%02x",nonce[i]);
    snprintf(claim,sizeof(claim),"%s.claim-%s",lock,token);
    if (mkdir(claim,0700)) return -errno;
    snprintf(record_path,sizeof(record_path),"%s/owner-%s",claim,token);
    int fd=open(record_path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if (fd<0) { e=-errno; (void)rmdir(claim); return e; }
    char record[112]; int n=snprintf(record,sizeof(record),"CUSR1 %u %s\n",(unsigned)getpid(),boot);
    e=write(fd,record,(size_t)n)==n && !fsync(fd) ? 0 : -EIO;
    if (close(fd) && !e) e=-EIO;
    if (!e && rename(claim,lock)) e=errno==EEXIST || errno==ENOTEMPTY || errno==ENOTDIR ? -EBUSY : -errno;
    if (e) { (void)unlink(record_path); (void)rmdir(claim); return e; }
    snprintf(owner,640,"%s/owner-%s",lock,token);
    return 0;
}
static void unlock_guard(int guard)
{
#ifdef USERS_NATIVE_LOCK
    if (guard==-2) (void)user_lease(W32R_PROFILE_RELEASE);
#else
    if (guard>=0) close(guard);
#endif
}
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
    const char *const paths[]={p.desktop,p.programs,"Favorites","Documents/Music","Documents/Pictures",
        "Application Data","Recent","SendTo","Templates","Start Menu/Programs/StartUp"};
    for (unsigned i=0;i<sizeof(paths)/sizeof(*paths);i++) {
        if (i<2) snprintf(path,sizeof(path),"%s%s",root,paths[i]);
        else snprintf(path,sizeof(path),"%s%s/%s",root,p.root,paths[i]);
        int e=dirs(path); if (e) return e;
    }
    return copy_default(root,p.settings);
}
int users_execute(const char *root,const struct users_request *q,struct users_view *v)
{
    memset(v,0,sizeof(*v));
    char path[512], temp[512], lock[512], owner[640];
    if (!root || strlen(root)>300) return -EINVAL;
    snprintf(path,sizeof(path),"%s/system/settings/users.dat",root);
    snprintf(temp,sizeof(temp),"%s/system/settings/users.new",root);
    snprintf(lock,sizeof(lock),"%s/system/settings/users.lock",root);
    int mutation=q->operation!=USERS_READ && q->operation!=USERS_AUTH, locked=0, guard=-1, e=0;
    if (mutation) {
        char parent[512]; snprintf(parent,sizeof(parent),"%s/system/settings",root);
        if ((e=dirs(parent))) return e;
        e=lock_users(root,lock,&guard,owner);
        if (e) { unlock_guard(guard); return e; }
        locked=1;
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
    if (locked) { (void)unlink(owner); (void)rmdir(lock); }
    unlock_guard(guard);
    if (e) memset(v,0,sizeof(*v));
    return e;
}
