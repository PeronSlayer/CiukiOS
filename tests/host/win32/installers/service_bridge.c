/* H1 fixture service: production w32srv directory/registry/restart, native ABI.
 * The socket adapter copies buffers; no PE pointer enters this process.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../../../../apps/w32srv/shell.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef H1_FONTS
#include "../../../../apps/w32srv/fonts.h"
#endif
struct fixture_service {
    struct wr_server registry;
    struct ws_shell *shell;
    struct ws_fs native, mapped;
    char root[1024], hives[1100];
    unsigned fault;
#ifdef H1_FONTS
    struct wf_service *fonts;
    char font_system[256], font_install[256];
#endif
};
#ifdef H1_FONTS
/* fx-hlrun: the production w32srv font service (fonts.c over libfont and
 * FreeType) behind gdi32's PE-to-native bridge, so H1 dialogs and windows
 * render their text. gdi32 names faces by their guest store paths:
 * /System/fonts/X.ttf (the pinned system faces, here the repository's
 * Liberation directory given by CIUKI_H1_FONTS) and /WINDOWS/FONTS/X.ttf
 * (installed faces, here the fixture C: tree). */
static uint32_t fixture_font_call(void *opaque,uint32_t pid,const struct w32r_request *a,const char *path,
                                  const void *data,struct w32r_reply *r,uint8_t *out,size_t cap){
    struct fixture_service *s=opaque;char mapped_path[600]="";
    const char *base=NULL,*dir=NULL;
    if(!strncasecmp(path,"/System/fonts/",14)){base=path+14;dir=s->font_system;}
    else if(!strncasecmp(path,"/WINDOWS/FONTS/",15)){base=path+15;dir=s->font_install;}
    if(*path && !base)return 2;
    if(base && snprintf(mapped_path,sizeof(mapped_path),"%s/%s",dir,base)>=(int)sizeof(mapped_path))return 206;
    return wf_call(s->fonts,pid,a,mapped_path,data,r,out,cap);
}
#endif
/* The fixture disk is case-insensitive and case-preserving for PE I/O
 * (host_services.Session.path), as FAT is for the production replay and
 * probe. Resolve every existing component of the host path the same way:
 * an NSIS engine creates C:\WINDOWS\wininit.ini and the restart service
 * opens C:\WINDOWS\WININIT.INI. A missing component keeps its spelling. */
static void fold_case(char *path,size_t from){
    size_t i=from+1;  /* path[from] is the '/' after the drive letter */
    for(;;){
        size_t start=i;
        while(path[i] && path[i]!='/')i++;
        char saved=path[i];path[i]=0;
        if(i>start && access(path,F_OK)){
            path[start-1]=0;
            DIR *d=opendir(path);
            path[start-1]='/';
            if(d){
                struct dirent *e;
                while((e=readdir(d)))
                    if(strlen(e->d_name)==i-start && !strcasecmp(e->d_name,path+start)){memcpy(path+start,e->d_name,i-start);break;}
                closedir(d);
            }
        }
        path[i]=saved;
        if(!saved)return;
        i++;
    }
}
static int mapped(struct fixture_service *s,const char *path,char out[2048]) {
    if(!path || path[0]!='/' || strstr(path,"/../") || strstr(path,"/./"))return EINVAL;
    int n=snprintf(out,2048,"%s/C%s",s->root,path);
    if(n<0 || n>=2048)return ENAMETOOLONG;
    fold_case(out,strlen(s->root)+2);
    return 0;
}
static int fs_stat(void *o,const char *p,int *d,uint64_t *n){struct fixture_service *s=o;char q[2048];int e=mapped(s,p,q);return e?e:s->native.stat(0,q,d,n);}
#define ONE(name) static int fs_##name(void *o,const char *p){struct fixture_service *s=o;char q[2048];int e=mapped(s,p,q);return e?e:s->native.name(0,q);}
ONE(mkdir) ONE(rmdir) ONE(unlink)
static int fs_rename(void *o,const char *p,const char *r){struct fixture_service *s=o;char q[2048],t[2048];
    if(s->fault==2 && strstr(r,"CIUKIRST.LOG")){s->fault=0;return EIO;}
    if(s->fault==3 && strstr(r,"WININIT.BAK")){s->fault=0;return EIO;}
    int e=mapped(s,p,q);if(!e)e=mapped(s,r,t);return e?e:s->native.rename(0,q,t);}
