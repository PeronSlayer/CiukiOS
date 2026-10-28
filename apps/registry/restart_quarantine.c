/* Preserve exact unowned inputs before publishing a consumed marker. Never
 * turn .DEFAULT/WININIT/v1 bytes into an interactive user's queued work.
 * SPDX-License-Identifier: MIT */
#include "restart_store.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define QMARK 0x32514343u
static int evidence(struct rj_quarantine *q,const char *path,const uint8_t *data,size_t n)
{
    uint8_t *old=NULL;size_t bytes=0;int e=q->evidence.read(q->evidence.context,path,&old,&bytes);
    if(!e){int same=bytes==n&&!memcmp(old,data,n);free(old);
        /* A prior flush may have failed without a process restart. Existing
         * bytes alone never prove the evidence is durable. */
        if(same)return q->evidence.sync(q->evidence.context,path);}
    else{free(old);if(e!=-ENOENT)return e;}
    e=q->evidence.write(q->evidence.context,path,data,n);
    return e?e:q->evidence.sync(q->evidence.context,path);
}
int rj_quarantine_run(struct rj_quarantine *q)
{
    if(!q||!q->source||!q->volume||!q->evidence.read||!q->evidence.write||!q->evidence.patch||!q->evidence.sync)return -EINVAL;
    for(unsigned index=0;index<RJ_LEGACY_COUNT;index++){
        uint8_t *data=NULL;size_t n=0;int e=q->source(q->source_context,index,&data,&n);
        if(e==-ENOENT){free(data);continue;}if(e){free(data);return e;}
        if(n>W32R_TEXT+40u){free(data);return -EFBIG;}
        uint8_t digest[32];char hex[65],path[112],done[112];sha256(data,n,digest);sha256_hex(digest,hex);
        snprintf(path,sizeof(path),"unowned-%u-%s.bin",index,hex);snprintf(done,sizeof(done),"unowned-%u-%s.consumed",index,hex);
        e=evidence(q,path,data,n);free(data);if(e)return e;
        uint8_t marker[96]={0};memcpy(marker,"CIUKIQV2",8);rb_put32(marker+8,2);rb_put32(marker+12,index);
        for(unsigned i=0;i<3;i++)rb_put32(marker+16+i*4,q->sid[i]);rb_put64(marker+32,q->volume);
        rb_put64(marker+40,n);memcpy(marker+48,digest,32);rb_put32(marker+88,wr_crc(marker,88));rb_put32(marker+92,QMARK);
        uint8_t *prior=NULL;size_t bytes=0;e=q->evidence.read(q->evidence.context,done,&prior,&bytes);
        if(!e&&bytes==sizeof(marker)&&!memcmp(prior,marker,sizeof(marker))){free(prior);
            e=q->evidence.sync(q->evidence.context,done);if(e)return e;continue;}
        if(!e&&bytes==96&&rb_get32(prior+92)==QMARK&&rb_get32(prior+88)==wr_crc(prior,88)&&
           (memcmp(prior+16,marker+16,12)||rb_get64(prior+32)!=q->volume)){free(prior);return -EACCES;}
        if(e&&e!=-ENOENT){free(prior);return e;}free(prior);
        rb_put32(marker+92,0);e=q->evidence.write(q->evidence.context,done,marker,sizeof(marker));
        if(!e)e=q->evidence.sync(q->evidence.context,done);uint8_t mark[4];rb_put32(mark,QMARK);
        if(!e)e=q->evidence.patch(q->evidence.context,done,92,mark,4);
        if(!e)e=q->evidence.sync(q->evidence.context,done);if(e)return e;
    }
    return 0;
}
