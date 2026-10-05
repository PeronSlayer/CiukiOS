#ifndef CIUK_WEBIO_H
#define CIUK_WEBIO_H
#include "app.h"
#include "webnet.h"
#include "../web/worker_abi.h"
#define WIO_START 1
#define WIO_POLL 2
#define WIO_CANCEL 3
#define WIO_RESOLVE 4
#define WIO_WORK 5
#define WIO_WORK_POLL 6
#define WIO_WORK_READ 7
#pragma pack(push,1)
struct webio_request {
    u16 abi,size,operation,state,status,result,bytes,offset;
    u32 limit,wire;
    char url[128],path[128],location[128],error[96],type[96],resolved[128];
    struct cww_header work;
    u8 data[1024];
};
#pragma pack(pop)
int webio_start_file(const char *url,const char *path,u32 maxbytes);
int webio_poll(void);
void webio_cancel(void);
int webio_close(void);
int webio_status(void);
u32 webio_wire_bytes(void);
int webio_redirect(char *target,int capacity);
const char *webio_error(void);
int webio_resolve(const char *base,const char *ref,char *out,u16 capacity);
int webio_work(u16 op,const void *data,u16 bytes,u32 aux);
int webio_work_poll(struct cww_header *reply);
int webio_work_read(u16 offset,void *data,u16 bytes);
#endif
