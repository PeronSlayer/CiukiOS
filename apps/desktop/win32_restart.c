/* Desktop remains responsive while USER threads query their windows and
 * w32srv advances WININIT. Each tick performs a bounded piece of work.
 * SPDX-License-Identifier: MIT */
#include "win32_restart.h"
#include <string.h>
#include <errno.h>
static uint32_t call(const struct desk_restart_ops *o,unsigned op,unsigned action,uint32_t flags,
                     struct w32r_reply *r,void *bytes,size_t cap)
{memset(r,0,sizeof(*r));return o->call(o->ctx,op,action,flags,r,bytes,cap);}
static void record(const struct desk_restart_ops *o,const char *event,uint32_t n)
{if(o->record)o->record(o->ctx,event,n);}
static void finish(struct desk_restart *s,const struct desk_restart_ops *o,int error)
{
    struct w32r_reply r;call(o,W32R_ENDSESSION,W32R_SESSION_RESET,0,&r,NULL,0);
    record(o,error?"[win32] restart failed":"[win32] restart complete",(uint32_t)error);
    s->stage=0;s->error=error;
    if(o->notice)o->notice(o->ctx,3,s->pid,s->flags);
}
int desk_restart_begin(struct desk_restart *s,const struct desk_restart_ops *o,uint32_t pid,uint32_t flags,uint64_t now)
{
    if(s->stage)return -EBUSY;
    if(flags&~0x1fu)return -EINVAL;
    struct w32r_reply r;uint8_t bytes[256];
    uint32_t e=call(o,W32R_SHUTDOWN,W32R_SHUTDOWN_LIST,0,&r,bytes,sizeof(bytes));
    if(e || r.count>64 || r.data_bytes!=r.count*4)return -EIO;
    s->count=r.count;
    for(unsigned i=0;i<r.count;i++)s->pids[i]=(uint32_t)bytes[i*4]|(uint32_t)bytes[i*4+1]<<8|(uint32_t)bytes[i*4+2]<<16|(uint32_t)bytes[i*4+3]<<24;
    e=call(o,W32R_ENDSESSION,W32R_SESSION_BEGIN,flags,&r,NULL,0);
    if(e)return -EIO;
    s->pid=pid;s->flags=flags;s->stage=1;s->error=0;s->child=0;s->deadline=now+10000;s->next_poll=0;
    record(o,"[win32] end-session begin",flags);
    if(o->notice)o->notice(o->ctx,2,pid,flags);
    return 0;
}
void desk_restart_idle_result(struct desk_restart *s,const struct desk_restart_ops *o,const struct w32r_reply *r,uint64_t now)
{
    if(s->stage)return;
    if(r->data_bytes){
        if(r->type&9u)desk_restart_begin(s,o,r->key,r->type,now);
        else if(o->notice)o->notice(o->ctx,1,r->key,r->type);
    }else if(!s->boot_checked){
        s->boot_checked=1;
        if(r->disposition)desk_restart_begin(s,o,0,2,now);
    }
}
void desk_restart_tick(struct desk_restart *s,const struct desk_restart_ops *o,uint64_t now)
{
    struct w32r_reply r;uint32_t e;
    /* Idle RPC belongs to the persistent native worker. Only an active
     * restart may perform service work here, at the existing 20 ms cadence. */
    if(!s->stage || now<s->next_poll)return;s->next_poll=now+20;
    if(s->stage==1){
        e=call(o,W32R_ENDSESSION,W32R_SESSION_STATUS,0,&r,NULL,0);
        if(e){finish(s,o,(int)e);return;}
        if(r.key==2){record(o,"[win32] end-session cancelled",s->pid);finish(s,o,0);return;}
        if(r.key==1 || now>=s->deadline)s->stage=2;
        else return;
    }
    if(s->stage==2){
        unsigned live=0;
        for(unsigned i=0;i<s->count;i++)if(s->pids[i] && o->alive(o->ctx,s->pids[i])){
            live++;
            if(now>=s->deadline){
                if(!o->terminate(o->ctx,s->pids[i]))record(o,"[win32] exit reason=restart",s->pids[i]);
            }
        }
        if(live)return;
        if(s->flags&9u){
            e=call(o,W32R_FLUSH,0,0,&r,NULL,0);
            if(!e && o->notice)o->notice(o->ctx,4,s->pid,s->flags);
            finish(s,o,(int)e);return;
        }
        e=call(o,W32R_ENDSESSION,W32R_SESSION_RESET,0,&r,NULL,0);
        if(e){finish(s,o,(int)e);return;}
        s->stage=3;
    }
    if(s->stage==3){
        e=call(o,W32R_SHUTDOWN,W32R_SHUTDOWN_BEGIN,s->flags,&r,NULL,0);
        if(e){finish(s,o,(int)e);return;}s->stage=4;return;
    }
    if(s->stage==5){
        if(o->alive(o->ctx,s->child)){
            if(now>=s->deadline){o->terminate(o->ctx,s->child);record(o,"[win32] restart runonce watchdog",s->child);}
            return;
        }
        s->stage=4;
    }
    if(s->stage==4){
        char command[2048];e=call(o,W32R_SHUTDOWN,W32R_SHUTDOWN_NEXT,0,&r,command,sizeof(command));
        if(e==170)return;
        if(e==259){s->stage=6;return;}
        if(e){finish(s,o,(int)e);return;}
        if(!r.data_bytes || r.data_bytes>sizeof(command) || command[r.data_bytes-1] || memchr(command,0,r.data_bytes-1)){
            finish(s,o,13);return;
        }
        memcpy(s->command,command,r.data_bytes);s->stage=7;s->deadline=now+300000;
    }
    if(s->stage==7){
        int pid=o->launch(o->ctx,s->command);
        if(pid==-EAGAIN)return;
        if(pid>0){s->child=(uint32_t)pid;s->stage=5;}
        else {record(o,"[win32] restart runonce launch-failed",(uint32_t)-pid);s->stage=4;}
        return;
    }
    if(s->stage==6){
        e=call(o,W32R_SHUTDOWN,W32R_SHUTDOWN_END,0,&r,NULL,0);
        if(e==170){s->stage=4;return;}finish(s,o,(int)e);
    }
}
