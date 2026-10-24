/* G21 startup snapshot and persistent selection. SPDX-License-Identifier: MIT */
#include "startup.h"
#include "win32_adapter.h"
#include <ciuki/registry.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
const char *desk_startup_source(unsigned source)
{
    static const char *const names[]={"HKLM RunServicesOnce", "HKLM RunServices", "HKLM RunOnce",
        "HKCU RunOnce", "HKLM Run", "HKCU Run", "StartUp folder"};
    return source<7 ? names[source] : "Unknown";
}
/* Hash only selects a key: the complete identity is verified before use. */
static void policy_key(const struct desk_startup_entry *e, char key[64])
{
    uint64_t hash=14695981039346656037ull;
    for (const unsigned char *p=(const unsigned char *)e->name;*p;p++) { hash^=*p; hash*=1099511628211ull; }
    snprintf(key,64,"Desktop\\Startup\\%u-%016llx",e->source,(unsigned long long)hash);
}
static int selection(struct reg_root *root, struct desk_startup_entry *e)
{
    char key[64], name[1021]; policy_key(e,key); size_t n=sizeof(name);
    e->enabled=1;
    if (reg_get_string(root,key,"Identity",name,&n)) return errno==ENOENT ? 0 : -errno;
    if (strcmp(name,e->name)) return -EEXIST;
    return reg_get_bool(root,key,"Enabled",&e->enabled) ? -errno : 0;
}
int desk_startup_enable_at(const char *registry, const struct desk_startup_entry *e, int enabled)
{
    if (!e || e->source>6 || !memchr(e->name,0,sizeof(e->name))) return -EINVAL;
    struct reg_root *root=reg_open_root_at("Desktop",registry); if (!root) return -errno;
    char key[64], identity[1021]; policy_key(e,key); size_t size=sizeof(identity);
    int result=0;
    if (!reg_get_string(root,key,"Identity",identity,&size)) {
        if (strcmp(identity,e->name)) result=-EEXIST;
    } else if (errno!=ENOENT) result=-errno;
    if (!result && (reg_set_string(root,key,"Identity",e->name) || reg_set_bool(root,key,"Enabled",!!enabled))) result=-errno;
    reg_close_root(root); return result;
}
static int folder_entries(const char *folder, struct desk_startup_entry *out, unsigned used, unsigned cap)
{
    DIR *dir=opendir(folder); if (!dir) return errno==ENOENT ? (int)used : -errno;
    int result=0; struct dirent *de;
    for (;;) {
        errno=0; de=readdir(dir);
        if (!de) { if (errno) result=-errno; break; }
        if (de->d_name[0]=='.') continue;
        char path[260]; int n=snprintf(path,sizeof(path),"%s/%s",folder,de->d_name);
        if (n<0 || (unsigned)n>=sizeof(path)) { result=-ENAMETOOLONG; break; }
        struct stat st; if (stat(path,&st)) { result=-errno; break; }
        if (!S_ISREG(st.st_mode)) continue;
        size_t len=strlen(path); int link=len>=4 && !strcasecmp(path+len-4,".lnk");
        if (!link && desk_w32_identify(path)<0) continue;
        if (used==cap) { result=-EOVERFLOW; break; }
        struct desk_startup_entry *e=&out[used]; memset(e,0,sizeof(*e));
        e->source=6; snprintf(e->name,sizeof(e->name),"%s",de->d_name);
        snprintf(e->command,sizeof(e->command),"%s",path);
        if (link) {
            uint8_t *bytes=malloc(DESK_LINK_BYTES+1); if (!bytes) { result=-ENOMEM; break; }
            FILE *f=fopen(path,"rb"); if (!f) { free(bytes); result=-errno; break; }
            size_t count=fread(bytes,1,DESK_LINK_BYTES+1,f);
            int bad=ferror(f); if (fclose(f)) bad=1;
            if (bad || !desk_link_decode(bytes,(unsigned)count,&e->launch)) e->error=-EINVAL;
            free(bytes);
        } else {
            snprintf(e->launch.label,sizeof(e->launch.label),"Startup");
            strcpy(e->launch.target,path); e->launch.show=1;
        }
        used++;
    }
    if (closedir(dir) && !result) result=-errno;
    return result ? result : (int)used;
}
int desk_startup_list_at(const char *folder, const char *registry, struct desk_startup_entry *out, unsigned cap)
{
    if (!out || !cap || cap>DESK_STARTUP_MAX) return -EINVAL;
    int n=ciuki_win32_startup_list(out,cap); if (n<0) return n;
    if ((unsigned)n>cap) return -EOVERFLOW;
    n=folder_entries(folder,out,(unsigned)n,cap); if (n<0) return n;
    struct reg_root *root=reg_open_root_at("Desktop",registry); if (!root) return -errno;
    int error=0;
    for (int i=0;i<n && !error;i++) error=selection(root,&out[i]);
    reg_close_root(root); return error ? error : n;
}
int desk_startup_execute(struct desk_startup_entry *entries, unsigned count, int skip,
                         int (*consume)(const struct desk_startup_entry *),
                         int (*launch)(const struct desk_launch_record *))
{
    if (skip) return 0;
    if (!entries || count>DESK_STARTUP_MAX || !consume || !launch) return -EINVAL;
    int first=0;
    for (unsigned i=0;i<count;i++) {
        struct desk_startup_entry *e=&entries[i]; if (!e->enabled) continue;
        int error=e->error;
        if (!error && (e->source==0 || e->source==2 || e->source==3)) error=consume(e);
        if (!error) { int pid=launch(&e->launch); if (pid<0) error=pid; }
        if (error) { if (!first) first=error; fprintf(stderr,"startup: %s: %s: error %d\n",desk_startup_source(e->source),e->name,-error); }
    }
    return first;
}
static int startup_launch(const struct desk_launch_record *record)
{
    int result=-EBUSY;
    for (unsigned retry=0;retry<100;retry++) {
        result=ciuki_win32_shell_launch(record);
        if (result!=-EBUSY && result!=-EAGAIN) return result;
        struct timespec pause={.tv_sec=0,.tv_nsec=100000000}; nanosleep(&pause,NULL);
    }
    return result;
}
static void *startup_thread(void *arg)
{
    (void)arg;
    struct desk_startup_entry *entries=calloc(DESK_STARTUP_MAX,sizeof(*entries));
    if (!entries) { fprintf(stderr,"startup: out of memory\n"); return NULL; }
    int n=-EPIPE;
    for (unsigned retry=0;retry<50;retry++) {
        if (!desk_w32_ready()) { n=desk_startup_list_at(DESK_STARTUP_FOLDER,REG_DIRECTORY,entries,DESK_STARTUP_MAX); break; }
        struct timespec pause={.tv_sec=0,.tv_nsec=200000000}; nanosleep(&pause,NULL);
    }
    if (n>=0) (void)desk_startup_execute(entries,(unsigned)n,0,ciuki_win32_startup_consume,startup_launch);
    else fprintf(stderr,"startup: snapshot failed: %d\n",-n);
    free(entries); return NULL;
}
int desk_startup_begin(int skip)
{
    if (skip) return 0;
    pthread_attr_t attr; int error=pthread_attr_init(&attr); if (error) return -error;
    error=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    pthread_t thread; if (!error) error=pthread_create(&thread,&attr,startup_thread,NULL);
    pthread_attr_destroy(&attr); return -error;
}
