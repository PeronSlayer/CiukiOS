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
