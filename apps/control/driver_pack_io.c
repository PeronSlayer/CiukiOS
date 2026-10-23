/* Local media adapter. FAT/ISO target has no links; the POSIX host adapter
 * walks pinned directory descriptors with O_NOFOLLOW. SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "driver_pack.h"
#include "../../src/kernel/include/ciuki/sha256.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static dp_fault inject;
static void *inject_ctx;
void dp_test_fault(dp_fault fn,void *ctx) { inject=fn; inject_ctx=ctx; }
static int fault(const char *op) { if(inject && inject(op,inject_ctx)) { errno=EIO; return 1; } return 0; }
static int equal(const char *a,const char *b)
{ while(*a && ((*a|32)==(*b|32))) { a++; b++; } return *a==*b; }
static int join(char out[DP_PATH],const char *a,const char *b)
{ if(snprintf(out,DP_PATH,"%s%s%s",a,strcmp(a,"/")?"/":"",b)>=(int)DP_PATH) { errno=ENAMETOOLONG; return -1; } return 0; }
static int no_links(const char *path,int directory)
{
    if(!path || path[0]!='/') { errno=EINVAL; return -1; }
    char copy[DP_PATH]; if(strlen(path)>=sizeof(copy)) { errno=ENAMETOOLONG; return -1; } strcpy(copy,path);
    for(char *c=copy+1;;c++) if(*c=='/' || !*c) {
        int last=!*c; char saved=*c; *c=0; struct stat st;
#ifdef DP_NATIVE_VFS
        int r=stat(copy,&st);
#else
        int r=lstat(copy,&st);
#endif
        if(r || (!last && !S_ISDIR(st.st_mode)) || (last && (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)))) { if(!r) errno=EINVAL; return -1; }
        *c=saved; if(last) return 0;
    }
}
static int root_ok(const char *root)
{
    if(!root || root[0]!='/' || strlen(root)>=DP_PATH || strstr(root,"/../") || strstr(root,"/./") || strstr(root,"//") ||
       (strlen(root)>1 && root[strlen(root)-1]=='/')) { errno=EINVAL; return -1; }
    const char *last=strrchr(root,'/'); if(last && (!strcmp(last,"/..") || !strcmp(last,"/."))) { errno=EINVAL; return -1; }
    return no_links(root,1);
}
static int source_open(const char *root,const char *relative)
{
    if(!ndp_relative(relative) || root_ok(root)) { errno=EINVAL; return -1; }
#ifndef DP_NATIVE_VFS
    /* Pin every component, including root's ancestors, against symlink swaps. */
    int fd=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC); if(fd<0) return -1;
    char path[DP_PATH]; if(join(path,root,relative)) { close(fd); return -1; }
    char *at=path+1;
    while(*at) {
        char *slash=strchr(at,'/'); if(slash) *slash=0;
        /* Windows media are case insensitive. Resolve one unique spelling;
         * case-colliding host files are rejected, never picked randomly. */
        char actual[256]; actual[0]=0;
        int duplicate=0,scan_fd=dup(fd); DIR *directory=scan_fd>=0?fdopendir(scan_fd):NULL;
        if(!directory) { if(scan_fd>=0) close(scan_fd); close(fd); return -1; }
        struct dirent *entry;
        while((entry=readdir(directory))) if(equal(entry->d_name,at)) { if(actual[0]) duplicate=1; else snprintf(actual,sizeof(actual),"%s",entry->d_name); }
        if(closedir(directory) || duplicate || !actual[0]) { close(fd); errno=EINVAL; return -1; }
        int next=openat(fd,actual,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|(slash?O_DIRECTORY:0));
        int saved=errno; close(fd); errno=saved; if(next<0) return -1; fd=next;
        if(!slash) break;
        at=slash+1;
    }
#else
    char path[DP_PATH]; if(join(path,root,relative) || no_links(path,0)) return -1;
    int fd=open(path,O_RDONLY|O_CLOEXEC); if(fd<0) return -1;
