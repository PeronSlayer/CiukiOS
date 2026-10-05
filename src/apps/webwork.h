#ifndef CIUK_WEBWORK_H
#define CIUK_WEBWORK_H
#include "webstore.h"
#include "../web/worker_abi.h"
int webwork_open(void);
int webwork_submit(u16 operation,const void *data,u16 bytes,u32 aux);
/* 0 pending, 1 completed, -1 transport/process failure. */
int webwork_poll(struct cww_header *reply,void *data,u16 capacity);
int webwork_result(u16 offset,void *data,u16 bytes);
int webwork_close(void);
void webwork_debug(void);
#endif
