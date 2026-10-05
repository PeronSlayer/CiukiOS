#include "webio.h"
#include "webwork.h"
static struct webio_request request;
static int file=-1;
static int sink_file(const u8 *data,u16 bytes)
{return file>=0&&dos_write(file,data,bytes)==bytes;}
static int close_file(void)
{
    int rc=0;
    if(file>=0){rc=dos_close(file);file=-1;}
    return rc>=0;
}
static void report_close_error(void)
{
    request.state=WEBNET_FAILED;
    str_copy(request.error,"Could not finish writing the browser cache file.");
}
static void summary(void)
{
    request.status=webnet_http_status();request.wire=webnet_wire_bytes();
    request.location[0]=0;webnet_redirect(request.location,sizeof request.location);
    str_ncopy(request.error,webnet_error(),sizeof request.error);
    str_ncopy(request.type,webnet_content_type(),sizeof request.type);
}
int app_event(int event,int a,int b,int c)
{
    (void)c;
    if(event==EV_CLOSE){webnet_cancel();close_file();return !webwork_close();}
    if(event!=EV_OPEN&&event!=EV_POLL)return 0;
    if((u16)b>65536UL-sizeof request)return 0;
    far_copy(app_seg(),(u16)&request,(u16)a,(u16)b,sizeof request);
    if(request.abi!=1||request.size!=sizeof request)return 0;
    request.result=1;request.url[127]=request.path[127]=0;
    switch(request.operation){
    case WIO_START:
        webnet_cancel();
        if(!close_file()){report_close_error();break;}
        file=dos_create(request.path);
        if(file<0){request.state=WEBNET_FAILED;str_copy(request.error,"Cannot create the browser cache file.");break;}
        request.state=webnet_start_sink(request.url,sink_file,request.limit);summary();
        if(request.state!=WEBNET_PENDING&&!close_file())report_close_error();break;
    case WIO_POLL:
        request.state=webnet_poll();summary();
        if(request.state!=WEBNET_PENDING&&!close_file())report_close_error();break;
    case WIO_CANCEL:webnet_cancel();close_file();request.state=WEBNET_IDLE;break;
    case WIO_RESOLVE:
        request.result=webnet_resolve_url(request.url,request.path,request.resolved,sizeof request.resolved);break;
    case WIO_WORK:
        request.result=request.bytes<=sizeof request.data&&webwork_submit((u16)request.work.operation,request.data,request.bytes,request.work.aux);break;
    case WIO_WORK_POLL:request.state=webwork_poll(&request.work,0,0);break;
    case WIO_WORK_READ:
        request.result=request.bytes<=sizeof request.data&&webwork_result(request.offset,request.data,request.bytes);break;
    default:request.result=0;break;
    }
    far_copy((u16)a,(u16)b,app_seg(),(u16)&request,sizeof request);return 0;
}
