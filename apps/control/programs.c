/* Installed-program inventory and bounded removal. SPDX-License-Identifier: MIT */
#include "programs.h"
#include "../desktop/catalog.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
/* The native FAT namespace has no symbolic links or lstat ABI. Host fixtures
 * use lstat so a symlink can never widen a removal beyond its fixture tree. */
#ifdef __i386__
#define program_stat stat
#else
#define program_stat lstat
#endif
#define WALK_DEPTH 16u
#define WALK_ENTRIES 16384u
static const char *const packages[]={"wolf3d-shareware","doom-shareware","quake-shareware"};
static int path(struct cp_program_store *s,const char *relative,char *out,size_t cap)
{
    int n=snprintf(out,cap,"%s%s",s->root ? s->root : "",relative);
    return n<0 || (size_t)n>=cap ? -ENAMETOOLONG : 0;
}
/* Canonical absolute native/C: paths only. Reject traversal, alternate drives
 * and ambiguous separators before comparing path components. */
static int normal(const char *in,char out[260])
{
    if(!in)return 0;
    if((in[0]=='C' || in[0]=='c') && in[1]==':')in+=2;
    if(*in!='/' && *in!='\\')return 0;
    unsigned n=0;
    do {
        unsigned c=(unsigned char)*in++;
        if(c=='\\')c='/';
        if(c && (c<32 || c==127 || c==':'))return 0;
        if(n>=259)return 0;
        out[n++]=(char)c;
    } while(out[n-1]);
    if(n>2 && out[n-2]=='/')out[--n-1]=0;
    const char *p=out+1;
    while(*p){const char *end=strchr(p,'/');unsigned len=end ? (unsigned)(end-p) : (unsigned)strlen(p);
        if(!len || (len==1 && *p=='.') || (len==2 && !memcmp(p,"..",2)))return 0;
        if(!end)break;p=end+1;
    }
    return 1;
}
int cp_program_contains(const char *directory,const char *target)
{
    char a[260],b[260];if(!normal(directory,a) || !normal(target,b))return 0;
    size_t n=strlen(a);return !strncasecmp(a,b,n) && (!b[n] || b[n]=='/');
}
static int system_path(const char *location)
{
    static const char *const names[]={"desktop","shell-launch","demo","gfxdemo","dosbox","files","tasks","ciuknote","ciukide","control","terminal","regedit","hwreport","resmeter","schedtask","sysinfo","sysmon","logview","calc","charmap","clock","help","mixer","music","paint","viewer","wordpad","lua"};
    if(cp_program_contains("/System",location) || cp_program_contains("/Windows",location))return 1;
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++){
        char p[80];snprintf(p,sizeof(p),"/Programs/%s",names[i]);
        if(cp_program_contains(p,location))return 1;
    }
    return 0;
}
static int walk(const char *name,unsigned depth,unsigned *budget,int remove,uint64_t *size)
{
    if(!depth || !*budget)return -E2BIG;
    --*budget;
    struct stat st;if(program_stat(name,&st))return -errno;
    if(!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))return -EINVAL;
    *size+=(uint64_t)st.st_blocks*512u;
    if(S_ISDIR(st.st_mode)){
        DIR *d=opendir(name);if(!d)return -errno;
        int error=0;struct dirent *e;
        errno=0;
        while((e=readdir(d))){
            if(!strcmp(e->d_name,".") || !strcmp(e->d_name,".."))continue;
            if(strchr(e->d_name,'/') || strchr(e->d_name,'\\')){error=-EINVAL;break;}
            char child[1024];int n=snprintf(child,sizeof(child),"%s/%s",name,e->d_name);
            if(n<0 || n>=(int)sizeof(child)){error=-ENAMETOOLONG;break;}
            error=walk(child,depth-1,budget,remove,size);if(error)break;
            errno=0;
        }
        if(!error && errno)error=-errno;
        if(closedir(d) && !error)error=-errno;
        if(error)return error;
        if(remove && rmdir(name))return -errno;
    }else if(remove && unlink(name))return -errno;
    return 0;
}
static int read_record(const char *file,struct desk_launch_record *out)
{
    FILE *f=fopen(file,"rb");if(!f)return -errno;
    uint8_t *bytes=malloc(DESK_LINK_BYTES+1);if(!bytes){fclose(f);return -ENOMEM;}
    size_t n=fread(bytes,1,DESK_LINK_BYTES+1,f);
    int bad=ferror(f);if(fclose(f))bad=1;
    int result=!bad && desk_link_decode(bytes,(unsigned)n,out) ? 0 : -EINVAL;
    free(bytes);return result;
}
/* A .ciuki launch record uses the existing CIUKLNK3 encoding, avoiding another
 * executable/argument parser. Shell .lnk records are also recognized. */