static int fs_read(void *o,const char *p,uint8_t **b,size_t *n,size_t cap){struct fixture_service *s=o;char q[2048];int e=mapped(s,p,q);return e?e:s->native.read(0,q,b,n,cap);}
static int fs_write(void *o,const char *p,const void *b,size_t n){struct fixture_service *s=o;char q[2048];
    if(s->fault==1 && strstr(p,"CIUKIRST.TMP")){s->fault=0;return EIO;}
    int e=mapped(s,p,q);return e?e:s->native.write(0,q,b,n);}
static int fs_list(void *o,const char *p,int (*cb)(void *,const char *,int),void *ctx){struct fixture_service *s=o;char q[2048];int e=mapped(s,p,q);return e?e:s->native.list(0,q,cb,ctx);}
static void fs_log(void *o,const char *p){(void)o;fprintf(stderr,"%s\n",p);}
void *installer_service_create(const char *root){
    struct fixture_service *s=calloc(1,sizeof(*s));if(!s)return 0;
    if(strlen(root)>=sizeof(s->root)){free(s);return 0;}strcpy(s->root,root);
    snprintf(s->hives,sizeof(s->hives),"%s/hives",root);
    struct wr_io io;wr_posix_io(&io,s->hives);
    if(!wr_init(&s->registry,&io,0)){free(s);return 0;}
    ws_posix_fs(&s->native);
    s->mapped=(struct ws_fs){s,fs_stat,fs_mkdir,fs_rmdir,fs_unlink,fs_rename,fs_read,fs_write,fs_list,fs_log};
    struct ws_config config={"/WINDOWS/Start Menu/Programs","/WINDOWS"};
    s->shell=ws_create(&s->mapped,&config,&s->registry);
    if(!s->shell){wr_destroy(&s->registry);free(s);return 0;}
    s->registry.shell=s->shell;s->registry.shell_call=ws_call;s->registry.shell_exit=ws_exit;
#ifdef H1_FONTS
    const char *system=getenv("CIUKI_H1_FONTS");
    if(system && strlen(system)<sizeof(s->font_system) &&
       snprintf(s->font_install,sizeof(s->font_install),"%s/C/WINDOWS/FONTS",root)<(int)sizeof(s->font_install)){
        strcpy(s->font_system,system);
        struct wf_config config={.system_dir=s->font_system,.install_dir=s->font_install};
        s->fonts=wf_create(&config);
        if(s->fonts){s->registry.font_call=fixture_font_call;s->registry.fonts=s;}
    }
#endif
    return s;
}
uint32_t installer_service_exchange(void *ctx,uint32_t pid,const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used){
    struct fixture_service *s=ctx;size_t bytes=0;
    uint32_t e=wr_dispatch(&s->registry,pid,in,n,out,cap,&bytes);*used=(uint32_t)bytes;return e;
}
uint32_t installer_service_exit(void *ctx,uint32_t pid){return wr_exit(&((struct fixture_service *)ctx)->registry,pid);}
uint32_t installer_service_destroy(void *ctx){struct fixture_service *s=ctx;uint32_t e=wr_flush(&s->registry);ws_destroy(s->shell);wr_destroy(&s->registry);
#ifdef H1_FONTS
    wf_destroy(s->fonts);
#endif
    free(s);return e;}
uint32_t installer_service_restart_begin(void *ctx){return ws_restart_begin(((struct fixture_service *)ctx)->shell,2);}
uint32_t installer_service_restart_phase(void *ctx){return ws_restart_phase(((struct fixture_service *)ctx)->shell);}
int installer_service_restart_tick(void *ctx){return ws_restart_tick(((struct fixture_service *)ctx)->shell);}
uint32_t installer_service_restart_next(void *ctx,char *command,uint32_t cap){uint32_t type,handed;return ws_restart_next(((struct fixture_service *)ctx)->shell,command,cap,&type,&handed);}
uint32_t installer_service_restart_end(void *ctx){return ws_restart_end(((struct fixture_service *)ctx)->shell);}
void installer_service_fault(void *ctx,unsigned fault){((struct fixture_service *)ctx)->fault=fault;}
/* Complete logical store, independent of the selected Uninstall observer.
 * Counted records avoid escaping ambiguities; the Python side sorts them. */
