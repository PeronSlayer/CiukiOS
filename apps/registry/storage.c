/* Fixed-path native machine roots; never installed in a user w32srv.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "broker.h"
#include "../../sdk/libreg/internal.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
static const char *const files[]={NULL,"/System/Security/Registry/Machine/System.reg",
    "/System/Security/Registry/Machine/Programs.reg"};
static const char *const names[]={NULL,"System","Programs"};
static int read_root(void *context,unsigned root,char **out,size_t *n)
{
    (void)context;if(root!=RB_SYSTEM&&root!=RB_PROGRAMS)return -EACCES;
    if(reg_recover(files[root],names[root])||reg_file_read(files[root],out,n))return -(errno?errno:EIO);
    return 0;
}
static int write_root(void *context,unsigned root,const char *text,size_t n,uint64_t expected)
{
    char *before=NULL;size_t length=0;int e=read_root(context,root,&before,&length);if(e)return e;
    struct rt *t=reg_parse(names[root],before,length);free(before);if(!t)return -EIO;
    e=t->generation!=expected?-EAGAIN:0;reg_tree_free(t);if(e)return e;
    if(expected>=UINT64_MAX-1)return -EOVERFLOW;
    return reg_file_write(files[root],names[root],text,n,expected+1)?-(errno?errno:EIO):0;
}
static int profile_directories(void *context,const struct rb_identity *c)
{
    (void)context;char path[128];if(!memchr(c->profile,0,sizeof(c->profile))||strncmp(c->profile,"/Windows/Profiles/",18))return -EINVAL;
    const char *suffix[]={"/Settings","/Settings/Registry"};
    for(unsigned i=0;i<2;i++){int n=snprintf(path,sizeof(path),"%s%s",c->profile,suffix[i]);if(n<0||(size_t)n>=sizeof(path))return -EOVERFLOW;
        if(mkdir(path,0700)&&errno!=EEXIST)return -errno;}
    return 0;
}
void rb_storage_posix(struct rb_storage *s)
{*s=(struct rb_storage){.read=read_root,.write=write_root,.profile=profile_directories};}
