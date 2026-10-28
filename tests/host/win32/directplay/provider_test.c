/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Execute the patched Wine provider, not a second transport implementation.
 * The fake models Winsock's byte stream/datagrams; no host network is used.
 */
#define INITGUID
#include "winsock2.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define COBJMACROS
#include "provider-under-test.c"

#define PEERS 5
#define SOCKETS 128
#define BYTES 131072
struct endpoint {
    int live, kind, owner, connected, peer;
    SOCKADDR_IN address;
    unsigned char bytes[BYTES];
    unsigned size;
    SOCKADDR_IN source;
};
static struct endpoint endpoints[SOCKETS];
static int current_peer, last_error, startups, handles, drop_udp, short_write = 3;
static int socket_fail, bind_fail, thread_fail, setdata_fail;
static int delivered[PEERS], datagrams, writes, retransmitted_segments;
static int receive_arms;
static unsigned last_wire_size;
static unsigned char last_wire[BYTES];
struct fake_sp {
    IDirectPlaySP iface;
    DPWS_DATA *state;
    void *local[16], *remote[16];
    DWORD local_size[16], remote_size[16];
};
static struct fake_sp peers[PEERS];

static struct endpoint *ep(SOCKET s) { assert(s > 0 && s < SOCKETS && endpoints[s].live); return &endpoints[s]; }
static SOCKADDR_IN address(int peer, unsigned short port) {
    SOCKADDR_IN a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = 0x0100000aU + (peer << 24); a.sin_port = port; return a;
}
int WINAPI WSAStartup(WORD version, WSADATA *data) { (void)version; memset(data,0,sizeof(*data)); startups++; return 0; }
int WINAPI WSACleanup(void) { assert(startups); startups--; return 0; }
int WINAPI WSAGetLastError(void) { return last_error; }
SOCKET WINAPI socket(int family,int kind,int protocol) {
    (void)protocol; assert(family == AF_INET);
    if(socket_fail) { last_error=WSAENOBUFS; return INVALID_SOCKET; }
    for(int i=1;i<SOCKETS;i++) if(!endpoints[i].live) { memset(&endpoints[i],0,sizeof(endpoints[i])); endpoints[i].live=1;endpoints[i].kind=kind;endpoints[i].owner=current_peer;return i; }
    abort();
}
int WINAPI closesocket(SOCKET s) { ep(s)->live=0;return 0; }
int WINAPI bind(SOCKET s,const struct sockaddr *a,int length) {
    struct endpoint *e=ep(s); assert(length==sizeof(SOCKADDR_IN));
    if(bind_fail) {last_error=WSAEACCES;return SOCKET_ERROR;}
    e->address=*(const SOCKADDR_IN*)a;
    for(int i=1;i<SOCKETS;i++) if(i!=(int)s && endpoints[i].live && endpoints[i].owner==current_peer && endpoints[i].kind==e->kind && endpoints[i].address.sin_port==e->address.sin_port) {last_error=WSAEADDRINUSE;return SOCKET_ERROR;}
    e->address.sin_addr=address(current_peer,0).sin_addr;return 0;
}
int WINAPI listen(SOCKET s,int backlog) {(void)backlog;ep(s);return 0;}
SOCKET WINAPI accept(SOCKET s,struct sockaddr *a,int *n) {(void)s;(void)a;(void)n;last_error=WSAEWOULDBLOCK;return INVALID_SOCKET;}
int WINAPI connect(SOCKET s,const struct sockaddr *a,int n) {struct endpoint *e=ep(s);assert(n==sizeof(SOCKADDR_IN));e->address=*(const SOCKADDR_IN*)a;e->connected=1;e->peer=((e->address.sin_addr.s_addr>>24)&255)-1;return 0;}
int WINAPI setsockopt(SOCKET s,int level,int option,const char *v,int n) {(void)level;(void)option;(void)v;(void)n;ep(s);return 0;}
u_short WINAPI htons(u_short v) {return (v<<8)|(v>>8);}
u_short WINAPI ntohs(u_short v) {return (v<<8)|(v>>8);}
u_long WINAPI htonl(u_long v) {return __builtin_bswap32(v);}
u_long WINAPI inet_addr(const char *text) {if(!strcmp(text,"10.0.0.2"))return address(1,0).sin_addr.s_addr;return INADDR_NONE;}
struct hostent * WINAPI gethostbyname(const char *name) {(void)name;return NULL;}
int WINAPI send(SOCKET s,const char *data,int length,int flags) {
    struct endpoint *e=ep(s);(void)flags;assert(e->connected);
    if(length>short_write)length=short_write;
    assert(e->size+(unsigned)length<=BYTES);
    memcpy(e->bytes+e->size,data,length);e->size+=length;writes++;
    /* A segment lost below SOCK_STREAM is retransmitted by TCP, so it does
       not lose bytes at the API boundary. This is explicitly a model. */
    if(writes%3==0) retransmitted_segments++;
    return length;
}
int WINAPI sendto(SOCKET s,const char *data,int length,int flags,const struct sockaddr *dest,int n) {
    struct endpoint *e=ep(s);const SOCKADDR_IN *a=(const SOCKADDR_IN*)dest;(void)flags;assert(n==sizeof(*a));
    assert(length>0 && length<=BYTES);last_wire_size=length;memcpy(last_wire,data,length);datagrams++;
    if(drop_udp)return length;
    for(int i=1;i<SOCKETS;i++) { struct endpoint *target=&endpoints[i];
        if(!target->live||target->kind!=SOCK_DGRAM||target->address.sin_port!=a->sin_port)continue;
        if(a->sin_addr.s_addr!=INADDR_BROADCAST&&target->address.sin_addr.s_addr!=a->sin_addr.s_addr)continue;
        assert(!target->size);memcpy(target->bytes,data,length);target->size=length;target->source=address(e->owner,e->address.sin_port);
    }return length;
}
int WINAPI recvfrom(SOCKET s,char *data,int length,int flags,struct sockaddr *from,int *n) {
    struct endpoint *e=ep(s);(void)flags;
    if(!e->size){last_error=WSAEWOULDBLOCK;return SOCKET_ERROR;}
    assert(length>=(int)e->size&&*n>=sizeof(SOCKADDR_IN));memcpy(data,e->bytes,e->size);memcpy(from,&e->source,sizeof(e->source));*n=sizeof(e->source);length=e->size;e->size=0;return length;
}
int WINAPI WSARecv(SOCKET s,WSABUF *b,DWORD n,DWORD *done,DWORD *flags,WSAOVERLAPPED *ov,LPWSAOVERLAPPED_COMPLETION_ROUTINE cb) {(void)s;(void)b;(void)n;(void)done;(void)flags;(void)ov;(void)cb;receive_arms++;last_error=WSA_IO_PENDING;return SOCKET_ERROR;}
int WINAPI WSASendTo(SOCKET s,WSABUF *b,DWORD n,DWORD *done,DWORD flags,const struct sockaddr *a,int len,WSAOVERLAPPED *ov,LPWSAOVERLAPPED_COMPLETION_ROUTINE cb) {
    char bytes[65536];unsigned size=0;(void)ov;(void)cb;
    for(unsigned i=0;i<n;i++){assert(size+b[i].len<=sizeof(bytes));memcpy(bytes+size,b[i].buf,b[i].len);size+=b[i].len;}
    int result=sendto(s,bytes,size,flags,a,len);if(result<0)return result;*done=result;return 0;
}
int WINAPI WSAEventSelect(SOCKET s,WSAEVENT event,LONG mask) {(void)event;(void)mask;ep(s);return 0;}
int WINAPI WSAEnumNetworkEvents(SOCKET s,WSAEVENT event,WSANETWORKEVENTS *events) {(void)event;ep(s);memset(events,0,sizeof(*events));return 0;}
HANDLE WINAPI CreateEventW(SECURITY_ATTRIBUTES *a,BOOL manual,BOOL initial,const WCHAR *name) {(void)a;(void)manual;(void)initial;(void)name;handles++;return malloc(1);}
BOOL WINAPI CloseHandle(HANDLE h) {assert(h);handles--;free(h);return TRUE;}
BOOL WINAPI SetEvent(HANDLE h) {assert(h);return TRUE;}
HANDLE WINAPI CreateThread(SECURITY_ATTRIBUTES *a,SIZE_T size,LPTHREAD_START_ROUTINE f,void *arg,DWORD flags,DWORD *id) {(void)a;(void)size;(void)f;(void)arg;(void)flags;(void)id;if(thread_fail)return NULL;handles++;return malloc(1);}
DWORD WINAPI WaitForSingleObject(HANDLE h,DWORD t) {(void)h;(void)t;return WAIT_OBJECT_0;}
DWORD WINAPI WaitForMultipleObjectsEx(DWORD n,const HANDLE *h,BOOL all,DWORD ms,BOOL alert) {(void)n;(void)h;(void)all;(void)ms;(void)alert;return WAIT_OBJECT_0;}
HRESULT WINAPI SetThreadDescription(HANDLE h,const WCHAR *s) {(void)h;(void)s;return S_OK;}
void WINAPI InitializeCriticalSection(CRITICAL_SECTION *cs) {memset(cs,0,sizeof(*cs));}
void WINAPI DeleteCriticalSection(CRITICAL_SECTION *cs) {(void)cs;}
void WINAPI EnterCriticalSection(CRITICAL_SECTION *cs) {(void)cs;}
void WINAPI LeaveCriticalSection(CRITICAL_SECTION *cs) {(void)cs;}
HINSTANCE WINAPI GetModuleHandleW(const WCHAR *s) {(void)s;return NULL;}
INT_PTR WINAPI DialogBoxParamW(HINSTANCE i,const WCHAR *n,HWND p,DLGPROC cb,LPARAM arg) {(void)i;(void)n;(void)p;(void)cb;(void)arg;return IDCANCEL;}
LONG_PTR WINAPI SetWindowLongPtrW(HWND h,int i,LONG_PTR n) {(void)h;(void)i;return n;}
LONG_PTR WINAPI GetWindowLongPtrW(HWND h,int i) {(void)h;(void)i;return 0;}
UINT WINAPI GetDlgItemTextA(HWND h,int i,char *s,int n) {(void)h;(void)i;(void)s;(void)n;return 0;}
BOOL WINAPI EndDialog(HWND h,INT_PTR n) {(void)h;(void)n;return TRUE;}
int WINAPI MessageBoxW(HWND h,const WCHAR *s,const WCHAR *title,UINT f) {(void)h;(void)s;(void)title;(void)f;return IDOK;}