unsigned installer_service_snapshot(void *ctx,uint8_t *out,unsigned cap){
    struct wr_server *s=&((struct fixture_service *)ctx)->registry;unsigned used=0;
    for(struct wr_key *k=s->keys;k;k=k->next){
        if(k->deleted)continue;
        char path[4096]="";unsigned n=0;
        for(struct wr_key *p=k;p && p->parent;p=p->parent){
            unsigned len=(unsigned)strlen(p->name);if(len+n+1>=sizeof(path))return 0;
            memmove(path+len+1,path,n+1);memcpy(path,p->name,len);path[len]='\\';n+=len+1;
        }
        if(n)path[--n]=0;
        struct wr_value *v=NULL;
        do{
            uint32_t head[]={v?1u:0u,k->hive,n,v?(uint32_t)strlen(v->name):0u,v?v->type:0u,v?v->bytes:0u};
            unsigned bytes=sizeof(head)+head[2]+head[3]+head[5];if(bytes>cap-used)return 0;
            memcpy(out+used,head,sizeof(head));used+=sizeof(head);
            memcpy(out+used,path,n);used+=n;
            if(v){memcpy(out+used,v->name,head[3]);used+=head[3];memcpy(out+used,v->data,v->bytes);used+=v->bytes;v=v->next;}
            else v=k->values;
        }while(v);
    }
    return used;
}
/* Same independent, read-only observer as the authenticated desktop. */
static struct fixture_service *observed;
uint32_t desk_w32_exchange(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used){return installer_service_exchange(observed,0x7ffffffe,in,n,out,cap,used);}
extern unsigned desk_installer_registry(unsigned);
unsigned installer_service_observe(void *ctx,unsigned stage){observed=ctx;unsigned e=desk_installer_registry(stage);observed=0;return e;}

/* Compile the very same VFS observation/predicate path as the guest probe.
 * Only the VFS transport below is replaced, not file/content expectations. */
#include <stdbool.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#undef _SC_PAGESIZE
#undef _SC_OPEN_MAX
#undef _SC_ARG_MAX
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include <unistd.h>
#include "../../../../src/probes/win32_installers/controller.h"
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define FS_ENOENT ENOENT
#define FS_ENOTDIR ENOTDIR
#define FS_EFBIG EFBIG
#define VFS_READ 1
#define VFS_DENY_NONE 0
struct vfs_table {struct fixture_service *service;};
struct fat_entry {unsigned unused;};
static int native_path(struct vfs_table *t,const char *path,char out[2048]){
    if(strncmp(path,"C:/",3))return EACCES;
    return mapped(t->service,path+2,out);
}
static int vfs_stat(struct vfs_table *t,const char *p,struct fat_entry *entry){
    (void)entry;char path[2048];int e=native_path(t,p,path);struct stat st;
    if(e)return -e;return stat(path,&st)?-errno:0;
}
static int vfs_open(struct vfs_table *t,const char *p,unsigned access,unsigned share,unsigned mode){
    (void)access;(void)share;(void)mode;char path[2048];int e=native_path(t,p,path);
    if(e)return -e;int fd=open(path,O_RDONLY);return fd<0?-errno:fd;
}
static int vfs_read(struct vfs_table *t,int fd,void *p,unsigned cap,size_t *n){
    (void)t;ssize_t r=read(fd,p,cap);if(r<0)return -errno;*n=(size_t)r;return 0;
}
static int vfs_close(struct vfs_table *t,int fd){(void)t;return close(fd)?-errno:0;}
static char *probe_output;static size_t probe_cap,probe_used;
static void rec_emit(const char *probe,const char *event,const char *format,...){
    if(probe_used>=probe_cap)return;
    int n=snprintf(probe_output+probe_used,probe_cap-probe_used,"RECORD %s %s ",probe,event);
    if(n<0 || (size_t)n>=probe_cap-probe_used){probe_used=probe_cap;return;}probe_used+=(size_t)n;
    va_list ap;va_start(ap,format);n=vsnprintf(probe_output+probe_used,probe_cap-probe_used,format,ap);va_end(ap);
    if(n<0 || (size_t)n+1>=probe_cap-probe_used){probe_used=probe_cap;return;}probe_used+=(size_t)n;
    probe_output[probe_used++]='\n';probe_output[probe_used]=0;
}
#define CIUKI_INSTALLER_OBSERVER_ONLY
#include "../../../../src/kernel/probes/installer_probe.inc"
void installer_service_probe_reset(void){memset(&installer_result,0,sizeof(installer_result));}
int installer_service_probe_checkpoint(void *ctx,unsigned stage,unsigned code,char *out,unsigned cap){
    installer_files.service=ctx;probe_output=out;probe_cap=cap;probe_used=0;if(cap)out[0]=0;
    unsigned errors=installer_service_observe(ctx,stage);
    return installer_report_stage(stage,code,errors) && probe_used<probe_cap;
}
unsigned installer_service_probe_publish(char *out,unsigned cap){
    probe_output=out;probe_cap=cap;probe_used=0;if(cap)out[0]=0;
    installer_publish(&installer_result,rec_emit);return probe_used<probe_cap?(unsigned)probe_used:0;
}
