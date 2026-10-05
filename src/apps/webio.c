#include "webio.h"
#include "webmodule.h"
static struct webmodule net;
static struct webio_request request;
static int call(u16 op)
{
    if(!webmodule_load(&net,"\\SYSTEM\\APPS\\WEBNET.APP")){
        str_copy(request.error,"The network module could not be loaded.");return 0;
    }
    request.abi=1;request.size=sizeof request;request.operation=op;request.result=0;
    webmodule_call(&net,EV_POLL,&request);return request.result;
}
int webio_start_file(const char *url,const char *path,u32 limit)
{
    str_ncopy(request.url,url,sizeof request.url);str_ncopy(request.path,path,sizeof request.path);
    request.limit=limit;
    return call(WIO_START)?request.state:WEBNET_FAILED;
}
int webio_poll(void){return call(WIO_POLL)?request.state:WEBNET_FAILED;}
void webio_cancel(void){if(net.segment)call(WIO_CANCEL);}
int webio_close(void)
{
    if(net.segment){
        if(webmodule_call(&net,EV_CLOSE,&request))return 0;
        webmodule_free(&net);
    }
    return 1;
}
int webio_status(void){return request.status;}
u32 webio_wire_bytes(void){return request.wire;}
int webio_redirect(char *target,int capacity)
{if(!request.location[0])return 0;str_ncopy(target,request.location,capacity);return 1;}
const char *webio_error(void){return request.error;}
int webio_resolve(const char *base,const char *ref,char *out,u16 capacity)
{
    str_ncopy(request.url,base,sizeof request.url);str_ncopy(request.path,ref,sizeof request.path);
    if(!call(WIO_RESOLVE)||str_len(request.resolved)>=capacity)return 0;
    str_copy(out,request.resolved);return 1;
}
int webio_work(u16 op,const void *data,u16 bytes,u32 aux)
{
    if(bytes>sizeof request.data)return 0;request.work.operation=op;request.work.aux=aux;
    request.bytes=bytes;if(bytes)mem_copy(request.data,data,bytes);return call(WIO_WORK);
}
int webio_work_poll(struct cww_header *reply)
{
    if(!call(WIO_WORK_POLL))return -1;
    mem_copy(reply,&request.work,sizeof *reply);return (short)request.state;
}
int webio_work_read(u16 offset,void *data,u16 bytes)
{
    if(bytes>sizeof request.data)return 0;request.offset=offset;request.bytes=bytes;
    if(!call(WIO_WORK_READ))return 0;mem_copy(data,request.data,bytes);return 1;
}
