/* One copied worker job; no UI pointers or socket handles cross the boundary.
 * SPDX-License-Identifier: MIT */
#include <stdatomic.h>
#include <netdb.h>
#include <sys/socket.h>
#include <poll.h>
struct cp_network_worker {
    pthread_t thread;
    atomic_int done;
    int active;
    unsigned operation;
    struct ciuki_net_control request;
    char name[128];
    struct cp_network_result result;
};
static void network_test_job(struct cp_network_worker *w)
{
    char dns[40]="DNS failed",gateway[40]="gateway unavailable";
    struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM},*answer=NULL;
    int e=getaddrinfo(w->name,NULL,&hints,&answer);
    if (!e && answer) {
        struct sockaddr_in *a=(void *)answer->ai_addr; const uint8_t *b=(const void *)&a->sin_addr.s_addr;
        snprintf(dns,sizeof(dns),"DNS %u.%u.%u.%u",b[0],b[1],b[2],b[3]);
    } else snprintf(dns,sizeof(dns),"DNS failed (%d)",e);
    if (answer) freeaddrinfo(answer);
    struct ciuki_net_control n={.version=1,.size=sizeof(n),.index=w->request.index};
    if (!ciuki_net_control(CIUKI_NETCTL_GET,&n) && n.address && n.gateway && n.config.enabled) {
        int fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);
        if (fd>=0) {
            struct sockaddr_in source={.sin_family=AF_INET,.sin_addr={.s_addr=n.address}};
            struct sockaddr_in target={.sin_family=AF_INET,.sin_port=htons(80),.sin_addr={.s_addr=n.gateway}};
            int error=0;
            if (bind(fd,(void *)&source,sizeof(source))) error=errno;
            else if (connect(fd,(void *)&target,sizeof(target))) {
                error=errno;
                if (error==EINPROGRESS) {
                    struct pollfd p={.fd=fd,.events=POLLOUT}; int ready=poll(&p,1,2000);
                    socklen_t bytes=sizeof(error);
                    if (ready>0) { if (getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&bytes)) error=errno; }
                    else error=ready ? errno : ETIMEDOUT;
                }
            }
            close(fd);
            snprintf(gateway,sizeof(gateway),error==0 ? "gateway TCP/80 connected" : error==ECONNREFUSED ? "gateway TCP/80 refused" : error==ETIMEDOUT ? "gateway TCP/80 timed out" : "gateway TCP/80 error %d",error);
        } else snprintf(gateway,sizeof(gateway),"gateway socket error %d",errno);
    }
    snprintf(w->result.message,sizeof(w->result.message),"%s; %s.",dns,gateway);
}
static void *network_job(void *arg)
{
    struct cp_network_worker *w=arg;
    memset(&w->result,0,sizeof(w->result));
    if (w->operation==CP_NET_TEST) network_test_job(w);
    else if (w->operation==CP_NET_IPX) {
        char text[32];
        int n=snprintf(text,sizeof(text),"frame_type=%u\n",w->request.ipx_frame_type);
        if (ciuki_system_setting_set(CIUKI_SYSTEM_SETTING_IPX_FRAME,text,(size_t)n)) {
            w->result.error=-errno;
            snprintf(w->result.message,sizeof(w->result.message),"IPX frame type could not be saved: %s (%d).",strerror(errno),errno);
        }
    }
    else if (w->operation!=CIUKI_NETCTL_GET && ciuki_net_control(w->operation,&w->request)) {
        w->result.error=-errno;
        snprintf(w->result.message,sizeof(w->result.message),"Network operation failed: %s (%d).",strerror(errno),errno);
    }
    for (unsigned i=0;i<2;i++) {
        struct ciuki_net_control *n=&w->result.adapters[i];
        *n=(struct ciuki_net_control){.version=1,.size=sizeof(*n),.index=i};
        if (ciuki_net_control(CIUKI_NETCTL_GET,n)) {
            if (i==0 && errno!=ENODEV && !w->result.error) { w->result.error=-errno;
                snprintf(w->result.message,sizeof(w->result.message),"Network status failed: %s (%d).",strerror(errno),errno); }
            break;
        }
        w->result.count++;
        if (i+1>=n->count) break;
    }
    if (!w->result.error && !w->result.message[0]) snprintf(w->result.message,sizeof(w->result.message),"%s",
        w->operation==CP_NET_IPX ? "IPX frame type saved and applied." : w->operation==CIUKI_NETCTL_SET ? "Network settings saved and applied." : w->operation==CIUKI_NETCTL_RENEW ? "DHCP renewal requested." :
        w->operation==CIUKI_NETCTL_RELEASE ? "DHCP lease released." : "Network status refreshed.");
    atomic_store_explicit(&w->done,1,memory_order_release); return NULL;
}
static int network_worker_begin(struct cp_network_worker *w,unsigned operation,const struct ciuki_net_control *request,const char *name)
{
    if (w->active) return -EBUSY;
    w->operation=operation; w->request=*request; snprintf(w->name,sizeof(w->name),"%s",name);
    atomic_store_explicit(&w->done,0,memory_order_relaxed);
    int e=pthread_create(&w->thread,NULL,network_job,w);
    if (e) return -e; w->active=1; return 0;
}
static int network_worker_poll(struct cp_network_worker *w,struct cp_network_result *out)
{
    if (!w->active || !atomic_load_explicit(&w->done,memory_order_acquire)) return 0;
    pthread_join(w->thread,NULL); w->active=0; *out=w->result; return 1;
}
static void network_worker_stop(struct cp_network_worker *w)
{ if (w->active) { pthread_join(w->thread,NULL); w->active=0; } }