static int records(struct cp_program_store *s,const char *dir,const char *location,
                   struct desk_launch_record *found,int remove,unsigned depth,unsigned *budget)
{
    if(!depth || !*budget)return -E2BIG;
    char full[1024];int error=path(s,dir,full,sizeof(full));if(error)return error;
    DIR *d=opendir(full);if(!d)return errno==ENOENT ? 0 : -errno;
    int matched=0;struct dirent *e;
    while((e=readdir(d))){
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,".."))continue;
        if(!(*budget)--){error=-E2BIG;break;}
        char rel[260];int n=snprintf(rel,sizeof(rel),"%s/%s",dir,e->d_name);
        if(n<0 || n>=(int)sizeof(rel)){error=-ENAMETOOLONG;break;}
        error=path(s,rel,full,sizeof(full));if(error)break;
        struct stat st;if(program_stat(full,&st)){error=-errno;break;}
        if(S_ISDIR(st.st_mode)){
            int r=records(s,rel,location,found,remove,depth-1,budget);
            if(r<0){error=r;break;}matched+=r;
        }else if(S_ISREG(st.st_mode)){
            const char *ext=strrchr(e->d_name,'.');
            if(!ext || (strcasecmp(ext,".lnk") && strcasecmp(ext,".ciuki")))continue;
            struct desk_launch_record r;
            int read_error=read_record(full,&r);
            if(read_error==-EINVAL)continue;
            if(read_error){error=read_error;break;}
            int belongs=cp_program_contains(location,r.target);
            char launcher[260];
            if(!belongs && normal(r.target,launcher) && !strcasecmp(launcher,"/Programs/dosbox")){
                /* Match a complete executable token, independently of optional
                 * switches, path separators, drive case and catalog spelling. */
                const char *arg=r.arguments;
                while(*arg){
                    while(*arg==' ' || *arg=='\t')arg++;
                    char token[260];unsigned n=0;int quoted=*arg=='"';if(quoted)arg++;
                    while(*arg && (quoted ? *arg!='"' : *arg!=' ' && *arg!='\t')){
                        if(n+1<sizeof(token))token[n++]=*arg;arg++;
                    }
                    if(quoted && *arg=='"')arg++;token[n]=0;
                    char executable[260];
                    if(normal(token,executable) && cp_program_contains(location,executable)){
                        const char *base=strrchr(executable,'/');
                        if(base && (!strcasecmp(base,"/DOOM.EXE") || !strcasecmp(base,"/WOLF3D.EXE") || !strcasecmp(base,"/QUAKE.EXE")))belongs=1;
                    }
                }
            }
            if(!belongs)continue;
            if(found)*found=r;
            if(remove==1 && unlink(full)){error=-errno;break;}
            if(remove==2 && s->forget_icon)s->forget_icon(s->ctx,r.label);
            matched++;
        }
    }
    if(closedir(d) && !error)error=-errno;
    return error ? error : matched;
}
static int launch_record(struct cp_program_store *s,const char *location,struct desk_launch_record *r)
{
    const char *const roots[]={location,"/Games","/Programs","/Windows/Start Menu/Programs","/Windows/Desktop","/Documents/Desktop"};
    for(unsigned i=0;i<sizeof(roots)/sizeof(*roots);i++){
        unsigned budget=WALK_ENTRIES;int n=records(s,roots[i],location,r,0,WALK_DEPTH,&budget);
        if(n>0)return 1;if(n<0 && n!=-ENOTDIR)return n;
    }
    return 0;
}
static int json_string(const char *json,const char *key,char *out,size_t cap)
{
    char pattern[80];snprintf(pattern,sizeof(pattern),"\"%s\"",key);
    const char *p=strstr(json,pattern);if(!p)return 0;p+=strlen(pattern);
    while(*p==' ' || *p=='\n' || *p=='\r' || *p=='\t')p++;
    if(*p++!=':')return 0;
    while(*p==' ' || *p=='\n' || *p=='\r' || *p=='\t')p++;
    if(*p++!='"')return 0;
    unsigned n=0;
    while(*p && *p!='"'){
        char c=*p++;if(c=='\\'){c=*p++;if(c!='"' && c!='\\' && c!='/')return 0;}
        if((unsigned char)c<32 || n+1>=cap)return 0;out[n++]=c;
    }
    out[n]=0;return *p=='"';
}
static __attribute__((noinline)) int game_metadata(struct cp_program_store *s,unsigned game,struct desk_uninstall_entry *r)
{
    char rel[260],full[1024];snprintf(rel,sizeof(rel),"/system/licenses/%s-UPSTREAM.json",packages[game-1]);
    int e=path(s,rel,full,sizeof(full));if(e)return e;
    FILE *f=fopen(full,"rb");if(!f)return -errno;
    char *json=malloc(65537);if(!json){fclose(f);return -ENOMEM;}
    size_t n=fread(json,1,65537,f);int bad=ferror(f);if(fclose(f))bad=1;
    if(bad || n>65536){free(json);return -EIO;}json[n]=0;
    char project[256]={0};int ok=json_string(json,"project",project,sizeof(project)) && json_string(json,"authors",r->publisher,sizeof(r->publisher));
    if(!json_string(json,"version",r->version,sizeof(r->version))){
        char *v=strstr(project," v");if(v){v+=2;unsigned len=0;while(v[len] && v[len]!=' ' && v[len]!=')' && len+1<sizeof(r->version))len++;memcpy(r->version,v,len);r->version[len]=0;}
    }
    const char *lic=strstr(json,"\"license_files\"");char file[128];
    if(lic && json_string(lic,"name",file,sizeof(file)))snprintf(r->licence,sizeof(r->licence),"/system/licenses/%s-%s",packages[game-1],file);
    free(json);return ok ? 0 : -EINVAL;
}
/* Older uninstall keys often omit InstallLocation. A private uninstaller's
 * containing directory can identify its shortcuts; never infer a shared or
 * system directory, and never use this inference for deleting files. */
