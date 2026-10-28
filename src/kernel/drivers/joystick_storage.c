/* Native adapter for the existing Control Panel INI directory. Thread only.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/joystick_settings.h>
static struct vfs *settings_vfs;
static struct vfs_table table;
static int open_table(void)
{
    if (!settings_vfs || !settings_vfs->volumes[2]) return -ENODEV;
    return px_error(vfs_table_init(settings_vfs,&table,2));
}
static int read_ini(void *context, const char *name, void *data, size_t capacity, size_t *done)
{
    (void)context;
    int e=open_table(); if (e) return e;
    char path[160]; ksnprintf(path,sizeof(path),"C:\\SYSTEM\\SETTINGS\\%s",name);
    int fd=vfs_open(&table,path,VFS_READ,VFS_DENY_WRITE,0);
    if (fd<0) { vfs_table_destroy(&table); return px_error(fd); }
    e=vfs_read(&table,fd,data,capacity,done);
    if (!e) {
        uint8_t extra; size_t n=0;
        e=vfs_read(&table,fd,&extra,1,&n); if (!e && n) e=-FS_EIO;
    }
    int closed=vfs_close(&table,fd); vfs_table_destroy(&table); return px_error(e ? e : closed);
}
static int write_ini(void *context, const char *name, const void *data, size_t size)
{
    (void)context;
    int e=open_table(); if (e) return e;
    e=vfs_mkdir(&table,"C:\\SYSTEM\\SETTINGS",0);
    if (e && e!=-FS_EEXIST) { vfs_table_destroy(&table); return px_error(e); }
    char path[160]; ksnprintf(path,sizeof(path),"C:\\SYSTEM\\SETTINGS\\%s",name);
    int fd=vfs_open(&table,path,VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_ALL,0);
    if (fd<0) { vfs_table_destroy(&table); return px_error(fd); }
    size_t n=0; e=vfs_write(&table,fd,data,size,&n);
    if (!e && n!=size) e=-FS_EIO;
    if (!e) e=vfs_commit(&table,fd);
    int closed=vfs_close(&table,fd); vfs_table_destroy(&table); return px_error(e ? e : closed);
}
/* --- sec-user-05 (user-security-contract.md K-Q8): the active interactive
 * session's own store, <profile>\Settings\Joystick, read and written as the
 * session desktop's retained subject through a subject-bound table (the
 * kernel path policy decides). A record (both double-buffer slots) that the
 * profile has never saved is read from the machine store, so a new user
 * starts from the machine defaults; writes never reach the machine store. */
#include <ciuki/security.h>
#pragma weak security_credential
#pragma weak security_session_profile
#pragma weak security_session_desktop
#pragma weak security_process_live
#pragma weak security_system
#pragma weak security_admin
#pragma weak vfs_table_init_subject
#pragma weak joystick_settings_scope
/* Two descriptors: the active scope's is never rewritten; the next scope
 * binds the other one while the worker's storage pass is excluded. */
struct personal_store {
    struct security_subject *subject;
    char dir[128];
};
static struct personal_store stores[2];
static unsigned bound;
/* One table (about 20 KiB): storage I/O is serialized by the settings
 * worker's lock (joystick_settings.c `busy`), like the machine table. */
