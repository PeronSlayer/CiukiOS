/* Session notifications run on the owning USER thread at message dispatch.
 * The service serializes windows and the desktop enforces a 10 s bound.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <ciuki/w32_diagnostics.h>
/* POLL and REPLY have no variable payload. Avoid the general directory
 * helper's W32R_MAX allocation and use precisely the wire reply size. */
static BOOL session_call(HWND h,DWORD action,DWORD tid,DWORD answer,struct w32r_reply *r)
{
    struct {struct w32r_request r;char empty[2];} a={.r={.version=1,.op=W32R_ENDSESSION,
        .key=U32_PTR(h),.index=action,.type=tid,.flags=answer,.path_bytes=1,.name_bytes=1}};
    DWORD used=0;
    DWORD e=u32_backend.rpc(&a,sizeof(a.r)+2,r,sizeof(*r),&used);
    if(!e && (used!=sizeof(*r) || r->version!=1 || r->name_bytes || r->data_bytes))e=13;
    if(!e)e=r->error;
    return e?u32_fail(e):1;
}
void u32_session_pump(void)
{
    U32_GUARD;
    DWORD tid=u32_tid();struct u32_thread *thread=u32_thread();
    DWORD now=u32_ticks();
    if(!thread || thread->session_active || (thread->session_poll_set && (int32_t)(now-thread->session_poll)<0))return;
    thread->session_active=1;thread->session_poll_set=1;
    DWORD saved=GetLastError();struct w32r_reply r;
    BOOL polled=session_call(0,W32R_SESSION_POLL,tid,0,&r);
    if(polled && r.key){
        HWND h=(HWND)(uintptr_t)r.key;struct u32_window *w=u32_window(h);
        BOOL answer=1;
        if(r.type==0x16 && r.count){
            struct ciuki_w32_report_args reason={.header={1,sizeof(reason),W32_REPORT,0},.action=W32_REPORT_REASON,.code=W32_EXIT_RESTART};
            u32_backend.call(&reason,sizeof(reason));
        }
        if(w && w->tid==tid){
            answer=!!u32_invoke(h,r.type,r.count,(LPARAM)(int32_t)r.disposition);
        }
        session_call(h,W32R_SESSION_REPLY,tid,answer,&r);
        now=u32_ticks();thread->session_fast=1;
        thread->session_fast_until=now+U32_SESSION_FAST_MS;
        thread->session_poll=now; /* next pump drains the next window */
    }else{
        now=u32_ticks();
        if(thread->session_fast && (!polled || (int32_t)(now-thread->session_fast_until)<0))
            thread->session_poll=polled?thread->session_fast_until:now+U32_SESSION_FAST_MS;
        else{
            thread->session_fast=0;
            thread->session_poll=now+U32_SESSION_POLL_MS;
        }
    }
    SetLastError(saved);thread->session_active=0;
}