static __attribute__((noinline)) void uninstall_location(struct desk_uninstall_entry *entry)
{
    if(entry->location[0])return;
    struct desk_launch_record record;char directory[260];
    if(!desk_uninstall_command(entry->command,&record) || !normal(record.target,directory))return;
    char *slash=strrchr(directory,'/');if(!slash || slash==directory)return;*slash=0;
    if(system_path(directory) || !strcasecmp(directory,"/Programs") || !strcasecmp(directory,"/Games") || !strcasecmp(directory,"/Program Files"))return;
    snprintf(entry->location,sizeof(entry->location),"%s",directory);
}
static int compare(const void *a,const void *b)
{
    const struct desk_uninstall_entry *x=a,*y=b;int n=strcasecmp(x->name,y->name);
    if(n)return n;n=strcmp(x->location,y->location);return n ? n : strcmp(x->key,y->key);
}
/* Keep the launch record workspace separate from the inventory candidate;
 * both fit the existing native 4 KiB frame budget without changing flags. */
static __attribute__((noinline)) int user_program(struct cp_program_store *s,
    const char *root_path,const char *name,unsigned root,struct desk_uninstall_entry *r,
    const struct desk_uninstall_entry *out,int n)
{
    char full[1024];int e;
    *r=(struct desk_uninstall_entry){.kind=root ? DESK_PROGRAM_NATIVE : DESK_PROGRAM_DOS};
    int len=snprintf(r->location,sizeof(r->location),"%s/%s",root_path,name);
    if(len<0 || len>=(int)sizeof(r->location)){e=-ENAMETOOLONG;return e;}
    e=path(s,r->location,full,sizeof(full));if(e)return e;
    struct stat st;if(program_stat(full,&st)){e=-errno;return e;}
    if(!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))return 0;
    r->no_remove=r->system_component=system_path(r->location);
    struct desk_launch_record record={0};int have=0;
    if(!r->no_remove){
        have=launch_record(s,r->location,&record);if(have<0){e=have;return e;}
        if(!root && !have)return 0;
    }
    if(!r->no_remove && !S_ISDIR(st.st_mode)){
        FILE *f=fopen(full,"rb");char magic[4];int elf=f && fread(magic,1,4,f)==4 && !memcmp(magic,"\177ELF",4);if(f)fclose(f);if(!elf && !have)return 0;
    }
    /* Registry entries already own their installation directory. */
    int duplicate=0;for(int i=0;i<n;i++)if(out[i].kind==DESK_PROGRAM_WIN32 && cp_program_contains(r->location,out[i].location))duplicate=1;
    if(duplicate)return 0;
    snprintf(r->name,sizeof(r->name),"%.63s",have ? record.label : name);
    if(have){const char *ext=strrchr(record.target,'.');if(ext && (strcasecmp(ext,".exe")==0 || strcasecmp(ext,".com")==0 || strcasecmp(ext,".bat")==0))r->kind=DESK_PROGRAM_DOS;}
    unsigned budget=WALK_ENTRIES;e=walk(full,WALK_DEPTH,&budget,0,&r->disk_bytes);if(e)return e;
    return 1;
}
int cp_program_list(struct cp_program_store *s,struct desk_uninstall_entry *out,unsigned cap)
{
    if(!s || !out || !cap)return -EINVAL;
    s->overflow=0;
    int n=s->registry_list ? s->registry_list(s->ctx,out,cap) : 0;
    /* Registry service absence must not hide DOS/native programs. */
    if(n==-EPIPE || n==-ENOSYS || n==-EAGAIN)n=0;
    if(n<0 || (unsigned)n>cap)return n<0 ? n : -EIO;
    if(n && out[0].inventory_more)s->overflow=1;
    for(int i=0;i<n;i++){
        uninstall_location(&out[i]);
        out[i].kind=DESK_PROGRAM_WIN32;out[i].disk_bytes=(uint64_t)out[i].estimated_kb*1024;
        if(out[i].system_component || system_path(out[i].location))out[i].no_remove=1;
        if(!out[i].command[0])out[i].no_remove=1;
    }
    for(unsigned game=1;game<=DESK_GAME_COUNT;game++){
        uint32_t bytes=0;int present=s->game ? s->game(s->ctx,game,0,&bytes) : 0;
        if(present<0)return present;if(!present)continue;
        if((unsigned)n==cap){s->overflow=1;continue;}
        struct desk_uninstall_entry *r=&out[n];
        *r=(struct desk_uninstall_entry){.kind=DESK_PROGRAM_GAME,.game=game,.disk_bytes=bytes};
        snprintf(r->name,sizeof(r->name),"%s",desk_games[game-1].label);
        snprintf(r->location,sizeof(r->location),"/%s",game==1 ? "WOLF3D" : game==2 ? "DOOM" : "QUAKE");
        int e=game_metadata(s,game,r);if(e)return e;n++;
    }
    const char *const roots[]={"/Games","/Programs"};
    for(unsigned root=0;root<2;root++){
        char full[1024];int e=path(s,roots[root],full,sizeof(full));if(e)return e;
        DIR *d=opendir(full);if(!d){if(errno==ENOENT)continue;return -errno;}
        struct dirent *ent;unsigned entries=WALK_ENTRIES;
        while((ent=readdir(d))){
            if(!entries--){e=-E2BIG;break;}
            if(ent->d_name[0]=='.')continue;
            struct desk_uninstall_entry candidate;
            struct desk_uninstall_entry *r=&candidate;
            int present=user_program(s,roots[root],ent->d_name,root,r,out,n);
            if(present<0){e=present;break;}if(!present)continue;
            if((unsigned)n==cap){
                s->overflow=1;
                if(!r->system_component)continue;
                unsigned replace=cap;while(replace && out[replace-1].system_component)replace--;
                if(!replace)continue;out[replace-1]=*r;
            }else out[n++]=*r;
        }
        if(closedir(d) && !e)e=-errno;if(e)return e;
    }
    static const struct {const char *name,*location;} protected[]={
        {"CiukiDOS kernel","/System/DOS/UNIFIED.IMG"},{"Windows service","/System/Win32/w32srv"},{"System fonts","/System/fonts"}};
    for(unsigned i=0;i<sizeof(protected)/sizeof(*protected);i++){
        struct desk_uninstall_entry candidate;
        struct desk_uninstall_entry *r=&candidate;
        *r=(struct desk_uninstall_entry){.kind=DESK_PROGRAM_NATIVE,.no_remove=1,.system_component=1};
        snprintf(r->name,sizeof(r->name),"%s",protected[i].name);snprintf(r->location,sizeof(r->location),"%s",protected[i].location);
        char full[1024];unsigned budget=WALK_ENTRIES;
        int e=path(s,r->location,full,sizeof(full));if(e)return e;
        e=walk(full,WALK_DEPTH,&budget,0,&r->disk_bytes);if(e && e!=-ENOENT)return e;
        if((unsigned)n==cap){
            s->overflow=1;unsigned replace=cap;while(replace && out[replace-1].system_component)replace--;
            if(replace)out[replace-1]=*r;
        }else out[n++]=*r;
    }
    qsort(out,(unsigned)n,sizeof(*out),compare);
    if(n)out[0].inventory_more=s->overflow;
    return n;
}
int cp_program_remove(struct cp_program_store *s,const struct desk_uninstall_entry *r)
{
    if(!s || !r)return -EINVAL;
    int result=0;
    if(r->no_remove || r->system_component || system_path(r->location))result=-EPERM;
    else if(r->kind==DESK_PROGRAM_WIN32)result=s->registry_run ? s->registry_run(s->ctx,r->key) : -ENOSYS;
    else if(r->kind==DESK_PROGRAM_GAME){
        uint32_t bytes=0;result=s->game ? s->game(s->ctx,r->game,1,&bytes) : -ENOSYS;
        if(result>=0)result=0;
        if(!result){
            char rel[128],full[1024];snprintf(rel,sizeof(rel),"/system/settings/dos-game-%u.cfg",r->game);
            result=path(s,rel,full,sizeof(full));if(!result && unlink(full) && errno!=ENOENT)result=-errno;
        }
    }else if(r->kind==DESK_PROGRAM_DOS || r->kind==DESK_PROGRAM_NATIVE){
        char normalized[260],full[1024];
        if(!normal(r->location,normalized) ||
           (!cp_program_contains("/Games",normalized) && !cp_program_contains("/Programs",normalized)) ||
           !strcasecmp(normalized,"/Games") || !strcasecmp(normalized,"/Programs"))result=-EPERM;
        else {
            result=path(s,normalized,full,sizeof(full));uint64_t bytes=0;unsigned budget=WALK_ENTRIES;
            if(!result)result=walk(full,WALK_DEPTH,&budget,0,&bytes); /* preflight */
            budget=WALK_ENTRIES;if(!result)result=walk(full,WALK_DEPTH,&budget,1,&bytes);
        }
    }else result=-EINVAL;
    if(!result){
        /* Win32 success means the process exited, not that it unregistered.
         * Keep its shortcuts until a fresh inventory proves key absence. */
        if(r->kind==DESK_PROGRAM_WIN32 && s->registry_check){
            int present=s->registry_check(s->ctx,r->key);
            result=present<0 ? present : present ? -EAGAIN : 0;
        }else if(r->kind==DESK_PROGRAM_WIN32){
            struct desk_uninstall_entry *fresh=calloc(DESK_UNINSTALL_MAX,sizeof(*fresh));
            int n=fresh && s->registry_list ? s->registry_list(s->ctx,fresh,DESK_UNINSTALL_MAX) : -ENOMEM;
            if(n<0)result=n;
            /* An incomplete snapshot cannot prove that a surviving key is absent. */
            if(n>0 && fresh[0].inventory_more)result=-EAGAIN;
            for(int i=0;i<n;i++)if(!strcmp(fresh[i].key,r->key))result=-EAGAIN;
            free(fresh);
        }
        const char *const roots[]={"/Windows/Start Menu/Programs","/Windows/Desktop","/Documents/Desktop","/Games","/Programs"};
        /* First collect icon ownership while every record still exists.
         * Persist that snapshot before deleting the records, so a failed save
         * or restart never loses the identities needed for a retry. */
        for(unsigned phase=2;phase && !result;phase--){
            for(unsigned i=0;!result && r->location[0] && i<5;i++){
                unsigned budget=WALK_ENTRIES;int e=records(s,roots[i],r->location,NULL,(int)phase,WALK_DEPTH,&budget);if(e<0)result=e;
            }
            if(phase==2 && !result && s->persist)result=s->persist(s->ctx);
        }
    }
    if(!result && r->kind==DESK_PROGRAM_GAME){
        char rel[64],full[1024];snprintf(rel,sizeof(rel),"/System/REMOVE%u.PND",r->game);
        result=path(s,rel,full,sizeof(full));
        if(!result && unlink(full) && errno!=ENOENT)result=-errno;
    }
    cp_program_record(s,r,result);return result;
}
void cp_program_record(struct cp_program_store *s,const struct desk_uninstall_entry *r,int result)
{
    if(s->log){char line[256];const char *const kinds[]={"win32","bundled-dos","dos","native"};
        char name[64];snprintf(name,sizeof(name),"%s",r->name);for(char *p=name;*p;p++)if((unsigned char)*p<32 || *p==127)*p='?';
        snprintf(line,sizeof(line),"[control] remove name=%s kind=%s result=%d\n",name,r->kind<4 ? kinds[r->kind] : "invalid",result);s->log(s->ctx,line);
    }
}
int cp_program_installer(const char *name)
{
    if(!name || strlen(name)>DESK_ARG_MAX)return 0;
    const char *base=strrchr(name,'/');base=base ? base+1 : name;
    size_t n=strlen(base);return !strcasecmp(base,"SETUP.EXE") || !strcasecmp(base,"INSTALL.EXE") || (n>4 && !strcasecmp(base+n-4,".BAT"));
}
