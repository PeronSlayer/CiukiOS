/* SPDX-License-Identifier: MIT
 * Paths inherited by native children. No global environment mutation. */
#ifndef CIUKI_NATIVE_PROFILE_H
#define CIUKI_NATIVE_PROFILE_H
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
static inline const char *profile_home(void)
{
    const char *active=getenv("CIUKI_PROFILE");
    const char *s=active && *active ? getenv("HOME") : NULL;
    return s && s[0]=='/' && strlen(s)<96 ? s : "/Documents";
}
static inline const char *profile_settings(void)
{
    const char *s=getenv("CIUKI_SETTINGS");
    return s && s[0]=='/' && strlen(s)<128 ? s : "/system/settings/desktop.cfg";
}
static inline void profile_child(char *out,size_t cap,const char *child)
{ snprintf(out,cap,"%s/%s",profile_home(),child); }
/* sec-user-05: the session's committed profile root and private temporary
 * directory as the kernel launch generated them (USERPROFILE, TMPDIR below
 * /Windows/Profiles). Locations only, never an authority: the kernel checks
 * every access. A profile-less process gets NULL and the machine /tmp. */
static inline const char *profile_root_path(void)
{
    const char *s=getenv("USERPROFILE");
    return s && !strncmp(s,"/Windows/Profiles/",18) && s[18] && strlen(s)<96 && !strstr(s,"/..") ? s : NULL;
}
static inline const char *profile_temp(void)
{
    const char *root=profile_root_path(),*s=getenv("TMPDIR");
    if(!root)return "/tmp";
    size_t n=strlen(root);
    if(s && !strncmp(s,root,n) && s[n]=='/' && strlen(s)<160 && !strstr(s,"/.."))return s;
    static char derived[128];
    snprintf(derived,sizeof(derived),"%s/Local Settings/Temp",root);
    return derived;
}
/* fx-hlis5: <profile>/Local Settings/Temp exists for the whole session, as
 * on Windows 2000/XP, where every profile is a copy of Default User and
 * Local Settings\Temp holds the user's temporary files (Microsoft, "User
 * Profile Structure", Windows Server 2003 Technical Reference). Windows
 * programs take it for granted: GetTempPath "does not verify that the path
 * exists" (Microsoft Learn, GetTempPathA), and InstallShield 5's bootstrap
 * writes its engine there directly. No account service skeleton creates it
 * (apps/accounts owner_kernel.c), so a fresh profile had none and the
 * Half-Life setup stopped at "unable to create a temporary file" (QEMU,
 * image c02b6731). Missing components of TEMP below the committed root are
 * created here, owner-only, when the session's desktop loads the profile;
 * anything outside the root, a ".." or a file in the way is refused. */
static inline int profile_temp_ready_at(const char *root,const char *temp)
{
    size_t r=root ? strlen(root) : 0,n=temp ? strlen(temp) : 0;
    if(!r || n<=r+1 || n>=200 || strncmp(temp,root,r) || temp[r]!='/' || strstr(temp,"/..")) { errno=EINVAL; return -1; }
    char path[200]; memcpy(path,temp,n+1);
    for(size_t i=r+1;i<=n;i++) {
        if(i<n && path[i]!='/') continue;
        if(path[i-1]=='/') continue;
        char c=path[i]; path[i]=0; struct stat st; int failed=0;
        if(!stat(path,&st)) { if(!S_ISDIR(st.st_mode)) { errno=ENOTDIR; failed=1; } }
        else if(errno!=ENOENT) failed=1;
        else if(mkdir(path,0700)) {
            if(errno!=EEXIST || stat(path,&st)) failed=1;
            else if(!S_ISDIR(st.st_mode)) { errno=ENOTDIR; failed=1; }
        }
        path[i]=c; if(failed) return -1;
    }
    return 0;
}
/* sec-sysvc2: the one per-user log folder below Local Settings\Application
 * Data, the kernel log service's USER scope (src/kernel/fs/sysvc.h
 * SYSVC_USER_LOGS): NETSURF.LOG and the crash reports CRASHn.TXT (native)
 * and WCRASHn.TXT (Win32). Programs reach it through the service; this name
 * is for the fallback when the service is absent (ENOSYS, ENODEV). */
#define PROFILE_LOGS "CiukiOS/Logs"
/* <profile>/Local Settings/Application Data/<leaf> (or Application Data
 * when roaming), missing folders created inside the profile; NULL without a
 * profile session or when the folder cannot be made. Per-user logs, crash
 * reports and application state go here, never into machine log folders. */
static inline const char *profile_data(char *out,size_t cap,const char *leaf,int roaming)
{
    const char *root=profile_root_path();
    int n=root ? snprintf(out,cap,"%s/%s/%s",root,roaming ? "Application Data" : "Local Settings/Application Data",leaf) : -1;
    if(n<0 || (size_t)n>=cap)return NULL;
    for(char *p=out+1;;p++){
        if(*p && *p!='/')continue;
        char c=*p;*p=0;struct stat st;
        int ok=!stat(out,&st) ? S_ISDIR(st.st_mode) : errno==ENOENT && (!mkdir(out,0700) || errno==EEXIST);
        *p=c;if(!ok)return NULL;if(!c)return out;
    }
}
/* sec-user-05: <profile>/<leaf> of a kernel profile session (the user's
 * Start Menu/Programs, Documents/Desktop, StartUp); 0 without a profile or
 * when it does not fit. */
static inline int profile_folder(char *out,size_t cap,const char *leaf)
{
    const char *root=profile_root_path(); char path[192];
    int n=root ? snprintf(path,sizeof(path),"%s/%s",root,leaf) : -1;
    if(n<0 || (size_t)n>=sizeof(path) || (size_t)n>=cap)return 0;
    memcpy(out,path,(size_t)n+1);return 1;
}
/* Appends HOME and TMPDIR and, in a profile session, USERPROFILE, TEMP and
 * TMP (at most five entries) at env[at]; returns the new count. The caller
 * terminates the array. Win32 startup DOS-spells the profile values. */
struct profile_env { char home[112], tmpdir[176], root[112], temp[176], tmp[176]; };
static inline unsigned profile_env_add(struct profile_env *s,char **env,unsigned at)
{
    const char *root=profile_root_path(),*temp=profile_temp();
    snprintf(s->home,sizeof(s->home),"HOME=%s",profile_home());
    snprintf(s->tmpdir,sizeof(s->tmpdir),"TMPDIR=%s",temp);
    env[at++]=s->home;env[at++]=s->tmpdir;
    if(root){
        snprintf(s->root,sizeof(s->root),"USERPROFILE=%s",root);
        snprintf(s->temp,sizeof(s->temp),"TEMP=%s",temp);
        snprintf(s->tmp,sizeof(s->tmp),"TMP=%s",temp);
        env[at++]=s->root;env[at++]=s->temp;env[at++]=s->tmp;
    }
    return at;
}
#endif
