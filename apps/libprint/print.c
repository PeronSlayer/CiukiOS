/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* sec-user-05 (user-security-contract.md 8.2; sec-k05 open item): a profile
 * session queues its jobs in its own <profile>/Settings/Spool, serviced by
 * that session's worker (apps/printers/spool.c), so Standard users print
 * without write access to /WINDOWS. The printer configuration stays the
 * machine's CP_ROOT/printers: everyone reads it, administrators change it.
 * A profile-less bootstrap session keeps the machine queue. An explicit
 * root (host fixtures) holds both, as before. */
static const char *queue_root(void)
{
    static char own[160];
    const char *profile=getenv("USERPROFILE");
    if(!profile || strncmp(profile,"/Windows/Profiles/",18) || !profile[18] || strlen(profile)>=96 || strstr(profile,"/.."))return CP_ROOT;
    snprintf(own,sizeof(own),"%s/Settings/Spool",profile);return own;
}
static const char *base(const char *r) { return r ? r : queue_root(); }
static const char *config_base(const char *r) { return r ? r : CP_ROOT; }
static int path(char *out, size_t n, const char *r, unsigned id, const char *leaf)
{
    int k = id ? snprintf(out,n,"%s/j%03u/%s",base(r),id,leaf) : snprintf(out,n,"%s/%s",base(r),leaf);
    if (k<0 || (size_t)k>=n) { errno=ENAMETOOLONG; return -1; } return 0;
}
static int config_path(char *out, size_t n, const char *r, const char *leaf)
{
    int k = snprintf(out,n,"%s/%s",config_base(r),leaf);
    if (k<0 || (size_t)k>=n) { errno=ENAMETOOLONG; return -1; } return 0;
}
static int valid_string(const char *s,size_t n) { return memchr(s,0,n)!=NULL; }
static int write_all(int fd,const void *v,size_t n)
{
    const unsigned char *p=v;
    while(n) { ssize_t k=write(fd,p,n); if(k<0 && errno==EINTR) continue; if(k<=0) { if(!k) errno=EIO; return -1; } p+=k;n-=(size_t)k; } return 0;
}
static int read_record(const char *p,void *v,size_t n)
{
    int fd=open(p,O_RDONLY); if(fd<0) return -1;
    size_t at=0; int ok=0;
    while(at<n) { ssize_t k=read(fd,(char *)v+at,n-at); if(k<0 && errno==EINTR) continue; if(k<=0) { ok=-1;break; } at+=(size_t)k; }
    if(close(fd)) ok=-1; if(ok) errno=EIO; return ok;
}
static int save_record(const char *p,const void *v,size_t n)
{
    char tmp[CP_PATH+96],backup[CP_PATH+96];
    int k=snprintf(tmp,sizeof(tmp),"%s.tmp",p);
    if(k<0 || (size_t)k>=sizeof(tmp)){errno=ENAMETOOLONG;return -1;}
    k=snprintf(backup,sizeof(backup),"%s.bak",p);
    if(k<0 || (size_t)k>=sizeof(backup)){errno=ENAMETOOLONG;return -1;}
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return -1;
    int e=write_all(fd,v,n);if(!e && fsync(fd))e=-1;if(close(fd))e=-1;
    if(e){unlink(tmp);return -1;}
    /* Keep the last complete record across FAT's non-replacing rename. */
    unlink(backup);
    if(rename(p,backup) && errno!=ENOENT)return -1;
    if(rename(tmp,p))return -1;
    unlink(backup);return 0;
}
static int read_saved(const char *p,void *v,size_t n)
{
    if(!read_record(p,v,n))return 0;
    if(errno!=ENOENT)return -1;
    char backup[CP_PATH+96],tmp[CP_PATH+96];
    snprintf(backup,sizeof(backup),"%s.bak",p);
    /* A backup proves this is an interrupted replacement, not a producer
     * still publishing its first job. */
    if(read_record(backup,v,n))return -1;
    snprintf(tmp,sizeof(tmp),"%s.tmp",p);
    if(!read_record(tmp,v,n))return 0;
    return read_record(backup,v,n);
}
static int acquire_lock(const char *r,const char *name,int self_stale)
{
    char p[CP_PATH+64],owner[CP_PATH+96];
    if(strcmp(name,"config.lock") ? path(p,sizeof(p),r,0,name) : config_path(p,sizeof(p),r,name))return -1;
    snprintf(owner,sizeof(owner),"%s/owner",p);
    if(mkdir(p,0700)){
        uint32_t pid=0;
        if(errno!=EEXIST)return -1;
        /* The single worker's record naming this very process was left by an
         * earlier boot (PIDs restart at every boot; a power loss keeps it). */
        if(read_record(owner,&pid,sizeof(pid)) || !pid ||
           ((!self_stale || pid!=(uint32_t)getpid()) && (!kill((pid_t)pid,0) || errno!=ESRCH))){errno=EBUSY;return -1;}
        unlink(owner);if(rmdir(p) || mkdir(p,0700)){errno=EBUSY;return -1;}
    }
    uint32_t pid=(uint32_t)getpid();int fd=open(owner,O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(fd<0){rmdir(p);return -1;}
    int e=write_all(fd,&pid,sizeof(pid));if(close(fd))e=-1;
    if(e){unlink(owner);rmdir(p);}return e;
}
static void release_lock(const char *r,const char *name)
{
    int saved=errno;char p[CP_PATH+96];
    if(!(strcmp(name,"config.lock") ? path(p,sizeof(p),r,0,name) : config_path(p,sizeof(p),r,name))){size_t n=strlen(p);if(n+6<sizeof(p)){strcat(p,"/owner");unlink(p);p[n]=0;rmdir(p);}}
    errno=saved;
}
static int config_lock(const char *r){return acquire_lock(r,"config.lock",0);}
static void config_unlock(const char *r){release_lock(r,"config.lock");}
static int worker_held;  /* this process holds worker.lock: its record is not stale */
int cp_service_lock(const char *r){int e=acquire_lock(r,"worker.lock",!worker_held);if(!e)worker_held=1;return e;}
/* The doorbell (print.h). kill() reaches the worker in the desktop's process
 * group; a missing or dead owner is not the producer's failure. */
int cp_service_notify(const char *r)
{
    int saved=errno;char p[CP_PATH+64];uint32_t pid=0;
    int e=path(p,sizeof(p),r,0,CP_WORKER_OWNER) || read_record(p,&pid,sizeof(pid)) || !pid || kill((pid_t)pid,SIGCHLD) ? -1 : 0;
    errno=saved;return e;
}
void cp_service_unlock(const char *r){release_lock(r,"worker.lock");worker_held=0;}

void cp_page_default(struct cp_page *p) { *p=(struct cp_page){612,792,0,36,36,36,36,1,0,0}; }
int cp_page_valid(const struct cp_page *p)
{
    if(!p || p->width<144 || p->width>2448 || p->height<144 || p->height>2448 || p->landscape>1 || !p->copies || p->copies>99 || (p->last && p->last<p->first))return 0;
    uint32_t w=p->landscape?p->height:p->width,h=p->landscape?p->width:p->height;
    return p->left<w && p->right<w-p->left && p->top<h && p->bottom<h-p->top;
}

static void default_config(struct cp_config *c)
{
    memset(c,0,sizeof(*c));c->magic=CP_CONFIG_MAGIC;c->version=1;
    c->printers[0].present=1;strcpy(c->printers[0].name,"Generic / Text Only");strcpy(c->printers[0].port,"/Documents/print.prn");cp_page_default(&c->printers[0].page);
}
int cp_config_load(const char *r,struct cp_config *c)
{
    char p[CP_PATH+64]; if(config_path(p,sizeof(p),r,"printers")) return -1;
    if(read_saved(p,c,sizeof(*c))) {if(errno==ENOENT){default_config(c);return 0;}return -1;}
    if(c->magic!=CP_CONFIG_MAGIC || c->version!=1 || c->default_printer>=CP_PRINTERS || !c->printers[c->default_printer].present) { errno=EINVAL;return -1; }
    for(unsigned i=0;i<CP_PRINTERS;i++) if(c->printers[i].present &&
       (c->printers[i].driver>CP_PCL || !valid_string(c->printers[i].name,sizeof(c->printers[i].name)) || !valid_string(c->printers[i].port,CP_PATH) || !cp_page_valid(&c->printers[i].page))) { errno=EINVAL;return -1; }
    return 0;
}
static int config_save(const char *r,const struct cp_config *c)
{
    char p[CP_PATH+64]; if(config_path(p,sizeof(p),r,"printers")) return -1;
    if(c->magic!=CP_CONFIG_MAGIC || c->version!=1 || c->default_printer>=CP_PRINTERS || !c->printers[c->default_printer].present) { errno=EINVAL;return -1; }
    for(unsigned i=0;i<CP_PRINTERS;i++)if(c->printers[i].present &&
        (c->printers[i].driver>CP_PCL || !valid_string(c->printers[i].name,64) || !valid_string(c->printers[i].port,CP_PATH) || !cp_page_valid(&c->printers[i].page))){errno=EINVAL;return -1;}
    return save_record(p,c,sizeof(*c));
}
int cp_config_save(const char *r,const struct cp_config *c)
{if(config_lock(r))return -1;int e=config_save(r,c);config_unlock(r);if(!e)cp_service_notify(r);return e;}

int cp_init(const char *r)
{
    /* sec-user-05: a profile queue's Settings parent is created inside the profile. */
    char settings[CP_PATH];const char *q=base(r),*slash=strrchr(q,'/');
    if(!r && strcmp(q,CP_ROOT) && slash && (size_t)(slash-q)<sizeof(settings)){
        memcpy(settings,q,(size_t)(slash-q));settings[slash-q]=0;
        if(mkdir(settings,0700) && errno!=EEXIST) return -1;
    }
    if(mkdir(q,0700) && errno!=EEXIST) return -1;
    struct cp_config *c=malloc(sizeof(*c)); if(!c) return -1;
    int e=cp_config_load(r,c);char p[CP_PATH+64];struct stat st;
    /* The default configuration is published by whoever may write the
     * machine folder; a Standard session prints with it unpublished. */
    if(!e && !config_path(p,sizeof(p),r,"printers") && stat(p,&st)){
        e=cp_config_save(r,c);
        if(e && (errno==EACCES || errno==EPERM || errno==EROFS))e=0;
    }
    free(c);return e;
}
int cp_add_printer(const char *r,const char *name,const char *port,unsigned driver,unsigned *index)
{
    if(!name || !*name || strlen(name)>=64 || !port || port[0]!='/' || strlen(port)>=CP_PATH || driver>CP_PCL) { errno=EINVAL;return -1; }
    if(config_lock(r))return -1;
    struct cp_config *c=malloc(sizeof(*c));if(!c){config_unlock(r);return -1;}
    int e=cp_config_load(r,c);unsigned i=0;
    if(!e){for(unsigned k=0;k<CP_PRINTERS;k++)if(c->printers[k].present && !strcmp(c->printers[k].name,name)){errno=EEXIST;e=-1;break;}}
    if(!e) { for(;i<CP_PRINTERS && c->printers[i].present;i++) {}if(i==CP_PRINTERS){errno=ENOSPC;e=-1;} }
    if(!e){struct cp_printer *p=&c->printers[i];memset(p,0,sizeof(*p));p->present=1;p->driver=driver;strcpy(p->name,name);strcpy(p->port,port);cp_page_default(&p->page);e=config_save(r,c);if(!e && index)*index=i;}
    free(c);config_unlock(r);return e;
}
int cp_set_default(const char *r,unsigned i)
{
    if(config_lock(r))return -1;
    struct cp_config *c=malloc(sizeof(*c));if(!c){config_unlock(r);return -1;}int e=cp_config_load(r,c);
    if(!e && (i>=CP_PRINTERS || !c->printers[i].present)){errno=EINVAL;e=-1;}if(!e){c->default_printer=i;e=config_save(r,c);}free(c);config_unlock(r);return e;
}
int cp_printer_pause(const char *r,unsigned i,int pause)
{
    if(config_lock(r))return -1;
    struct cp_config *c=malloc(sizeof(*c));if(!c){config_unlock(r);return -1;}int e=cp_config_load(r,c);
    if(!e && (i>=CP_PRINTERS || !c->printers[i].present)){errno=EINVAL;e=-1;}if(!e){c->printers[i].paused=!!pause;e=config_save(r,c);}free(c);config_unlock(r);
    if(!e)cp_service_notify(r);return e;
}
int cp_job_read(const char *r,unsigned id,struct cp_job *j)
{
    char p[CP_PATH+64];if(id<1 || id>CP_JOBS){errno=EINVAL;return -1;}if(path(p,sizeof(p),r,id,"job"))return -1;
    if(read_saved(p,j,sizeof(*j)))return -1;
    if(j->magic!=CP_JOB_MAGIC || j->version!=1 || j->id!=id || j->printer>=CP_PRINTERS || j->state>CP_ERROR || j->bytes>CP_MAX_BYTES || j->offset>j->bytes || !cp_page_valid(&j->page) || j->completed>=j->page.copies || !valid_string(j->title,CP_TITLE) || !valid_string(j->target,CP_PATH)) { errno=EINVAL;return -1; }return 0;
}
/* One directory read names the reserved slots. Probing all 128 slots for
 * job and job.bak cost the background worker 256 failed path walks through
 * /WINDOWS on every idle pass (QEMU, image 56e58374: printspool was the top
 * CPU user, median 38 %, and held C: for 81 ms per lookup). POSIX readdir;
 * FAT may return the short name in either case. */
int cp_job_ids(const char *r,unsigned ids[CP_JOBS])
{
    DIR *d=opendir(base(r));if(!d)return -1;
    unsigned char seen[CP_JOBS+1]={0};struct dirent *e;
    while((e=readdir(d))){
        const char *s=e->d_name;unsigned id=0,i=1;
        if((s[0]!='j' && s[0]!='J') || strlen(s)!=4)continue;
        for(;i<4 && s[i]>='0' && s[i]<='9';i++)id=id*10+(unsigned)(s[i]-'0');
        if(i==4 && id>=1 && id<=CP_JOBS)seen[id]=1;
    }
    closedir(d);
    unsigned n=0;for(unsigned id=1;id<=CP_JOBS;id++)if(seen[id])ids[n++]=id;
    return (int)n;
}
int cp_job_control(const char *r,unsigned id,enum cp_control ctl)
{
    struct cp_job j;char p[CP_PATH+64];if(ctl<CP_PAUSE || ctl>CP_CANCEL){errno=EINVAL;return -1;}if(cp_job_read(r,id,&j) || path(p,sizeof(p),r,id,"ctl"))return -1;
    char c=ctl==CP_PAUSE?'p':ctl==CP_RESUME?'r':'c';if(save_record(p,&c,1))return -1;cp_service_notify(r);return 0;
}
int cp_purge(const char *r,unsigned printer)
{
    unsigned ids[CP_JOBS];int n=cp_job_ids(r,ids);if(n<0)return -1;
    int e=0;for(int i=0;i<n;i++){unsigned id=ids[i];struct cp_job j;if(!cp_job_read(r,id,&j) && j.printer==printer && cp_job_control(r,id,CP_CANCEL))e=-1;}return e;
}
static void release(const char *r,unsigned id)
{
    char p[CP_PATH+64];const char *names[]={"data","job","job.tmp","job.bak","ctl","ctl.tmp","ctl.bak"};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++){if(!path(p,sizeof(p),r,id,names[i]))unlink(p);}if(!path(p,sizeof(p),r,id,""))rmdir(p);
}
/* Renderers only write a private temporary FILE; submission publishes it. */
static int submit(const char *r,unsigned printer,const char *title,FILE *source,const struct cp_page *page,const char *target)
{
    struct cp_config *c=malloc(sizeof(*c));if(!c)return -1;int e=cp_init(r);if(!e)e=cp_config_load(r,c);
    if(!e && printer==CP_PRINTERS)printer=c->default_printer;
    if(!e && (printer>=CP_PRINTERS || !c->printers[printer].present || (page && !cp_page_valid(page)) || (target && (target[0]!='/' || strlen(target)>=CP_PATH)))){errno=EINVAL;e=-1;}
    struct cp_job j;memset(&j,0,sizeof(j));j.magic=CP_JOB_MAGIC;j.version=1;j.printer=printer;
    if(!e)j.page=page?*page:c->printers[printer].page;free(c);if(e)return -1;
    snprintf(j.title,sizeof(j.title),"%s",title?title:"Document");if(target)strcpy(j.target,target);
    char p[CP_PATH+64];unsigned id;
    for(id=1;id<=CP_JOBS;id++){if(path(p,sizeof(p),r,id,""))return -1;if(!mkdir(p,0700))break;if(errno!=EEXIST)return -1;}
    if(id>CP_JOBS){errno=ENOSPC;return -1;}j.id=id;
    if(path(p,sizeof(p),r,id,"data")){release(r,id);return -1;}
    int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0){release(r,id);return -1;}
    unsigned char buf[1024];size_t n;rewind(source);
    while((n=fread(buf,1,sizeof(buf),source))){if(n>CP_MAX_BYTES-j.bytes){errno=EFBIG;e=-1;break;}if(write_all(fd,buf,n)){e=-1;break;}j.bytes+=(uint32_t)n;}
    if(ferror(source))e=-1;if(!e && fsync(fd))e=-1;if(close(fd))e=-1;
    if(!e && !path(p,sizeof(p),r,id,"job"))e=save_record(p,&j,sizeof(j));else e=-1;
    if(e){int saved=errno;release(r,id);errno=saved;return -1;}cp_service_notify(r);return (int)id;
}
int cp_submit_file(const char *r,unsigned printer,const char *title,const char *source,const struct cp_page *p,const char *target)
{FILE *f=fopen(source,"rb");if(!f)return -1;int id=submit(r,printer,title,f,p,target);fclose(f);return id;}
int cp_submit_raw(const char *r,unsigned printer,const char *title,const void *data,size_t n,const struct cp_page *p,const char *target)
{
    if((!data && n)|| n>CP_MAX_BYTES){errno=EINVAL;return -1;}char temp[CP_PATH+96];FILE *f=cp_tempfile(r,temp,sizeof(temp));if(!f)return -1;
    int id=-1;if(fwrite(data,1,n,f)==n)id=submit(r,printer,title,f,p,target);cp_tempclose(f,temp);return id;
}
const char *cp_state_name(unsigned s){static const char *const names[]={"Queued","Paused","Printing","Offline","Error"};return s<=CP_ERROR?names[s]:"Invalid";}
int cp_service_step(const char *r){return cp_service_next(r,NULL);}
int cp_service_next(const char *r,unsigned *offline)
{
    if(offline)*offline=0;
    unsigned ids[CP_JOBS];int count=cp_job_ids(r,ids);if(count<=0)return count;  /* idle: no slot reserved */
    struct cp_config *c=malloc(sizeof(*c));if(!c)return -1;if(cp_config_load(r,c)){free(c);return -1;}
    for(int k=0;k<count;k++){
        unsigned id=ids[k];struct cp_job j;char p[CP_PATH+64];if(cp_job_read(r,id,&j))continue;
        char ctl=0;if(!path(p,sizeof(p),r,id,"ctl") && !read_saved(p,&ctl,1)){
            if(ctl=='c'){release(r,id);free(c);return 1;}if(ctl=='p')j.state=CP_PAUSED;if(ctl=='r')j.state=CP_QUEUED;unlink(p);
            if(path(p,sizeof(p),r,id,"job") || save_record(p,&j,sizeof(j))){free(c);return -1;}
        }
        if(j.state==CP_PAUSED || c->printers[j.printer].paused || !c->printers[j.printer].present)continue;
        const char *target=j.target[0]?j.target:c->printers[j.printer].port;
        if(path(p,sizeof(p),r,id,"data")){free(c);return -1;}
        int in=open(p,O_RDONLY),out=-1;int e=0;unsigned char buf[1024];ssize_t n=-1;
        if(in>=0 && lseek(in,j.offset,SEEK_SET)>=0)n=read(in,buf,j.bytes-j.offset<sizeof(buf)?j.bytes-j.offset:sizeof(buf));
        if(n>=0){int flags=O_WRONLY;
            if(!strncmp(target,"/dev/",5))flags|=O_NONBLOCK;
            else flags|=O_CREAT|((j.offset==0 && j.completed==0)?O_TRUNC:0);
            out=open(target,flags,0600);
            if(out>=0 && strncmp(target,"/dev/",5) && lseek(out,(off_t)j.completed*j.bytes+j.offset,SEEK_SET)<0){close(out);out=-1;}
        }
        if(out<0 || n<0 || (n==0 && j.offset<j.bytes))e=-1;
        if(!e && n>0){ssize_t wrote=write(out,buf,(size_t)n);if(wrote>0)j.offset+=(uint32_t)wrote;else e=-1;}
        if(in>=0)close(in);if(out>=0){if(strncmp(target,"/dev/",5) && fsync(out))e=-1;if(close(out))e=-1;}
        if(!e && j.offset==j.bytes){j.completed++;j.offset=0;if(j.completed>=j.page.copies){release(r,id);free(c);return 1;}}
        j.state=e?CP_OFFLINE:CP_PRINTING;if(e && offline)(*offline)++;
        if(path(p,sizeof(p),r,id,"job") || save_record(p,&j,sizeof(j))){free(c);return -1;}
        /* An offline printer must not starve other printers in this pass. */
        if(!e){free(c);return 1;}
    }
    free(c);return 0;
}

FILE *cp_tempfile(const char *r,char *out,size_t cap)
{
    if(cp_init(r))return NULL;
    for(unsigned i=0;i<CP_JOBS;i++) {
        int n=snprintf(out,cap,"%s/r%03u",base(r),i);
        if(n<0 || (size_t)n+6>=cap){errno=ENAMETOOLONG;return NULL;}
        if(mkdir(out,0700)){if(errno==EEXIST)continue;return NULL;}
        strcat(out,"/data");FILE *f=fopen(out,"w+b");
        if(!f){out[strlen(out)-5]=0;rmdir(out);}return f;
    }
    errno=ENOSPC;return NULL;
}
void cp_tempclose(FILE *f,const char *p)
{
    char dir[CP_PATH+96];fclose(f);unlink(p);snprintf(dir,sizeof(dir),"%s",p);
    char *slash=strrchr(dir,'/');if(slash){*slash=0;rmdir(dir);}
}
