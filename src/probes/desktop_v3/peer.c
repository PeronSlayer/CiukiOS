/* Authored PE guest fixture: no Windows DLLs, host libraries or stdout evidence.
 * It speaks the production v3 channel and the existing f5-08 FRAME interface.
 * SPDX-License-Identifier: MIT */
#include <ciuki/abi.h>
#include <stddef.h>
#include <ciuki/present_abi.h>
#include <w32/server.h>
#include "../../../apps/desktop/protocol.h"
void *memset(void *p,int c,size_t n){unsigned char *s=p;while(n--)*s++=(unsigned char)c;return p;}
void *memcpy(void *p,const void *q,size_t n){unsigned char *s=p;const unsigned char *t=q;while(n--)*s++=*t++;return p;}
#ifdef DESKTOP_V3_HOST
intptr_t desktop_v3_host_call(unsigned,uintptr_t,uintptr_t,uintptr_t);
#define call desktop_v3_host_call
#else
static intptr_t call(unsigned op,uintptr_t b,uintptr_t c,uintptr_t d)
{
    int result;
    __asm__ volatile("pushl %%ebp; xorl %%ebp,%%ebp; int $0x80; popl %%ebp"
        : "=a"(result):"a"(op),"b"(b),"c"(c),"d"(d),"S"(0),"D"(0):"memory","cc");
    return result;
}
#endif
#define PTR(p) ((uintptr_t)(p))
static unsigned clock_ms(void)
{struct ciuki_timespec t={0};call(CIUKI_SYS_CLOCK_GETTIME,CLOCK_MONOTONIC,PTR(&t),0);return (unsigned)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;}
static void sleep_ms(unsigned ms)
{struct ciuki_timespec t={.tv_nsec=(int)ms*1000000};call(CIUKI_SYS_NANOSLEEP,PTR(&t),0,0);}
static void quit(unsigned status){call(CIUKI_SYS_EXIT,status,0,0);for(;;);}
static int fd;static unsigned request;
static void message(struct ciuki_message *m,unsigned op,unsigned bytes,unsigned id)
{memset(m,0,sizeof(*m));m->length=16+bytes;m->data[0]=3;m->data[1]=(unsigned char)op;
 for(unsigned i=0;i<4;i++){m->data[4+i]=(unsigned char)(request>>(8*i));m->data[8+i]=(unsigned char)(id>>(8*i));m->data[12+i]=(unsigned char)((id ? 1u : 0u)>>(8*i));}request++;}
static unsigned get(const unsigned char *b){return b[0]|(unsigned)b[1]<<8|(unsigned)b[2]<<16|(unsigned)b[3]<<24;}
static void put(unsigned char *b,unsigned x){for(unsigned i=0;i<4;i++)b[i]=(unsigned char)(x>>(8*i));}
static void send(struct ciuki_message *m)
{unsigned end=clock_ms()+2000;while(call(CIUKI_SYS_CHANNEL_SEND,fd,PTR(m),DONTWAIT)<0){if((int)(clock_ms()-end)>=0)quit(2);sleep_ms(1);}}
static void create(unsigned id,unsigned style)
{
    int surface=call(CIUKI_SYS_SURFACE_CREATE,160,100,CIUKI_SURFACE_XRGB8888);if(surface<0)quit(3);
    unsigned *pixels=(void *)(uintptr_t)call(CIUKI_SYS_SURFACE_MAP,surface,PROT_READ|PROT_WRITE,0);
    if((uintptr_t)pixels>=(uintptr_t)-4095)quit(4);
    for(unsigned i=0;i<16000;i++)pixels[i]=0x375564+(id*0x10203);
    struct ciuki_message m;message(&m,DESK_CREATE_WINDOW,43,id);
    put(m.data+16,style);put(m.data+28,100+id*30);put(m.data+32,90+id*30);
    put(m.data+36,160);put(m.data+40,100);put(m.data+44,1);put(m.data+48,7);memcpy(m.data+52,"Fixture",7);
    m.fd_count=1;m.fds[0]=surface;send(&m);
    call(CIUKI_SYS_CLOSE,surface,0,0);
}
static unsigned char frame[64000];
#ifdef DESKTOP_V3_HOST
void entry(void)
#else
void __attribute__((stdcall)) entry(void)
#endif
{
    struct w32r_service service={1,sizeof(service),0x200,0,2,1,0,0,0,0};
    unsigned end=clock_ms()+10000;
    while(call(CIUKI_SYS_W32,PTR(&service),sizeof(service),0)<0 || service.error || !service.result) {
        if((int)(clock_ms()-end)>=0)quit(5);sleep_ms(10);service.error=service.result=0;
    }
    fd=(int)service.endpoint;request=0;
    struct ciuki_message m;message(&m,DESK_HELLO,0,0);send(&m);
    for(unsigned id=1;id<=4;id++)create(id,id==1?0:id==2?DESK_V3_POPUP:id==3?DESK_V3_TOOL:DESK_V3_TOPMOST);
    message(&m,DESK_SET_CURSOR,12,4);put(m.data+16,DESK_CURSOR_HAND);send(&m);
    message(&m,DESK_V3_POINTER,20,4);put(m.data+16,DESK_V3_CAPTURE|DESK_V3_CLIP|DESK_V3_RELATIVE|DESK_V3_HIDDEN);
    put(m.data+28,100);put(m.data+32,80);send(&m);
    message(&m,DESK_V3_WARP,8,4);put(m.data+16,10);put(m.data+20,10);send(&m);
    message(&m,DESK_V3_LEASE,16,4);put(m.data+16,1);put(m.data+20,320);put(m.data+24,200);put(m.data+28,8);send(&m);
    unsigned grants=0;end=clock_ms()+30000;
    for(;;) {
        int n=call(CIUKI_SYS_CHANNEL_RECV,fd,PTR(&m),DONTWAIT);
        if(n==0)quit(6);
        if(n==1 && m.length>=16 && m.data[0]==3) {
            unsigned op=m.data[1];
            if(op==DESK_V3_RESULT && (int)get(m.data+20)<0)quit(7);
            if(op==DESK_V3_LEASE_EVENT && m.length==88 && get(m.data+16)==1) {
                struct ciuki_w32_display_args a={.header={1,sizeof(a),W32_DISPLAY,0},.action=W32_DISPLAY_FRAME,
                    .token=get(m.data+36),.generation=get(m.data+40),.address=PTR(frame)};
                memset(frame,32,sizeof(frame));
                if(call(CIUKI_SYS_W32,PTR(&a),sizeof(a),0)<0 || a.error || !a.result)quit(8);
                if(++grants==2) {
                    message(&m,DESK_V3_LEASE,16,4);send(&m);
                    message(&m,DESK_V3_CRASH,20,4);put(m.data+16,0xc0000005);put(m.data+20,0x401234);
                    put(m.data+24,8);memcpy(m.data+28,"game.exe",8);send(&m);
                }
            }
            if(op==DESK_PING) {
                if(get(m.data+16)==0xf5090008) {message(&m,DESK_V3_RESTART,4,4);put(m.data+16,2);send(&m);}
                else {m.data[1]=DESK_PONG;send(&m);}
            }
        }
        if((int)(clock_ms()-end)>=0)quit(9);sleep_ms(1);
    }
}
