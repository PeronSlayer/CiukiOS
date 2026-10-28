/* SYSTEM-only account/profile reader over the production ACCOUNTS transport.
 * No verifier or caller-provided profile path enters a registry operation.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "broker.h"
#include "../../sdk/libreg/internal.h"
#include "../../sdk/libreg/transport.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int machine(struct rb_identity *out)
{
    FILE *f=fopen("/System/Security/machine.sid","rb");if(!f)return -errno;
    uint8_t bytes[65],digest[32];size_t n=fread(bytes,1,sizeof(bytes),f);int e=ferror(f)?-EIO:0;
    if(fclose(f)&&!e)e=-EIO;if(e)return e;
    if(n!=64||memcmp(bytes,"CIUKISID",8)||rb_get32(bytes+20))return -EIO;
    sha256(bytes,32,digest);if(memcmp(digest,bytes+32,32))return -EIO;
    for(unsigned i=0;i<3;i++)out->machine_sid[i]=rb_get32(bytes+8+i*4);
    out->volume=rb_get64(bytes+24);return out->volume?0:-EIO;
}
int rb_installation_identity(struct rb_identity *out){memset(out,0,sizeof(*out));return machine(out);}
int rb_account_current(void *unused,uint32_t uid,struct rb_identity *out,int *enabled)
{
    (void)unused;if(!out||!enabled)return -EACCES;
    if(!uid){memset(out,0,sizeof(*out));*enabled=1;return machine(out);}
    uint8_t request[312]={0},reply[272];size_t n=0;struct reg_remote link={.fd=-1,.service=2};
    rb_put32(request,1);rb_put32(request+36,uid);
    int e=reg_registry_rpc(&link,1,request,sizeof(request),reply,sizeof(reply),&n)?-errno:0;
    if(!e&&(n!=160||rb_get32(reply)!=1||rb_get32(reply+4)!=1||rb_get32(reply+16)!=uid||
        rb_get32(reply+28)||!memchr(reply+32,0,32)))e=-EIO;
    if(!e){
        memset(out,0,sizeof(*out));out->uid=uid;out->account_generation=rb_get64(reply+8);
        uint32_t groups=rb_get32(reply+20),flags=rb_get32(reply+24);*enabled=!!(flags&1u);
        if(groups&1u)out->groups[out->group_count++]=544;
        if(groups&2u)out->groups[out->group_count++]=545;
        if(groups&4u)out->groups[out->group_count++]=546;
        if(!groups||(groups&~7u))e=-EIO;else memcpy(out->name,reply+32,32);
    }
    if(!e){
        rb_put64(request+40,out->account_generation);
        e=reg_registry_rpc(&link,8,request,sizeof(request),reply,sizeof(reply),&n)?-errno:0;
        if(!e&&(n!=272||rb_get32(reply)!=1||rb_get32(reply+4)!=272||rb_get32(reply+8)!=uid||
            rb_get32(reply+12)||rb_get64(reply+16)!=out->account_generation||!memchr(reply+32,0,64)))e=-EIO;
        if(!e)memcpy(out->profile,reply+32,64);
    }
    reg_remote_close(&link);if(!e)e=machine(out);return e;
}

int rb_account_users(void *unused,const struct rb_identity *caller,char **out,size_t *n)
{
    struct rb_identity c={0};int enabled=0;int e=rb_account_current(unused,caller->uid,&c,&enabled);if(e)return e;
    struct rt *t=reg_tree_new("Users");if(!t)return -ENOMEM;t->generation=c.account_generation;
    char key[64];snprintf(key,sizeof(key),"Users\\Accounts\\u%08x",c.uid);int32_t uid=(int32_t)c.uid;
    int32_t active=enabled;
    if(reg_put(t,key,"name",REG_STRING,c.name,strlen(c.name)+1)||reg_put(t,key,"uid",REG_INT32,&uid,4)||
       reg_put(t,key,"enabled",REG_BOOL,&active,4))e=-(errno?errno:EIO);
    if(!e){*out=reg_serialise(t,NULL,n);if(!*out)e=-(errno?errno:EIO);}reg_tree_free(t);return e;
}
