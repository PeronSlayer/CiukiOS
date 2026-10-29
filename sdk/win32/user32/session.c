/* Session notifications (WM_QUERYENDSESSION, WM_ENDSESSION and, G11,
 * WM_POWERBROADCAST) run on the owning USER thread at message dispatch.
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
    if(!thread)return;
    struct ciuki_w32_service_args wake={.header=K32_HEADER(W32_SERVICE,wake),
        .action=W32_SVC_SESSION,.service=W32_SERVICE_W32SRV};
    if(!u32_backend.call(&wake,sizeof(wake)) && !wake.error && wake.result)
        thread->session_deferred=1;
    /* A modal loop consumes a late duplicate too. Preserve any real next
     * turn until the callback returns, without recursively invoking it. */
    if(thread->session_active || !thread->session_deferred)return;
    thread->session_active=1;
    DWORD saved=GetLastError();struct w32r_reply r;
    do {
        thread->session_deferred=0;
        BOOL polled=session_call(0,W32R_SESSION_POLL,tid,0,&r);
        if(polled && r.key){
            HWND h=(HWND)(uintptr_t)r.key;struct u32_window *w=u32_window(h);
            BOOL answer=1;
            if(r.type==0x16 && r.count){
                struct ciuki_w32_report_args reason={.header={1,sizeof(reason),W32_REPORT,0},.action=W32_REPORT_REASON,.code=W32_EXIT_RESTART};
                u32_backend.call(&reason,sizeof(reason));
            }
            if(w && w->tid==tid){
                LRESULT result=u32_invoke(h,r.type,r.count,(LPARAM)(int32_t)r.disposition);
                /* G11: WM_POWERBROADCAST denies a query only with BROADCAST_QUERY_DENY. */
                answer=r.type==W32R_POWERBROADCAST ? (uint32_t)result!=0x424D5144u : !!result;
            }
            session_call(h,W32R_SESSION_REPLY,tid,answer,&r);
        }
    } while(thread->session_deferred);
    SetLastError(saved);thread->session_active=0;
}
