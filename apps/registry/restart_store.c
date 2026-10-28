/* Durable intent precedes replacement; a marked intent always finishes its
 * exact new generation. No acknowledged journal is rolled back.
 * SPDX-License-Identifier: MIT */
#include "restart_store.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#define INTENT 64u
#define MARK 0x324a5243u
static const char *base(const struct rj_store *s){return s->scope==RJ_MACHINE?"machine.v2":"personal.v2";}
static void names(const struct rj_store *s,char current[32],char next[32],char old[32],char intent[32])
{
    const char *p=base(s);size_t n=strlen(p);memcpy(current,p,n+1);memcpy(next,p,n);memcpy(next+n,".new",5);
    memcpy(old,p,n);memcpy(old+n,".bak",5);memcpy(intent,p,n);memcpy(intent+n,".intent",8);
}
static int remove_file(struct rj_store *s,const char *p){int e=s->io.remove(s->io.context,p);return e==-ENOENT?0:e;}
static int read_file(struct rj_store *s,const char *p,uint8_t **v,size_t *n)
{*v=NULL;*n=0;int e=s->io.read(s->io.context,p,v,n);if(e){free(*v);*v=NULL;*n=0;}return e;}
static int journal(struct rj_store *s,uint8_t *v,size_t n)
{
    if(n<RJ_HEADER||n>RJ_BYTES||memcmp(v,"CIUKIRS2",8)||rb_get32(v+8)!=2||rb_get32(v+12)!=n||
       rb_get32(v+28)!=s->scope||rb_get32(v+32)!=s->uid||rb_get64(v+40)!=s->volume||rb_get32(v+64)>RJ_JOBS||
       n!=RJ_HEADER+(size_t)rb_get32(v+64)*RJ_RECORD)return 0;
    for(unsigned i=0;i<3;i++)if(rb_get32(v+16+i*4)!=s->sid[i])return 0;
    uint32_t crc=rb_get32(v+124);rb_put32(v+124,0);int valid=crc==wr_crc(v,n);rb_put32(v+124,crc);return valid;
}
static int matches(struct rj_store *s,const char *path,const uint8_t *intent,int *present)
{
    uint8_t *v=NULL,digest[32];size_t n=0;int e=read_file(s,path,&v,&n);*present=e!=-ENOENT;
    if(e)return e==-ENOENT?0:e;
    int valid=journal(s,v,n)&&n==rb_get32(intent+12)&&rb_get64(v+48)==rb_get64(intent+16);
    if(valid){sha256(v,n,digest);valid=!memcmp(digest,intent+24,32);}free(v);return valid;
}
static int recover(struct rj_store *s)
{
    char current[32],next[32],old[32],intent[32];names(s,current,next,old,intent);
    uint8_t *marker=NULL;size_t bytes=0;int e=read_file(s,intent,&marker,&bytes);
    if(e&&e!=-ENOENT)return e;
    if(e==-ENOENT||bytes<INTENT||rb_get32(marker+60)!=MARK){
        /* An unmarked/short intent never admitted replacement. */
        free(marker);e=remove_file(s,next);if(!e)e=remove_file(s,intent);if(e)return e;
        uint8_t *v=NULL;size_t n=0;e=read_file(s,current,&v,&n);
        if(!e){int valid=journal(s,v,n);free(v);if(!valid)return -EIO;
            e=remove_file(s,old);if(!e)e=s->io.sync(s->io.context,current);return e;}
        if(e!=-ENOENT)return e;
        e=read_file(s,old,&v,&n);if(e==-ENOENT)return 0;if(e)return e;
        int valid=journal(s,v,n);free(v);if(!valid)return -EIO;
        e=s->io.move(s->io.context,old,current);return e?e:s->io.sync(s->io.context,current);
    }
    if(bytes!=INTENT||memcmp(marker,"CIUKIRJ2",8)||rb_get32(marker+8)!=2||rb_get32(marker+12)<RJ_HEADER||
       rb_get32(marker+12)>RJ_BYTES||!rb_get64(marker+16)||rb_get32(marker+56)!=wr_crc(marker,56)){
        free(marker);return -EIO;
    }
    int have_current=0,have_next=0;int match=matches(s,current,marker,&have_current);
    if(match<0){free(marker);return match;}
    if(!match){
        match=matches(s,next,marker,&have_next);if(match<=0){free(marker);return match?match:-EIO;}
        if(have_current){
            /* A replacement backup must not silently overwrite another receipt. */
            uint8_t *v=NULL;size_t n=0;e=read_file(s,old,&v,&n);free(v);
            if(!e){
                uint8_t *active=NULL,*backup=NULL;size_t an=0,bn=0;
                int ae=read_file(s,current,&active,&an),be=read_file(s,old,&backup,&bn);
                int same=!ae&&!be&&an==bn&&!memcmp(active,backup,an);free(active);free(backup);
                if(!same){free(marker);return -EIO;}e=remove_file(s,old);
                if(e){free(marker);return e;}
            }else if(e!=-ENOENT){free(marker);return e;}
            e=s->io.move(s->io.context,current,old);if(!e)e=s->io.sync(s->io.context,old);
            if(e){free(marker);return e;}
        }
        e=s->io.move(s->io.context,next,current);if(!e)e=s->io.sync(s->io.context,current);
        if(e){free(marker);return e;}
    }
    free(marker);e=remove_file(s,next);if(!e)e=remove_file(s,old);
    if(!e)e=s->io.sync(s->io.context,current);
    if(!e)e=remove_file(s,intent);if(!e)e=s->io.sync(s->io.context,current);return e;
}
int rj_store_init(struct rj_store *s,const struct rj_io *io,unsigned scope,uint32_t uid,const uint32_t sid[3],uint64_t volume)
{
    if(!s||!io||!io->read||!io->write||!io->patch||!io->sync||!io->move||!io->remove||!sid||!volume||
       (scope!=RJ_PERSONAL&&scope!=RJ_MACHINE)||(scope==RJ_MACHINE?uid!=0:!uid))return -EINVAL;
    memset(s,0,sizeof(*s));s->io=*io;s->scope=scope;s->uid=uid;s->volume=volume;memcpy(s->sid,sid,12);return 0;
}
int rj_store_read(void *context,unsigned scope,uint32_t uid,uint8_t **out,size_t *n)
{
    struct rj_store *s=context;if(!s||!out||!n||scope!=s->scope||uid!=s->uid)return -EACCES;
    int e=recover(s);if(e)return e;e=read_file(s,base(s),out,n);if(e)return e;
    if(!journal(s,*out,*n)){free(*out);*out=NULL;*n=0;return -EIO;}return 0;
}
int rj_store_write(void *context,unsigned scope,uint32_t uid,const void *data,size_t n)
{
    struct rj_store *s=context;if(!s||!data||scope!=s->scope||uid!=s->uid)return -EACCES;
    if(n<RJ_HEADER||n>RJ_BYTES)return -EINVAL;
    uint8_t *copy=malloc(n?n:1);if(!copy)return -ENOMEM;memcpy(copy,data,n);
    if(!journal(s,copy,n)||!rb_get64(copy+48)){free(copy);return -EINVAL;}
    int e=recover(s);if(e){free(copy);return e;}
    uint8_t *prior=NULL;size_t old_bytes=0;e=read_file(s,base(s),&prior,&old_bytes);
    if(!e&&(!journal(s,prior,old_bytes)||rb_get64(prior+48)>=rb_get64(copy+48)))e=-EAGAIN;
    free(prior);if(e&&e!=-ENOENT){free(copy);return e;}
    char current[32],next[32],old[32],intent[32];names(s,current,next,old,intent);
    uint8_t marker[INTENT]={0};memcpy(marker,"CIUKIRJ2",8);rb_put32(marker+8,2);rb_put32(marker+12,(uint32_t)n);
    rb_put64(marker+16,rb_get64(copy+48));sha256(copy,n,marker+24);rb_put32(marker+56,wr_crc(marker,56));
    e=s->io.write(s->io.context,next,copy,n);if(!e)e=s->io.sync(s->io.context,next);
    if(!e)e=s->io.write(s->io.context,intent,marker,sizeof(marker));if(!e)e=s->io.sync(s->io.context,intent);
    uint8_t mark[4];rb_put32(mark,MARK);
    if(!e)e=s->io.patch(s->io.context,intent,60,mark,4);if(!e)e=s->io.sync(s->io.context,intent);
    free(copy);return e?e:recover(s);
}