#endif
    struct stat st; if(fstat(fd,&st) || !S_ISREG(st.st_mode)) { close(fd); errno=EINVAL; return -1; } return fd;
}
static int fetch(void *ctx,const char *name,uint8_t **out,size_t *length)
{
    const char *root=ctx; int fd=source_open(root,name); if(fd<0) return -errno;
    struct stat st; int result=-EINVAL;
    if(fstat(fd,&st) || st.st_size<=0 || st.st_size>DP_INF_LIMIT) goto done;
    *length=(size_t)st.st_size; *out=malloc(*length); if(!*out) { result=-ENOMEM; goto done; }
    size_t at=0; while(at<*length) { ssize_t n=read(fd,*out+at,*length-at); if(n<=0) goto done; at+=(size_t)n; }
    uint8_t extra; if(read(fd,&extra,1)!=0) goto done;
    result=0;
 done:
    if(close(fd) && !result) result=-EIO;
    if(result) { free(*out); *out=NULL; }
    return result;
}
static int hash_file(const char *root,const char *name,uint32_t *size,uint8_t hash[32])
{
    int fd=source_open(root,name); if(fd<0) return -errno;
    uint8_t block[1024]; struct sha256_ctx ctx; sha256_init(&ctx); *size=0; int result=0;
    for(;;) { ssize_t n=read(fd,block,sizeof(block)); if(n<0) { result=-errno; break; } if(!n) break;
        if((uint32_t)n>4u*1024*1024-*size) { result=-EFBIG; break; }
        *size+=(uint32_t)n; sha256_update(&ctx,block,(size_t)n);
    }
    if(close(fd) && !result) result=-errno;
    if(!*size) result=-EINVAL;
    sha256_final(&ctx,hash); return result;
}
static int pe_header(const char *root,const char *name)
{
    int fd=source_open(root,name); if(fd<0) return -1;
    unsigned char dos[64],pe[94]; int ok=0;
    if(read(fd,dos,sizeof(dos))!=(ssize_t)sizeof(dos) || dos[0]!='M' || dos[1]!='Z') goto done;
    uint32_t offset=(uint32_t)dos[60]|((uint32_t)dos[61]<<8)|((uint32_t)dos[62]<<16)|((uint32_t)dos[63]<<24);
    if(offset>4u*1024*1024-sizeof(pe) || lseek(fd,offset,SEEK_SET)<0 || read(fd,pe,sizeof(pe))!=(ssize_t)sizeof(pe)) goto done;
    ok=!memcmp(pe,"PE\0\0",4) && pe[4]==0x4c && pe[5]==1 && pe[24]==0x0b && pe[25]==1 && pe[92]==1 && !pe[93];
 done:
    if(close(fd)) ok=0;
    return ok?0:-1;
}
int dp_preview(struct dp_plan *plan,const char *root,const char *inf,const struct ndp_device *dev,char reason[160])
{
    if(root_ok(root) || !ndp_relative(inf)) { snprintf(reason,160,"Select a local folder or mounted CD with safe INF paths."); return -EINVAL; }
    char folder[DP_PATH]; if(strlen(root)>=sizeof(folder)) return -ENAMETOOLONG; strcpy(folder,root);
    const char *name=strrchr(inf,'/'); char prefix[192]="";
    if(name) { size_t n=(size_t)(name-inf); if(n>=sizeof(prefix)) return -ENAMETOOLONG; memcpy(prefix,inf,n); prefix[n]=0; if(join(folder,root,prefix)) return -errno; name++; }
    else name=inf;
    int r=dp_parse(plan,name,dev,2,fetch,folder,reason); if(r) return r;
    if(pe_header(folder,plan->source[plan->manifest.binary])) { snprintf(reason,160,"Binary is not a native i386 PE32 NT driver."); return -EINVAL; }
    for(unsigned i=0;i<plan->manifest.file_count;i++) {
        struct ndp_file *file=&plan->manifest.files[i];
        uint32_t bytes; uint8_t hash[32];
        if(hash_file(folder,plan->source[i],&bytes,hash)) { snprintf(reason,160,"Missing, unsafe, compressed or oversized dependency: %.64s",plan->source[i]); return -EINVAL; }
        if(file->bytes && (file->bytes!=bytes || memcmp(file->hash,hash,32))) { snprintf(reason,160,"Source INF changed during selection; select the package again."); return -EINVAL; }
        file->bytes=bytes; memcpy(file->hash,hash,32);
        if(*prefix) { char source[192]; if(snprintf(source,sizeof(source),"%s/%s",prefix,plan->source[i])>=(int)sizeof(source)) return -ENAMETOOLONG; strcpy(plan->source[i],source); }
    }
    ndp_seal(&plan->manifest); snprintf(reason,160,"%s",plan->manifest.reason); return 0;
}
static int scan(const char *root,const char *prefix,char names[][192],unsigned cap,unsigned *count,unsigned *entries,unsigned depth)
{
    if(depth>8) return -E2BIG;
    char path[DP_PATH]; if(*prefix ? join(path,root,prefix) : (strcpy(path,root),0)) return -errno;
    if(no_links(path,1)) return -errno;
    DIR *dir=opendir(path); if(!dir) return -errno;
    int result=0; struct dirent *de;
    while((de=readdir(dir))) {
        if(!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
        if(++*entries>4096) { result=-E2BIG; break; }
        if(!ndp_component(de->d_name)) continue;
        char relative[192]; if(snprintf(relative,sizeof(relative),"%s%s%s",prefix,*prefix?"/":"",de->d_name)>=(int)sizeof(relative) || join(path,root,relative)) { result=-ENAMETOOLONG; break; }
        struct stat st;
#ifdef DP_NATIVE_VFS
        if(stat(path,&st)) { result=-errno; break; }
#else
        if(lstat(path,&st)) { result=-errno; break; }
#endif
        if(S_ISDIR(st.st_mode)) { result=scan(root,relative,names,cap,count,entries,depth+1); if(result) break; }
        else if(S_ISREG(st.st_mode)) {
            const char *ext=strrchr(de->d_name,'.'); if(!ext || !equal(ext,".inf")) continue;
            if(*count==cap) { result=-E2BIG; break; } strcpy(names[(*count)++],relative);
        }
    }
    if(closedir(dir) && !result) result=-errno;
    return result;
}
static int order(const void *a,const void *b) { return strcmp(a,b); }
int dp_scan(const char *root,char names[][192],unsigned cap)
{
    if(root_ok(root)) return -errno;
    unsigned count=0,entries=0; int r=scan(root,"",names,cap,&count,&entries,0);
    if(r) return r;
    qsort(names,count,sizeof(names[0]),order); return (int)count;
}
static int write_all(int fd,const void *bytes,size_t length)
{
    const uint8_t *p=bytes;
    while(length) { if(fault("write")) return -1; ssize_t n=write(fd,p,length); if(n<=0) return -1; p+=n; length-=(size_t)n; }
    return 0;
}
static int write_file(const char *dir,const char *name,const void *bytes,size_t length)
{
    char path[DP_PATH]; if(join(path,dir,name) || fault("create")) return -1;
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600); if(fd<0) return -1;
    int r=write_all(fd,bytes,length); if(!r && (fault("fsync") || fsync(fd))) r=-1;
    if(close(fd)) r=-1;
    return r;
}
static int copy_file(const char *source,const char *relative,const char *stage,const struct ndp_file *file)
{
    int from=source_open(source,relative); if(from<0) return -1;
    char path[DP_PATH]; int to=-1,r=-1;
    if(join(path,stage,file->name) || fault("create")) goto done;
    to=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600); if(to<0) goto done;
    uint8_t block[1024],hash[32]; struct sha256_ctx ctx; uint32_t bytes=0; sha256_init(&ctx);
    for(;;) { if(fault("read")) goto done; ssize_t n=read(from,block,sizeof(block)); if(n<0) goto done; if(!n) break;
        if((uint32_t)n>file->bytes-bytes || write_all(to,block,(size_t)n)) goto done;
        bytes+=(uint32_t)n; sha256_update(&ctx,block,(size_t)n);
    }
    sha256_final(&ctx,hash);
    if(bytes!=file->bytes || memcmp(hash,file->hash,32) || fault("fsync") || fsync(to)) goto done;
    r=0;
 done:
    if(close(from)) r=-1;
    if(to>=0 && close(to)) r=-1;
    return r;
}
struct read_context { const char *directory; };
static int reader(void *ctx,const char *name,uint32_t offset,void *out,size_t cap,size_t *got)
{
    struct read_context *c=ctx; int fd=source_open(c->directory,name); if(fd<0) return -errno;
    ssize_t n=pread(fd,out,cap,offset); int r=n<0 ? -errno : 0; if(!r) *got=(size_t)n;
    if(close(fd) && !r) r=-errno;
    return r;
}
static int cleanup(const char *dir,const struct ndp_manifest *m)
{
    int r=0; char path[DP_PATH];
    for(unsigned i=0;i<m->file_count;i++) { if(join(path,dir,m->files[i].name)) return -1; if(fault("unlink") || (unlink(path) && errno!=ENOENT)) r=-1; }
    if(join(path,dir,"registry.ndp")) return -1;
    if(fault("unlink") || (unlink(path) && errno!=ENOENT)) r=-1;
    /* Retain the validated inventory if a dependency could not be removed. */
    if(r) return r;
    if(join(path,dir,"transaction.ndp")) return -1;
    if(fault("unlink") || (unlink(path) && errno!=ENOENT)) return -1;
    if(join(path,dir,"manifest.ndp")) return -1;
    if(fault("unlink") || (unlink(path) && errno!=ENOENT)) return -1;
    if(fault("rmdir") || rmdir(dir)) r=-1;
    return r;
}
static int load_record(const char *dir,const char *name,struct ndp_manifest *m)
{
    int fd=source_open(dir,name); if(fd<0) return -errno;
    size_t at=0; int r=-EINVAL;
    while(at<sizeof(*m)) { ssize_t n=read(fd,(uint8_t *)m+at,sizeof(*m)-at); if(n<=0) goto done; at+=(size_t)n; }
    uint8_t extra; if(read(fd,&extra,1) || !ndp_manifest_valid(m)) goto done;
    r=0;
 done: if(close(fd) && !r) r=-errno; return r;
}
static int load_manifest(const char *dir,struct ndp_manifest *m)
{ return load_record(dir,"manifest.ndp",m); }
static int device_package(const struct ndp_device *dev,const char *store,char out[DP_PATH],struct ndp_manifest *m,int removed)
{
    if(root_ok(store)) return -errno;
    DIR *dir=opendir(store); if(!dir) return -errno;
    struct dirent *de; int found=0,r=0; char candidate[DP_PATH];
    while((de=readdir(dir))) {
        size_t len=strlen(de->d_name);
        int tombstone=len>7 && !strcmp(de->d_name+len-7,".remove");
        if(!ndp_component(de->d_name) || strstr(de->d_name,".stage") || tombstone!=removed) continue;
        if(join(candidate,store,de->d_name)) { r=-errno; break; }
        if(load_manifest(candidate,m) || memcmp(&m->device,dev,sizeof(*dev))) continue;
        if(found++) { r=-EEXIST; break; } strcpy(out,candidate);
    }
    if(closedir(dir) && !r) r=-errno;
    if(r) return r;
    if(!found) return -ENOENT;
    return load_manifest(out,m);
}
int dp_installed(const struct ndp_device *dev,const char *store)
{
    struct ndp_manifest *m=malloc(sizeof(*m)); if(!m) return -ENOMEM;
    char path[DP_PATH]; int r=device_package(dev,store,path,m,0); free(m); return r;
}
int dp_install(const struct dp_plan *plan,const char *root,const char *store,char reason[160])
{
    if(!plan) return -EINVAL;
    const struct ndp_manifest *m=&plan->manifest;
    if(!ndp_manifest_valid(m) || strcmp(m->reason,m->enabled?NDP_QUALIFIED:NDP_DISABLED) || root_ok(root) || root_ok(store)) { snprintf(reason,160,"Invalid or unauthorized package plan."); return -EINVAL; }
    for(unsigned i=0;i<m->file_count;i++) if(!memchr(plan->source[i],0,sizeof(plan->source[i])) || !ndp_relative(plan->source[i])) {
        snprintf(reason,160,"Unsafe source path in package plan."); return -EINVAL;
    }
    int exists=dp_installed(&m->device,store);
    if(exists!=-ENOENT) { snprintf(reason,160,"Remove the existing package explicitly before replacement."); return exists ? exists : -EEXIST; }
    char final[DP_PATH],stage[DP_PATH],stage_name[80];
    snprintf(stage_name,sizeof(stage_name),"%s.stage",m->package);
    if(join(final,store,m->package) || join(stage,store,stage_name)) return -errno;
    struct stat existing;
    if(!stat(final,&existing) || errno!=ENOENT) { snprintf(reason,160,"The package directory already exists; no files were replaced."); return -EEXIST; }
    if(!stat(stage,&existing)) {
        struct ndp_manifest *old=malloc(sizeof(*old)); int recovered=old && !load_record(stage,"transaction.ndp",old) &&
            !old->enabled && !memcmp(old->seal,m->seal,32) && !cleanup(stage,old);
        free(old);
        if(!recovered) { snprintf(reason,160,"An incomplete staging directory needs review before retry."); return -EBUSY; }
    }
    if(fault("mkdir") || mkdir(stage,0700)) { snprintf(reason,160,"Staging directory is unavailable; no installed package changed."); return -errno; }
    int r=-EIO;
    if(write_file(stage,"transaction.ndp",m,sizeof(*m))) goto rollback;
    for(unsigned i=0;i<m->file_count;i++) if(copy_file(root,plan->source[i],stage,&m->files[i])) goto rollback;
    if(write_file(stage,"registry.ndp",m->parameters,m->parameter_count*sizeof(m->parameters[0])) ||
       write_file(stage,"manifest.ndp",m,sizeof(*m))) goto rollback;
    struct read_context ctx={stage};
    if(fault("verify") || !ndp_verify(m,&m->device,reader,&ctx)) goto rollback;
    char transaction[DP_PATH];
    if(join(transaction,stage,"transaction.ndp") || fault("transaction-remove") || unlink(transaction) || fault("rename") || rename(stage,final)) goto rollback;
    snprintf(reason,160,"%s",m->reason); return 0;
 rollback:
    if(cleanup(stage,m)) snprintf(reason,160,"Install failed; staging cleanup needs retry. No manifest was committed.");
    else snprintf(reason,160,"Install failed and was rolled back; no installed package changed.");
    return r;
}
int dp_uninstall_checked(const struct ndp_device *dev,const char *store,dp_busy_check busy,void *ctx,char reason[160])
{
    struct ndp_manifest *m=malloc(sizeof(*m)); if(!m) return -ENOMEM;
    char path[DP_PATH],removed[DP_PATH]; int r=device_package(dev,store,path,m,0);
    if(r==-ENOENT) {
        r=device_package(dev,store,path,m,1);
        if(!r && (!m->enabled || (busy && !busy(ctx,dev)))) { r=cleanup(path,m) ? -EIO : 0; snprintf(reason,160,r?"Removal cleanup needs retry.":"Driver package removed."); goto done; }
        if(r==-ENOENT && !root_ok(store)) {
            /* A failed final rmdir can leave only an empty tombstone. Its
             * BDF suffix identifies the removal request; rmdir cannot delete
             * any unexpected files. */
            char suffix[40]; snprintf(suffix,sizeof(suffix),"-%02x-%02x-%x.remove",dev->bus,dev->slot,dev->function);
            DIR *dir=opendir(store); struct dirent *entry; int recovered=0;
            if(dir) {
                while((entry=readdir(dir))) {
                    size_t len=strlen(entry->d_name),n=strlen(suffix);
                    if(ndp_component(entry->d_name) && len>n && !strcmp(entry->d_name+len-n,suffix) &&
                       !join(path,store,entry->d_name) && !rmdir(path)) recovered=1;
                }
                if(closedir(dir)) recovered=0;
            }
            if(recovered) { r=0; snprintf(reason,160,"Driver package removed."); goto done; }
        }
    }
    if(r) { snprintf(reason,160,"No removable package found."); goto done; }
    /* A missing/failed observation cannot prove a qualified host inactive. */
    if(m->enabled && (!busy || busy(ctx,dev))) { r=-EBUSY; snprintf(reason,160,"An active or quarantined package requires a successful host stop first."); goto done; }
    if(snprintf(removed,sizeof(removed),"%s.remove",path)>=(int)sizeof(removed)) { r=-ENAMETOOLONG; goto done; }
    if(fault("remove-rename") || rename(path,removed)) { r=-errno; snprintf(reason,160,"Removal failed; installed package retained."); goto done; }
    r=cleanup(removed,m) ? -EIO : 0;
    snprintf(reason,160,r ? "Package disabled; removal cleanup needs retry." : "Driver package removed. Native driver remains the default.");
 done: free(m); return r;
}
int dp_uninstall(const struct ndp_device *dev,const char *store,char reason[160])
{ return dp_uninstall_checked(dev,store,NULL,NULL,reason); }
