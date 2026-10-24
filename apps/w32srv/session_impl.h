/* Private registry-dispatch implementation. Notifications are relayed by
 * owning USER threads; desktop alone starts/resets the coordinator.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
struct wr_end_window {struct w32r_window w; unsigned queried,delivered,ended;};
/* G3: a HOLD session (the desktop's Shut Down, Restart, Log Off) has no
 * deadline: the desktop watches the window in turn for hangs and ends a
 * hung process (its windows are then skipped). After every window agreed it
 * waits for COMMIT (WM_ENDSESSION TRUE) or CANCEL (FALSE to the queried). */
struct wr_end {uint32_t flags,count,at,stage,veto,hold,decided;uint64_t deadline;struct wr_end_window windows[4096];};
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
        if(!e->stage){
            if(e->hold && !e->decided && !e->veto)return;   /* held: every window agreed */
            e->stage=1;e->at=0;
        }
        else e->stage=2;
    }
}
/* G11: one WM_POWERBROADCAST, every top-level window of a live client in
 * turn; a query (PBT 0/1) stops at the first BROADCAST_QUERY_DENY. */
struct wr_power {uint32_t event,lparam,count,at,granted,denied;struct wr_end_window windows[4096];};
static void wr_power_advance(struct wr_server *s)
{
    struct wr_power *p=s->power;if(!p)return;
    while(p->at<p->count && !wr_end_live(s,p->windows[p->at].w.pid))p->at++;
}
static uint32_t wr_power_call(struct wr_server *s,uint32_t pid,const struct w32r_request *a,struct w32r_reply *r)
{
    if(pid!=s->compat_writer_pid || !pid)return 5;
    if(a->key || a->data_bytes)return 87;
    if(a->index==W32R_POWER_RESET){if(a->type||a->flags)return 87;free(s->power);s->power=NULL;return 0;}
    if(a->index==W32R_POWER_STATUS){
        if(a->type||a->flags)return 87;
        struct wr_power *p=s->power;wr_power_advance(s);
        r->key=!p||p->at>=p->count?(p&&p->denied?2:1):0;r->count=p?p->granted:0;return 0;
    }
    if(a->type>0x12)return 87;   /* the PBT_* range of Windows 98 */
    if(s->power){wr_power_advance(s);if(s->power->at<s->power->count)return 1115;free(s->power);s->power=NULL;}
    struct wr_power *p=calloc(1,sizeof(*p));if(!p)return 8;
    p->event=a->type;p->lparam=a->flags;
    uint8_t bytes[24+W32R_NAME];
    for(unsigned i=0;i<4096;i++){
        struct w32r_request q={.op=W32R_WINDOW_ENUM,.index=i};struct w32r_reply v={0};
        uint32_t err=wr_directory_call(s,pid,&q,"",NULL,&v,bytes);
        if(err==259)break;
        if(err){free(p);return err;}
        struct w32r_window w;memcpy(&w,bytes,sizeof(w));
        if(wr_end_live(s,w.pid))p->windows[p->count++].w=w;
    }
    s->power=p;wr_power_advance(s);return 0;
}
/* POLL/REPLY of a USER thread while no end of session is pending. */
static uint32_t wr_power_turn(struct wr_server *s,uint32_t pid,const struct w32r_request *a,struct w32r_reply *r)
{
    struct wr_power *p=s->power;
    if(p)wr_power_advance(s);
    if(!p || p->at>=p->count)return a->index==W32R_SESSION_POLL?0:6;
    struct wr_end_window *w=&p->windows[p->at];
    if(w->w.pid!=pid || w->w.tid!=a->type)return a->index==W32R_SESSION_POLL?0:5;
    if(a->index==W32R_SESSION_POLL){
        if(w->delivered)return 0;
        w->delivered=1;r->key=w->w.hwnd;r->type=W32R_POWERBROADCAST;r->count=p->event;r->disposition=p->lparam;return 0;
    }
    if(!w->delivered || a->key!=w->w.hwnd || a->flags>1)return 87;
    w->delivered=0;
    if(!a->flags && p->event<=1){p->denied=1;p->at=p->count;return 0;}
    if(a->flags)p->granted++;
    p->at++;wr_power_advance(s);return 0;
}
static uint32_t wr_end_call(struct wr_server *s,uint32_t pid,const struct w32r_request *a,
                            const char *path,const char *name,const uint8_t *order,struct w32r_reply *r)
{
    if(*path || *name || a->access || (a->index!=W32R_SESSION_BEGIN && a->data_bytes))return 87;
    if(a->index>=W32R_POWER_BEGIN && a->index<=W32R_POWER_RESET)return wr_power_call(s,pid,a,r);
    if(a->index==W32R_SESSION_BEGIN){
        if(pid!=s->compat_writer_pid || !pid)return 5;
        if(a->key || a->type || a->flags&~(0x1fu|W32R_SESSION_HOLD) || a->data_bytes%8 || a->data_bytes>128u*8)return 87;
        if(s->ending)return 1115;
        struct wr_end *e=calloc(1,sizeof(*e));if(!e)return 8;
        e->flags=a->flags&0x1fu;e->hold=!!(a->flags&W32R_SESSION_HOLD);
        e->deadline=e->hold?UINT64_MAX:s->now+10000;
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
    if(a->index==W32R_SESSION_STATUS || a->index==W32R_SESSION_RESET ||
       a->index==W32R_SESSION_CANCEL || a->index==W32R_SESSION_COMMIT){
        if(pid!=s->compat_writer_pid || !pid)return 5;
        if(a->key || a->type || a->flags)return 87;
        if(a->index==W32R_SESSION_RESET){free(e);s->ending=NULL;return 0;}
        if(a->index==W32R_SESSION_CANCEL){
            /* Another program vetoed or the user cancelled: the windows that
             * agreed get WM_ENDSESSION(FALSE). */
            if(e && !e->stage){e->veto=1;e->decided=1;e->at=e->count;}
            wr_end_advance(s);return 0;
        }
        if(a->index==W32R_SESSION_COMMIT){
            if(!e || !e->hold || e->stage || e->veto || e->at<e->count)return 87;
            e->decided=1;wr_end_advance(s);return 0;
        }
        wr_end_advance(s);
        if(e && e->stage==2)r->key=e->veto?2:1;
        else if(e && e->hold && !e->stage && !e->veto && e->at>=e->count)r->key=3;
        else r->key=0;
        if(e && !e->stage && e->at<e->count){   /* the window in turn, for the desktop's hung check */
            const struct wr_end_window *w=&e->windows[e->at];
            r->count=w->w.pid;r->type=w->w.hwnd;r->disposition=w->delivered;
        }
        return 0;
    }
    if(a->index!=W32R_SESSION_POLL && a->index!=W32R_SESSION_REPLY)return 87;
    if(!a->type || (a->index==W32R_SESSION_POLL && (a->key || a->flags)))return 87;
    wr_end_advance(s);if(!e || e->stage==2)return wr_power_turn(s,pid,a,r);   /* G11: no session pending */
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
