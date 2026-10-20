/* Session notifications run on the owning USER thread at message dispatch.
 * The service serializes windows and the desktop enforces a 10 s bound.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <ciuki/w32_diagnostics.h>
void u32_session_pump(void)
{
    U32_GUARD;
    DWORD tid=u32_tid();struct u32_thread *thread=u32_thread();
    if(!thread || thread->session_active || (thread->session_poll && (int32_t)(u32_ticks()-thread->session_poll)<0))return;
    thread->session_poll=u32_ticks()+U32_SESSION_POLL_MS;thread->session_active=1;
    DWORD saved=GetLastError();struct w32r_reply r;
    if(u32_directory(W32R_ENDSESSION,0,W32R_SESSION_POLL,tid,0,"",0,0,&r,0,0) && r.key){
        HWND h=(HWND)(uintptr_t)r.key;struct u32_window *w=u32_window(h);
        BOOL answer=1;
        if(r.type==0x16 && r.count){
            struct ciuki_w32_report_args reason={.header={1,sizeof(reason),W32_REPORT,0},.action=W32_REPORT_REASON,.code=W32_EXIT_RESTART};
            u32_backend.call(&reason,sizeof(reason));
        }
        if(w && w->tid==tid){
            answer=!!u32_invoke(h,r.type,r.count,(LPARAM)(int32_t)r.disposition);
        }
        u32_directory(W32R_ENDSESSION,h,W32R_SESSION_REPLY,tid,answer,"",0,0,&r,0,0);
    }
    SetLastError(saved);thread->session_active=0;
}
