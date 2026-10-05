/* Loaded as a CAPP only by the bounded integration harness. */
#include "app.h"
#include "webwork.h"
static struct cww_header reply;
static u8 answer[64];
static int waiting,phase,closing;
static const char message[]="CiukiOS worker XMS round trip";
static void close_step(void)
{
    if(!webwork_close())return;
    closing=0;
    if(phase==1){
        phase=2;waiting=webwork_submit(CWW_PING,message,sizeof message,0);
        app_log(waiting?"[WEBWORK TEST] submitted":"[WEBWORK TEST] reopen failed",0);
        if(!waiting){phase=5;closing=1;webwork_close();}
    }else if(phase==3){phase=4;app_log("[WEBWORK TEST] teardown and reopen PASS",0);}
    else if(phase==5)phase=6;
}
int app_event(int ev,int a,int b,int c)
{
    int result;(void)a;(void)b;(void)c;
    if(ev==EV_OPEN){
        str_copy(app_title,"Web worker validation");
        waiting=webwork_submit(CWW_PING,message,sizeof message,0);
        app_log(waiting?"[WEBWORK TEST] submitted":"[WEBWORK TEST] spawn failed",0);return 1;
    }
    if(ev==EV_POLL&&closing){close_step();return 0;}
    if(ev==EV_POLL&&waiting){
        result=webwork_poll(&reply,answer,sizeof answer);if(!result)return 0;
        waiting=0;
        if(result<0||reply.error||reply.output_bytes!=sizeof message||mem_cmp(message,answer,sizeof message)){
            app_log("[WEBWORK TEST] FAIL",0);phase=5;closing=1;close_step();return 0;
        }
        app_log("[WEBWORK TEST] round trip PASS",0);
        phase=phase==0?1:3;closing=1;close_step();
    }
    if(ev==EV_CLOSE)return !webwork_close();return 0;
}
