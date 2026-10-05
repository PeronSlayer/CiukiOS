/* Small host shim for including the production WebNet HTTP decoder. */
#ifndef WEBNET_HTTP_HOST_H
#define WEBNET_HTTP_HOST_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

struct host { unsigned ticks; };
extern struct host webnet_test_host;
#define HOST webnet_test_host

struct regs { u16 ax,bx,cx,dx,si,di,ds,es,flags,cxh,dxh,axh; };

#define WEBNET_IDLE       0
#define WEBNET_PENDING    1
#define WEBNET_COMPLETE   2
#define WEBNET_FAILED     3

int str_len(const char *s);
void str_copy(char *d,const char *s);
void str_ncopy(char *d,const char *s,int n);
void str_cat(char *d,const char *s);
int str_cmp(const char *a,const char *b);
void mem_copy(void *d,const void *s,int n);
void mem_move(void *d,const void *s,int n);
void mem_set(void *d,int c,int n);
int mem_cmp(const void *a,const void *b,int n);
void fmt_u32(char *d,u32 value);
int intr(int number,struct regs *r);
int dos_open(const char *path,int mode);
int dos_read(int handle,void *buffer,int bytes);
int dos_close(int handle);
u16 app_seg(void);
void webnet_cancel(void);
#include "../../src/web/worker_abi.h"
int webwork_submit(u16 op,const void *data,u16 bytes,u32 aux);
int webwork_poll(struct cww_header *reply,void *data,u16 capacity);
int webwork_result(u16 offset,void *data,u16 bytes);
int webwork_close(void);
void webwork_debug(void);

#endif
