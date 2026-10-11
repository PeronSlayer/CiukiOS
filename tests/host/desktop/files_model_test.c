/* SPDX-License-Identifier: MIT */
#include "model.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct node { char path[FM_PATH]; struct fm_meta meta; unsigned char data[40000]; int live; };
static struct node nodes[128];
static struct { int node,live; unsigned offset; } fds[16];
static int countdown,operations,metadata_count,short_io;
static int injection(void) { operations++; return countdown>0 && --countdown==0?-EIO:0; }
static int lookup(const char *p) { for (int i=0;i<128;i++) if (nodes[i].live && !strcmp(nodes[i].path,p)) return i; return -1; }
static int create(const char *p,enum fm_type type)
{
    if (lookup(p)>=0) return -EEXIST;
    for (int i=0;i<128;i++) if (!nodes[i].live) { memset(&nodes[i],0,sizeof(nodes[i])); nodes[i].live=1; strcpy(nodes[i].path,p);
        nodes[i].meta=(struct fm_meta){type,0,(uint64_t)i+1,strncmp(p,"/mnt/d",6)?1:2,0666,12345}; return i; }
    return -ENOSPC;
}
static int canon(void *ctx,const char *p,char *out) { (void)ctx; int r=injection(); if (r) return r; int n=lookup(p); if (n<0) return -ENOENT; if (nodes[n].meta.type!=FM_DIR) return -ENOTDIR; strcpy(out,p); return 0; }
static int stat_node(void *ctx,const char *p,struct fm_meta *m) { (void)ctx; int r=injection(); if (r) return r; int n=lookup(p); if (n<0) return -ENOENT; *m=nodes[n].meta; return 0; }
static int list(void *ctx,const char *p,fm_emit emit,void *arg)
{
    (void)ctx; int r=injection(); if (r) return r; int n=lookup(p); if (n<0) return -ENOENT;
    if (nodes[n].meta.type!=FM_DIR) return -ENOTDIR;
    for (int i=0;i<128;i++) if (nodes[i].live && strcmp(nodes[i].path,p) && fm_beneath(nodes[i].path,p)) {
        const char *name=nodes[i].path+strlen(p); if (*name=='/') name++; if (!strchr(name,'/')) { r=emit(name,&nodes[i].meta,arg); if (r) return r; }
    }
    return 0;
}
static int mkdir_node(void *ctx,const char *p) { (void)ctx; int r=injection(); if (r) return r; int n=create(p,FM_DIR); return n<0?n:0; }
static int rename_node(void *ctx,const char *a,const char *b)
{
    (void)ctx; int r=injection(); if (r) return r; int source=lookup(a),target=lookup(b);
    if (source<0) return -ENOENT;
    unsigned volume=strncmp(b,"/mnt/d",6)?1:2; if (nodes[source].meta.dev!=volume) return -EXDEV;
    if (fm_beneath(b,a) && strcmp(a,b)) return -EINVAL;
    if (source==target) return 0;
    if (target>=0) nodes[target].live=0;
    for (int i=0;i<128;i++) if (nodes[i].live && fm_beneath(nodes[i].path,a)) {
        char path[FM_PATH]; snprintf(path,sizeof(path),"%s%s",b,nodes[i].path+strlen(a)); strcpy(nodes[i].path,path);
    }
    return 0;
}
static int unlink_node(void *ctx,const char *p) { (void)ctx; int r=injection(); if (r) return r; int n=lookup(p); if (n<0) return -ENOENT; if (nodes[n].meta.type!=FM_FILE) return -EISDIR; nodes[n].live=0; return 0; }
static int rmdir_node(void *ctx,const char *p)
{ (void)ctx; int r=injection(); if (r) return r; int n=lookup(p); if (n<0) return -ENOENT; for (int i=0;i<128;i++) if (i!=n && nodes[i].live && fm_beneath(nodes[i].path,p)) return -ENOTEMPTY; nodes[n].live=0; return 0; }
static int open_node(void *ctx,const char *p,int make)
{
    (void)ctx; int r=injection(); if (r) return r; int n=make?create(p,FM_FILE):lookup(p); if (n<0) return make?n:-ENOENT;
    for (int fd=0;fd<16;fd++) if (!fds[fd].live) { fds[fd].node=n; fds[fd].live=1; fds[fd].offset=0; return fd; } return -EMFILE;
}
static int read_node(void *ctx,int fd,void *p,unsigned size)
{
    (void)ctx; int r=injection(); if (r) return r; assert(fds[fd].live); struct node *n=&nodes[fds[fd].node];
    unsigned count=(unsigned)n->meta.size-fds[fd].offset; if (count>size) count=size; if (short_io && count>113) count=113;
    memcpy(p,n->data+fds[fd].offset,count); fds[fd].offset+=count; return (int)count;
}
static int write_node(void *ctx,int fd,const void *p,unsigned size)
{
    (void)ctx; int r=injection(); if (r) return r; assert(fds[fd].live); struct node *n=&nodes[fds[fd].node];
    if (short_io && size>71) size=71; assert(fds[fd].offset+size<=sizeof(n->data));
    memcpy(n->data+fds[fd].offset,p,size); fds[fd].offset+=size; n->meta.size=fds[fd].offset; return (int)size;
}
static int sync_node(void *ctx,int fd) { (void)ctx; assert(fds[fd].live); return injection(); }
static int close_node(void *ctx,int fd) { (void)ctx; assert(fds[fd].live); fds[fd].live=0; return injection(); }
static int metadata_node(void *ctx,const char *p,const struct fm_meta *m) { (void)ctx; int n=lookup(p); assert(n>=0); nodes[n].meta.mode=m->mode; nodes[n].meta.modified=m->modified; metadata_count++; return 0; }
static struct fm_vfs fs={NULL,canon,list,stat_node,mkdir_node,rename_node,unlink_node,rmdir_node,open_node,read_node,write_node,sync_node,close_node,metadata_node};
static void fixture(void)
{
    memset(nodes,0,sizeof(nodes)); memset(fds,0,sizeof(fds)); countdown=operations=metadata_count=0; short_io=0;
    create("/",FM_DIR); create("/system",FM_DIR); create("/system/sub",FM_DIR); create("/home",FM_DIR); create("/mnt",FM_DIR); create("/mnt/d",FM_DIR);
    int n=create("/system/readme.txt",FM_FILE); nodes[n].meta.size=33001;
    for (unsigned i=0;i<nodes[n].meta.size;i++) nodes[n].data[i]=(unsigned char)(i%251);
    n=create("/system/sub/a.txt",FM_FILE); nodes[n].meta.size=17; memset(nodes[n].data,42,17);
    create("/system/Z.txt",FM_FILE);
}
static void clean(void)
{ for (int fd=0;fd<16;fd++) assert(!fds[fd].live); for (int i=0;i<128;i++) if (nodes[i].live) assert(!strstr(nodes[i].path,".ciuki-copy")); }
static void finish(struct fm_job *j)
{
    for (unsigned i=0;i<10000 && j->state==FM_RUNNING;i++) fm_job_step(j);
    assert(j->state!=FM_RUNNING);
}
static void same(const char *a,const char *b)
{ int x=lookup(a),y=lookup(b); assert(x>=0 && y>=0); assert(nodes[x].meta.size==nodes[y].meta.size); assert(!memcmp(nodes[x].data,nodes[y].data,(size_t)nodes[x].meta.size)); }
int main(void)
{
    fixture(); struct fm_model m; fm_init(&m,&fs); assert(!fm_navigate(&m,"/")); assert(!fm_navigate(&m,"/system"));
    assert(m.count==3 && !strcmp(m.entries[0].name,"sub"));
    assert(fm_navigate(&m,"/missing")==-ENOENT && !strcmp(m.path,"/system"));
    fm_select(&m,1,0); fm_select(&m,2,1); assert(m.entries[1].selected && m.entries[2].selected && !m.entries[0].selected);
    fm_select(&m,1,2); assert(!m.entries[1].selected); fm_select_all(&m,1); assert(m.entries[0].selected && m.entries[1].selected && !m.entries[2].selected);
    fm_sort(&m,1,1); assert(!strcmp(m.entries[0].name,"sub") && !strcmp(m.entries[1].name,"readme.txt"));
    assert(fm_find(&m,'z',1)==2); assert(!fm_clipboard(&m,0) && m.clipboard_count==1);
    assert(!fm_history(&m,-1) && !strcmp(m.path,"/")); assert(!fm_history(&m,1)); assert(!fm_up(&m));
    assert(!fm_navigate(&m,"/home")); assert(!fm_create(&m,"New Folder",1)); assert(!fm_rename(&m,m.active,"Renamed")); assert(lookup("/home/Renamed")>=0);
    assert(fm_create(&m,"bad/name",1)==-EINVAL); assert(fm_rename(&m,m.active,"..") == -EINVAL);
    fm_destroy(&m);
    struct fm_job j; char *paths[]={"/system"};
    fixture(); short_io=1; assert(!fm_job_start(&j,&fs,FM_COPY,paths,1,"/home")); finish(&j); assert(j.state==FM_DONE && j.metadata_warnings==0 && metadata_count==5);
    same("/system/readme.txt","/home/system/readme.txt"); same("/system/sub/a.txt","/home/system/sub/a.txt"); fm_job_destroy(&j); clean();
    fixture(); assert(fm_job_start(&j,&fs,FM_COPY,paths,1,"/system/sub")==-EINVAL); clean();
    char *file[]={"/system/readme.txt"};
    fixture(); assert(!fm_job_start(&j,&fs,FM_COPY,file,1,"/home")); fm_job_step(&j); fm_job_step(&j); fm_job_cancel(&j); finish(&j); assert(j.state==FM_CANCELLED && lookup(file[0])>=0 && lookup("/home/readme.txt")<0); fm_job_destroy(&j); clean();
    fixture(); create("/home/readme.txt",FM_FILE); int old=lookup("/home/readme.txt"); nodes[old].meta.size=3; memcpy(nodes[old].data,"old",3);
    assert(!fm_job_start(&j,&fs,FM_COPY,file,1,"/home")); finish(&j); assert(j.state==FM_REPLACE); fm_job_answer(&j,FM_NO); finish(&j);
    assert(j.state==FM_DONE && j.skipped==1 && nodes[old].meta.size==3); fm_job_destroy(&j); clean();
    assert(!fm_job_start(&j,&fs,FM_COPY,file,1,"/home")); finish(&j); fm_job_answer(&j,FM_YES_ALL); finish(&j); assert(j.state==FM_DONE); same(file[0],"/home/readme.txt"); fm_job_destroy(&j); clean();
    fixture(); assert(!fm_job_start(&j,&fs,FM_MOVE,paths,1,"/mnt/d")); finish(&j); assert(j.state==FM_DONE && lookup("/system")<0 && lookup("/mnt/d/system/sub/a.txt")>=0); fm_job_destroy(&j); clean();
    char *deleted[]={"/mnt/d/system"}; assert(!fm_job_start(&j,&fs,FM_DELETE,deleted,1,NULL)); finish(&j); assert(j.state==FM_DONE && lookup(deleted[0])<0); fm_job_destroy(&j); clean();
    fixture(); assert(!fm_job_start(&j,&fs,FM_PROPERTIES,paths,1,NULL)); finish(&j); assert(j.bytes==33018 && j.files==3 && j.folders==2); fm_job_destroy(&j); clean();
    fixture(); assert(!fm_job_start(&j,&fs,FM_COPY,file,1,"/home")); finish(&j); int stages=operations; fm_job_destroy(&j);
    for (int failure=1;failure<=stages;failure++) {
        fixture(); countdown=failure; assert(!fm_job_start(&j,&fs,FM_COPY,file,1,"/home")); finish(&j);
        assert(j.state==FM_FAILED || j.state==FM_DONE); assert(lookup(file[0])>=0);
        countdown=0; fm_job_destroy(&j); clean();
        int n=lookup(file[0]); assert(nodes[n].meta.size==33001); for (unsigned i=0;i<33001;i++) assert(nodes[n].data[i]==i%251);
    }
    fixture(); struct fm_vfs no_meta=fs; no_meta.metadata=NULL; assert(!fm_job_start(&j,&no_meta,FM_COPY,file,1,"/home")); finish(&j); assert(j.state==FM_DONE && j.metadata_warnings==1); fm_job_destroy(&j); clean();
    puts("PASS Files model: history, selection, sorting, recursive jobs, short I/O, replacement, Cancel, EXDEV, metadata, injected errors");
    return 0;
}
