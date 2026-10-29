/* Private registry-dispatch implementation. Notifications are relayed by
 * owning USER threads; desktop alone starts/resets the coordinator.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
struct wr_end_window {struct w32r_window w; unsigned queried,delivered,ended,notified;};
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
struct wr_power {uint32_t event,lparam,count,at,granted,denied;uint64_t deadline;struct wr_end_window windows[4096];};
static void wr_power_retire(struct wr_server *s)
{
    if(s->power && s->power->event<=1) {free(s->power_query);s->power_query=s->power;}
    else free(s->power);
    s->power=NULL;
}
static void wr_power_advance(struct wr_server *s)
{
    struct wr_power *p=s->power;if(!p)return;
    while(p->at<p->count) {
        struct wr_end_window *w=&p->windows[p->at];
        if(!wr_end_live(s,w->w.pid)) {p->at++;p->deadline=0;continue;}
        if(!p->deadline) p->deadline=s->now+(p->event<=1?20000:2000);
        if(s->now>=p->deadline && (!w->delivered || p->event>1)) {
            w->queried=1;p->granted++;p->at++;p->deadline=0;continue;
        }
        break;
    }
}
static uint32_t wr_power_call(struct wr_server *s,uint32_t pid,const struct w32r_request *a,struct w32r_reply *r)
{
    if(pid!=s->compat_writer_pid || !pid)return 5;
    if(a->key || a->data_bytes)return 87;
    if(a->index==W32R_POWER_RESET){if(a->type||a->flags)return 87;
        wr_power_retire(s);return 0;}
    if(a->index==W32R_POWER_STATUS){
        if(a->type||a->flags)return 87;
        struct wr_power *p=s->power;wr_power_advance(s);
        r->key=!p||p->at>=p->count?(p&&p->denied?2:1):0;r->count=p?p->granted:0;return 0;
    }
    if(a->type>0x12)return 87;   /* the PBT_* range of Windows 98 */
    if(s->power){wr_power_advance(s);if(s->power->at<s->power->count)return 1115;wr_power_retire(s);}
    struct wr_power *p=calloc(1,sizeof(*p));if(!p)return 8;
    p->event=a->type;p->lparam=a->flags;
    if(a->type==2 && s->power_query) {
        for(unsigned i=0;i<s->power_query->count;i++)
            if(s->power_query->windows[i].queried && wr_end_live(s,s->power_query->windows[i].w.pid))
                p->windows[p->count++].w=s->power_query->windows[i].w;
        s->power=p;wr_power_advance(s);return 0;
    }
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
    w->delivered=0;w->notified=0;
    if(!a->flags && p->event<=1){p->denied=1;p->at=p->count;return 0;}
    if(a->flags){p->granted++;w->queried=1;}
    p->deadline=0;
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
    if(e->at>=e->count)return a->index==W32R_SESSION_POLL?0:6;
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
    w->delivered=0;w->notified=0;e->at++;wr_end_advance(s);return 0;
}

/* Notify the next undelivered turn. The kernel latch coalesces notifications
 * until its owning USER thread consumes one; unrelated threads do no RPC. */
void wr_session_wake(struct wr_server *s)
{
    if (!s->session_notify) return;
    wr_end_advance(s);
    struct wr_end *e=s->ending;
    if (e && e->stage<2) {
        /* A USER thread can consume its power latch while this session
         * owns POLL. The undelivered power turn needs a fresh notification
         * once the session releases priority, even if its old latch was
         * already consumed. Delivered callbacks keep their existing state. */
        struct wr_power *p=s->power;
        if (p && p->at<p->count && !p->windows[p->at].delivered)
            p->windows[p->at].notified=0;
        if (e->at<e->count && !e->windows[e->at].delivered && !e->windows[e->at].notified) {
            e->windows[e->at].notified=1;
            s->session_notify(e->windows[e->at].w.pid,e->windows[e->at].w.tid);
        }
        return;
    }
    wr_power_advance(s);
    struct wr_power *p=s->power;
    if (p && p->at<p->count && !p->windows[p->at].delivered && !p->windows[p->at].notified) {
        p->windows[p->at].notified=1;
        s->session_notify(p->windows[p->at].w.pid,p->windows[p->at].w.tid);
    }
}
uint64_t wr_deadline(struct wr_server *s)
{
    uint64_t deadline=UINT64_MAX;
    if (!s->session_only)
        for (unsigned h=0;h<2;h++)
            if (s->hive[h].dirty && s->hive[h].dirty_since+2000<deadline)
                deadline=s->hive[h].dirty_since+2000;
    struct wr_end *e=s->ending;
    if (e && e->stage<2 && e->at<e->count && e->deadline<deadline) deadline=e->deadline;
    struct wr_power *p=s->power;
    /* Match wr_session_wake: a power deadline cannot advance while a
     * session has priority. The release iteration evaluates it at s->now. */
    if ((!e || e->stage==2) && p && p->at<p->count && p->deadline &&
        (!p->windows[p->at].delivered || p->event>1) && p->deadline<deadline) deadline=p->deadline;
    return deadline;
}