static struct vfs_table personal_tables;
static int personal_path(const struct personal_store *ps, char *out, size_t size, const char *name)
{
    int n=ksnprintf(out,size,"%s\\%s",ps->dir,name);
    return n<0 || (size_t)n>=size ? -ENAMETOOLONG : 0;
}
static int personal_table(const struct personal_store *ps, struct vfs_table *t)
{
    if (!settings_vfs || !settings_vfs->volumes[2] || !vfs_table_init_subject || !ps->subject) return -ENODEV;
    return px_error(vfs_table_init_subject(settings_vfs,t,2,ps->subject));
}
static int personal_exists(const struct personal_store *ps, const char *name)
{
    char path[200]; struct vfs_table *t=&personal_tables;
    if (personal_path(ps,path,sizeof(path),name) || personal_table(ps,t)) return 0;
    int fd=vfs_open(t,path,VFS_READ,VFS_DENY_NONE,0);
    if (fd>=0) vfs_close(t,fd);
    vfs_table_destroy(t); return fd>=0;
}
static int read_personal(void *context, const char *name, void *data, size_t capacity, size_t *done)
{
    const struct personal_store *ps=context;
    char path[200]; struct vfs_table *t=&personal_tables;
    int e=personal_path(ps,path,sizeof(path),name); if (e) return e;
    if ((e=personal_table(ps,t))) return e;
    int fd=vfs_open(t,path,VFS_READ,VFS_DENY_WRITE,0);
    if (fd>=0) {
        e=vfs_read(t,fd,data,capacity,done);
        if (!e) { uint8_t extra; size_t n=0; e=vfs_read(t,fd,&extra,1,&n); if (!e && n) e=-FS_EIO; }
        int closed=vfs_close(t,fd); vfs_table_destroy(t); return px_error(e ? e : closed);
    }
    vfs_table_destroy(t);
    if (fd!=-FS_ENOENT) return px_error(fd);
    /* "<stem>.<slot>.ini": the profile owns the record once either slot exists. */
    size_t n=strlen(name);
    if (n>6 && !strncmp(name+n-4,".ini",5) && name[n-6]=='.' && (name[n-5]=='0' || name[n-5]=='1')) {
        char other[112]; if (n>=sizeof(other)) return -ENAMETOOLONG;
        memcpy(other,name,n+1); other[n-5]=name[n-5]=='0' ? '1' : '0';
        if (personal_exists(ps,other)) return -ENOENT;
    }
    return read_ini(0,name,data,capacity,done); /* machine default */
}
static int write_personal(void *context, const char *name, const void *data, size_t size)
{
    const struct personal_store *ps=context;
    char path[200], dir[128]; struct vfs_table *t=&personal_tables;
    int e=personal_path(ps,path,sizeof(path),name); if (e) return e;
    if ((e=personal_table(ps,t))) return e;
    /* Settings, then Settings\Joystick, below the committed profile root. */
    size_t settings=strlen(ps->dir)-(sizeof("\\Joystick")-1);
    memcpy(dir,ps->dir,settings); dir[settings]=0;
    e=vfs_mkdir(t,dir,0);
    if (!e || e==-FS_EEXIST) e=vfs_mkdir(t,ps->dir,0);
    if (e && e!=-FS_EEXIST) { vfs_table_destroy(t); return px_error(e); }
    int fd=vfs_open(t,path,VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_ALL,0);
    if (fd<0) { vfs_table_destroy(t); return px_error(fd); }
    size_t n=0; e=vfs_write(t,fd,data,size,&n);
    if (!e && n!=size) e=-FS_EIO;
    if (!e) e=vfs_commit(t,fd);
    int closed=vfs_close(t,fd); vfs_table_destroy(t); return px_error(e ? e : closed);
}
struct personal_request {
    struct security_subject *subject;
    const struct ciuki_security_profile *profile;
};
/* Native committed root "/Windows/Profiles/<x>" ->
 * "C:\Windows\Profiles\<x>\Settings\Joystick" (never derived from the uid). */
static int personal_bind(void *context, struct joystick_settings_io *io)
{
    const struct personal_request *r=context;
    const struct ciuki_security_profile *profile=r->profile;
    static const char prefix[]="/Windows/Profiles/";
    if (!profile->path_bytes || profile->path_bytes>sizeof(profile->path) || profile->path[profile->path_bytes-1] ||
        strncmp(profile->path,prefix,sizeof(prefix)-1) || !profile->path[sizeof(prefix)-1]) return -EACCES;
    for (unsigned i=0;i+1<profile->path_bytes;i++) {
        char c=profile->path[i];
        if (c<33 || c>126 || c=='\\' || c==':' || (c=='/' && profile->path[i+1]=='/') ||
            (c=='.' && profile->path[i+1]=='.')) return -EACCES;
    }
    struct personal_store *ps=&stores[(bound+1)&1];
    int n=ksnprintf(ps->dir,sizeof(ps->dir),"C:%s\\Settings\\Joystick",profile->path);
    if (n<0 || (size_t)n>=sizeof(ps->dir)) return -ENAMETOOLONG;
    for (char *c=ps->dir+2;*c;c++) if (*c=='/') *c='\\';
    ps->subject=r->subject; bound=(bound+1)&1;
    *io=(struct joystick_settings_io){read_personal,write_personal,ps};
    return 0;
}
int joystick_settings_select(struct process *p)
{
    if (!security_process_live || !security_credential || !security_session_profile || !security_session_desktop ||
        !security_system || !security_admin || !joystick_settings_scope) return -EOPNOTSUPP;
    if (!security_process_live(p)) return -EACCES;
    if (security_system(p)) return joystick_settings_scope(0,0,0,0);
    const struct ciuki_credential *c=security_credential(p);
    if (!c || !c->session_id || !(c->flags & CIUKI_CRED_INTERACTIVE)) return -EACCES;
    struct ciuki_security_profile profile;
    int e=security_session_profile(c->session_id,&profile);
    if (e) return e==-ESRCH ? -EACCES : e;
    /* The interim bootstrap desktop has no committed profile: an
     * administrator keeps the machine store, anyone else is refused. */
    if (!profile.path_bytes) return security_admin(p) ? joystick_settings_scope(0,0,0,0) : -EACCES;
    if (profile.uid!=c->uid) return -EACCES;
    struct process *desktop=security_session_desktop(c->session_id);
    if (!desktop || !desktop->subject) return -EAGAIN;
    struct personal_request request={desktop->subject,&profile};
    return joystick_settings_scope(c->session_id,desktop->subject,personal_bind,&request);
}
/* --- end sec-user-05 --- */
void joystick_storage_init(struct vfs *vfs)
{ settings_vfs=vfs; }
void joystick_storage_start(void)
{
    const struct joystick_settings_io io={read_ini,write_ini,0};
    joystick_settings_start(&io);
}