static HRESULT WINAPI get_sp(IDirectPlaySP *iface,void **data,DWORD *size,DWORD flags) {struct fake_sp *p=(void*)iface;assert(flags==DPSET_LOCAL);*data=p->state;*size=sizeof(*p->state);return DP_OK;}
static HRESULT WINAPI set_sp(IDirectPlaySP *iface,void *data,DWORD size,DWORD flags) {struct fake_sp *p=(void*)iface;assert(flags==DPSET_LOCAL);if(setdata_fail)return DPERR_OUTOFMEMORY;p->state=malloc(size);assert(p->state);memcpy(p->state,data,size);return DP_OK;}
static HRESULT WINAPI get_player(IDirectPlaySP *iface,DPID id,void **data,DWORD *size,DWORD flags) {struct fake_sp *p=(void*)iface;assert(id<16);*data=flags==DPSET_LOCAL?p->local[id]:p->remote[id];*size=flags==DPSET_LOCAL?p->local_size[id]:p->remote_size[id];return *data?DP_OK:DPERR_INVALIDPLAYER;}
static HRESULT WINAPI set_player(IDirectPlaySP *iface,DPID id,void *data,DWORD size,DWORD flags) {struct fake_sp *p=(void*)iface;assert(id<16);void **slot=flags==DPSET_LOCAL?&p->local[id]:&p->remote[id];DWORD *sz=flags==DPSET_LOCAL?&p->local_size[id]:&p->remote_size[id];free(*slot);*slot=malloc(size);memcpy(*slot,data,size);*sz=size;return DP_OK;}
static HRESULT WINAPI message(IDirectPlaySP *iface,void *data,DWORD size,void *header) {(void)data;(void)size;(void)header;delivered[(struct fake_sp*)iface-peers]++;return DP_OK;}
static HRESULT WINAPI enum_address(IDirectPlaySP *iface,LPDPENUMADDRESSCALLBACK cb,const void *data,DWORD size,void *ctx) {(void)iface;(void)cb;(void)data;(void)size;(void)ctx;return DP_OK;}
static const IDirectPlaySPVtbl vtable={.GetSPData=get_sp,.SetSPData=set_sp,.GetSPPlayerData=get_player,.SetSPPlayerData=set_player,.HandleMessage=message,.EnumAddress=enum_address};
static DPSP_SPCALLBACKS callbacks[PEERS];
static void init_peer(int i) {SPINITDATA init={0};current_peer=i;peers[i].iface.lpVtbl=&vtable;init.lpISP=&peers[i].iface;init.lpCB=&callbacks[i];init.lpGuid=(GUID*)&DPSPGUID_TCPIP;assert(SPInit(&init)==DP_OK);assert(init.dwSPHeaderSize==20);}
static void open_peer(int i,int host) {DPSP_OPENDATA open={0};DPSP_MSG_HEADER header={0};current_peer=i;header.SockAddr=address(0,htons(2300));open.lpISP=&peers[i].iface;open.bCreate=host;open.lpSPMessageHeader=host?NULL:&header;assert(callbacks[i].Open(&open)==DP_OK);}
static int live_sockets(void) {int n=0;for(int i=1;i<SOCKETS;i++)n+=endpoints[i].live;return n;}
static void close_peer(int i) {DPSP_SHUTDOWNDATA shutdown={.lpISP=&peers[i].iface};current_peer=i;assert(callbacks[i].ShutdownEx(&shutdown)==DP_OK);free(peers[i].state);peers[i].state=NULL;for(int n=0;n<16;n++){free(peers[i].local[n]);free(peers[i].remote[n]);peers[i].local[n]=peers[i].remote[n]=NULL;}}
int main(void) {
    for(int count=2;count<=PEERS;count++) {
        for(int i=0;i<count;i++)init_peer(i);
        open_peer(0,1);
        for(int i=1;i<count;i++) {
            unsigned char request[52]={0};memcpy(request+20,"play",4);request[24]=2;request[26]=11;
            DPSP_ENUMSESSIONSDATA enumerate={.lpMessage=request,.dwMessageSize=sizeof(request),.lpISP=&peers[i].iface,.bReturnStatus=TRUE};
            current_peer=i;assert(callbacks[i].EnumSessions(&enumerate)==DP_OK);
            assert(ep(peers[0].state->enumSock)->size==52);
            int previous=delivered[0];DPWS_ReceiveEnum(peers[0].state);assert(delivered[0]==previous+1);
            assert(ntohs(((DPSP_MSG_HEADER*)peers[0].state->enumBuffer)->SockAddr.sin_port)>=2300);
            open_peer(i,0);
            unsigned char reply[28]={0};DPSP_MSG_HEADER header={0};header.SockAddr=address(i,peers[i].state->tcpAddr.sin_port);
            DPSP_REPLYDATA response={.lpSPMessageHeader=&header,.lpMessage=reply,.dwMessageSize=sizeof(reply),.lpISP=&peers[0].iface};
            current_peer=0;assert(callbacks[0].Reply(&response)==DP_OK);assert(writes>0);
            DPWS_PLAYERDATA remote={address(0,peers[0].state->tcpAddr.sin_port),address(0,peers[0].state->udpAddr.sin_port)};
            assert(set_player(&peers[i].iface,1,&remote,sizeof(remote),DPSET_REMOTE)==DP_OK);
            DPSP_MSG_HEADER source={0};source.SockAddr=address(0,peers[0].state->tcpAddr.sin_port);
            DPSP_CREATEPLAYERDATA create={.idPlayer=1,.lpSPMessageHeader=&source,.lpISP=&peers[i].iface};
            current_peer=i;assert(callbacks[i].CreatePlayer(&create)==DP_OK);
            unsigned char text[25]={0};memcpy(text+20,"chat",5);
            DPSP_SENDDATA senddata={.dwFlags=DPSEND_GUARANTEED,.idPlayerTo=1,.lpMessage=text,.dwMessageSize=sizeof(text),.lpISP=&peers[i].iface};
            assert(callbacks[i].Send(&senddata)==DP_OK);
            int found_stream=0;
            for(int n=1;n<SOCKETS;n++) if(endpoints[n].live && endpoints[n].owner==i && endpoints[n].kind==SOCK_STREAM && endpoints[n].connected) {
                assert(endpoints[n].size==sizeof(text));
                assert(!memcmp(endpoints[n].bytes+20,"chat",5));found_stream++;
            }
            assert(found_stream==1);
            DPSP_CREATEGROUPDATA group={.idGroup=2,.lpISP=&peers[i].iface};
            assert(callbacks[i].CreateGroup(&group)==DP_OK);
            DPSP_ADDPLAYERTOGROUPDATA add={.idPlayer=1,.idGroup=2,.lpISP=&peers[i].iface};
            assert(callbacks[i].AddPlayerToGroup(&add)==DP_OK);
            SGBUFFER buffers[2]={{20,text},{5,text+20}};
            DPSP_SENDTOGROUPEXDATA group_send={.lpISP=&peers[i].iface,.dwFlags=DPSEND_GUARANTEED,
                .idGroupTo=2,.lpSendBuffers=buffers,.cBuffers=2,.dwMessageSize=25};
            assert(callbacks[i].SendToGroupEx(&group_send)==DP_OK);
            DPSP_REMOVEPLAYERFROMGROUPDATA remove_group={.idPlayer=1,.idGroup=2,.lpISP=&peers[i].iface};
            assert(callbacks[i].RemovePlayerFromGroup(&remove_group)==DP_OK);
            DPSP_DELETEGROUPDATA delete_group={.idGroup=2,.lpISP=&peers[i].iface};
            assert(callbacks[i].DeleteGroup(&delete_group)==DP_OK);
            senddata.dwFlags=0;senddata.lpMessage=text+20;senddata.dwMessageSize=5;drop_udp=1;
            assert(callbacks[i].Send(&senddata)==DP_OK);drop_udp=0;
            assert(callbacks[i].Send(&senddata)==DP_OK);assert(ep(peers[0].state->udpSock)->size==5);ep(peers[0].state->udpSock)->size=0;
            DPSP_DELETEPLAYERDATA remove={.idPlayer=1,.lpISP=&peers[i].iface};assert(callbacks[i].DeletePlayer(&remove)==DP_OK);
        }
        close_peer(0); /* Host departure releases the actual listener. */
        for(int i=1;i<count;i++)close_peer(i);
        assert(!startups&&!handles&&!live_sockets());
    }
    init_peer(0);socket_fail=1;DPSP_OPENDATA open={.bCreate=TRUE,.lpISP=&peers[0].iface};assert(callbacks[0].Open(&open)==DPERR_UNAVAILABLE);socket_fail=0;
    bind_fail=1;assert(callbacks[0].Open(&open)==DPERR_UNAVAILABLE);bind_fail=0;
    thread_fail=1;assert(callbacks[0].Open(&open)==DPERR_UNAVAILABLE);thread_fail=0;close_peer(0);
    assert(!startups&&!handles&&!live_sockets());
    SPINITDATA init={.lpISP=&peers[0].iface,.lpCB=&callbacks[0],.lpGuid=(GUID*)&DPSPGUID_TCPIP};setdata_fail=1;assert(SPInit(&init)==DPERR_OUTOFMEMORY);setdata_fail=0;assert(!startups);
    init.lpGuid=(GUID*)&DPSPGUID_IPX;assert(SPInit(&init)==DPERR_UNAVAILABLE);assert(!startups);
    init_peer(0);open_peer(0,1);
    DPWS_DATA *state=peers[0].state;
    memcpy(state->buffer+20,"play",4);
    ((DPSP_MSG_HEADER*)state->buffer)->mixed=0xfab00004; /* Invalid short frame. */
    int before=receive_arms;DPWS_UdpReceiveCompleted(0,28,&state->overlapped,0);
    assert(receive_arms==before+1);
    ((DPSP_MSG_HEADER*)state->buffer)->mixed=0xfab00100; /* Truncated frame. */
    before=receive_arms;DPWS_UdpReceiveCompleted(0,28,&state->overlapped,0);
    assert(receive_arms==before+1);
    const char *numeric="10.0.0.2";
    assert(DPWS_ReadAddress(&DPAID_INet,strlen(numeric)+1,numeric,state));
    assert(state->enumTarget==address(1,0).sin_addr.s_addr && state->targetChosen);
    close_peer(0);assert(!startups&&!handles&&!live_sockets());
    assert(retransmitted_segments&&datagrams&&last_wire_size);
    printf("PASS patched Wine TCP provider: 2-5 peers, host/listener, enumeration, join endpoints, replies, short stream writes, guaranteed TCP/loss model, UDP loss, player cleanup, host leaving and failure rollback\n");
    fflush(stdout);
    return 0;
}
