/* SPDX-License-Identifier: MIT -- native client of the existing w32srv RPC. */
#include "operations.h"
#include "../../sdk/win32/include/w32/server.h"
#include <ciuki/channel.h>
#include <ciuki/abi.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
uint32_t ciuki_syscall(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
static pthread_mutex_t transport_lock=PTHREAD_MUTEX_INITIALIZER;
static uint32_t transport_serial;
static uint64_t transport_now(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+(uint32_t)t.tv_nsec; }
static uint32_t transport_packet(int fd,const struct w32r_packet *in,struct w32r_packet *out)
{
    struct ciuki_message m={.length=256}; memcpy(m.data,in,256);
    uint64_t deadline=transport_now()+UINT64_C(2000000000);
    while (ciuki_channel_send(fd,&m,DONTWAIT)<0) {
        if ((errno!=EINTR && errno!=EAGAIN) || transport_now()>=deadline) return 1062;
        struct timespec pause={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
    for (;;) {
        int n=ciuki_channel_recv(fd,&m,DONTWAIT);
        if (n<0 && (errno==EINTR || errno==EAGAIN)) {
            if (transport_now()>=deadline) return 1062;
            struct timespec pause={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&pause,NULL); continue;
        }
        if (n!=1) return 1062;
        if (m.fd_count) { for(unsigned i=0;i<m.fd_count && i<4;i++) close(m.fds[i]); return 13; }
        if (m.length!=256) return 13;
        memcpy(out,m.data,256);
        if (out->version!=1) return 13;
        if (out->id!=in->id) { if (transport_now()>=deadline) return 1062; continue; }
        return out->kind==in->kind ? out->error : 13;
    }
}
uint32_t files_registry_exchange(const void *input,uint32_t n,void *output,uint32_t cap,uint32_t *used)
{
    if (!input || !output || !used || n<sizeof(struct w32r_request)+2 || n>W32R_MAX) return 87;
    *used=0; pthread_mutex_lock(&transport_lock);
    struct w32r_service service={.version=1,.size=sizeof(service),.operation=0x200,.action=2,.service=2};
    uint32_t error=ciuki_syscall(CIUKI_SYS_W32,(uint32_t)(uintptr_t)&service,sizeof(service),0,0,0,0);
    error=error?1062:service.error;
    if (!error && !service.result) error=1062;
    uint32_t id=++transport_serial; if (!id) id=++transport_serial;
    struct w32r_packet in,out; uint32_t at=0,total=0;
    while (!error && at<n) {
        in=(struct w32r_packet){.version=1,.id=id,.kind=at?W32R_WRITE:W32R_BEGIN,.total=n,.offset=at};
        uint32_t bytes=n-at; if (bytes>232) bytes=232;
        memcpy(in.bytes,(const uint8_t *)input+at,bytes);
        error=transport_packet((int)service.endpoint,&in,&out); if (error) break;
        at+=bytes; if (out.offset!=at) { error=13; break; } total=out.total;
    }
    if (!error && (total<sizeof(struct w32r_reply) || total>cap || total>W32R_MAX)) error=13;
    at=0;
    while (!error && at<total) {
        in=(struct w32r_packet){.version=1,.id=id,.kind=W32R_READ,.total=total,.offset=at};
        error=transport_packet((int)service.endpoint,&in,&out); if (error) break;
        uint32_t bytes=total-at; if (bytes>232) bytes=232;
        if (out.offset!=at+bytes || out.total!=total) { error=13; break; }
        memcpy((uint8_t *)output+at,out.bytes,bytes); at+=bytes;
    }
    if (!error) *used=total;
    pthread_mutex_unlock(&transport_lock); return error;
}
