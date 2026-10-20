/* Private registry-dispatch implementation. Notifications are relayed by
 * owning USER threads; desktop alone starts/resets the coordinator.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
struct wr_end_window {struct w32r_window w; unsigned queried,delivered,ended;};
struct wr_end {uint32_t flags,count,at,stage,veto;uint64_t deadline;struct wr_end_window windows[4096];};
static int wr_end_live(struct wr_server *s,uint32_t pid)
{for(unsigned i=0;i<s->live_count;i++)if(s->live[i]==pid)return 1;return 0;}
static void wr_end_advance(struct wr_server *s)
{
    struct wr_end *e=s->ending;if(!e)return;
    while(e->stage<2){
        while(e->at<e->count){
            struct wr_end_window *w=&e->windows[e->at];
            if(!wr_end_live(s,w->w.pid) || (e->stage==1 && !w->queried)){e->at++;continue;}
            if(s->now>=e->deadline){e->at++;continue;}
            return;
        }
        if(!e->stage){e->stage=1;e->at=0;}
        else e->stage=2;
    }
}
static uint32_t wr_end_call(struct wr_server *s,uint32_t pid,const struct w32r_request *a,
                            const char *path,const char *name,const uint8_t *order,struct w32r_reply *r)
{
    if(*path || *name || a->access || (a->index!=W32R_SESSION_BEGIN && a->data_bytes))return 87;
    if(a->index==W32R_SESSION_BEGIN){
        if(pid!=s->compat_writer_pid || !pid)return 5;
        if(a->key || a->type || a->flags&~0x1fu || a->data_bytes%8 || a->data_bytes>128u*8)return 87;
        if(s->ending)return 1115;
        struct wr_end *e=calloc(1,sizeof(*e));if(!e)return 8;
        e->flags=a->flags;e->deadline=s->now+10000;
        uint8_t bytes[24+W32R_NAME];
        for(unsigned i=0;i<4096;i++){
            struct w32r_request q={.op=W32R_WINDOW_ENUM,.index=i};struct w32r_reply v={0};
            uint32_t err=wr_directory_call(s,pid,&q,"",NULL,&v,bytes);
            if(err==259)break;
            if(err){free(e);return err;}
            struct w32r_window w;memcpy(&w,bytes,sizeof(w));
            if(wr_end_live(s,w.pid))e->windows[e->count++].w=w;
        }
        unsigned ranked=0;
        for(unsigned i=0;i<a->data_bytes;i+=8){
            uint32_t hwnd=(uint32_t)order[i]|(uint32_t)order[i+1]<<8|(uint32_t)order[i+2]<<16|(uint32_t)order[i+3]<<24;
            uint32_t owner=(uint32_t)order[i+4]|(uint32_t)order[i+5]<<8|(uint32_t)order[i+6]<<16|(uint32_t)order[i+7]<<24;
            for(unsigned j=ranked;j<e->count;j++)if(e->windows[j].w.hwnd==hwnd && e->windows[j].w.pid==owner){
                struct wr_end_window w=e->windows[j];
                memmove(e->windows+ranked+1,e->windows+ranked,(j-ranked)*sizeof(w));e->windows[ranked++]=w;break;
            }
        }
        s->ending=e;wr_end_advance(s);return 0;
    }
    struct wr_end *e=s->ending;
    if(a->index==W32R_SESSION_STATUS || a->index==W32R_SESSION_RESET){
        if(pid!=s->compat_writer_pid || !pid)return 5;
        if(a->key || a->type || a->flags)return 87;
        if(a->index==W32R_SESSION_RESET){free(e);s->ending=NULL;return 0;}
        wr_end_advance(s);r->key=e&&e->stage==2?(e->veto?2:1):0;return 0;
    }
    if(a->index!=W32R_SESSION_POLL && a->index!=W32R_SESSION_REPLY)return 87;
    if(!a->type || (a->index==W32R_SESSION_POLL && (a->key || a->flags)))return 87;
    wr_end_advance(s);if(!e || e->stage==2)return a->index==W32R_SESSION_POLL?0:6;
    struct wr_end_window *w=&e->windows[e->at];
    if(w->w.pid!=pid || w->w.tid!=a->type)return a->index==W32R_SESSION_POLL?0:5;
    if(a->index==W32R_SESSION_POLL){
        if(w->delivered)return 0;
        w->delivered=1;r->key=w->w.hwnd;r->type=e->stage?0x16:0x11;
        r->count=e->stage?!e->veto:0;r->disposition=(e->flags&0xbu)?0:0x80000000u;return 0;
    }
    if(!w->delivered || a->key!=w->w.hwnd || a->flags>1)return 87;
    if(!e->stage){w->queried=1;if(!a->flags && !(e->flags&4)){e->veto=1;e->at=e->count;}}
    else w->ended=1;
    w->delivered=0;e->at++;wr_end_advance(s);return 0;
}
