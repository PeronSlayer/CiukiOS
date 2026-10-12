/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <ciuki/channel.h>
#include <reent.h>
typedef ssize_t _ssize_t;
static char output[4096]; static size_t used;
static int calls,fallback,failure,interrupt;
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    assert((fd==1 || fd==2) && !flags && !m->sender_pid && !m->fd_count && !m->reserved);
    assert(m->length<=CIUKI_MESSAGE_PAYLOAD_MAX);
    for (size_t i=m->length;i<sizeof(m->data);++i) assert(!m->data[i]);
    ++calls;
    if (interrupt) { interrupt=0; errno=EINTR; return -1; }
    if (failure && calls>=failure) { errno=EBADF; return -1; }
    memcpy(output+used,m->data,m->length); used+=m->length; return 0;
}
_ssize_t __real__write_r(struct _reent *r,int fd,const void *s,size_t n)
{ (void)r; (void)fd; (void)s; ++fallback; return (_ssize_t)n; }
#include "../../apps/tcc/capture.c"
static void reset(void) { used=0; calls=fallback=failure=interrupt=0; }
int main(void)
{
    struct _reent r={0}; char data[1025]; memset(data,'a',sizeof(data));
    reset(); interrupt=1; errno=123;
    assert(__wrap__write_r(&r,1,data,sizeof(data))==sizeof(data));
    assert(used==sizeof(data) && !memcmp(output,data,sizeof(data)) && calls==6 && !fallback && errno==123);
    reset(); failure=1; assert(__wrap__write_r(&r,2,data,sizeof(data))==sizeof(data) && fallback==1 && !used);
    reset(); assert(__wrap__write_r(&r,7,data,sizeof(data))==sizeof(data) && fallback==1 && !calls);
    reset(); failure=2; assert(__wrap__write_r(&r,1,data,sizeof(data))==256 && !fallback && r._errno==EBADF);
    reset(); assert(__wrap__write_r(&r,1,data,0)==0 && !calls && !fallback);
    puts("TCC stdio channel framing, console fallback, interruption and partial write: PASS"); return 0;
}
