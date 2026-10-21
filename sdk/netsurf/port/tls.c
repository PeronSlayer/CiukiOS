/* BearSSL's generic engine on the native nonblocking sockets. SPDX-License-Identifier: MIT */
#include "tls.h"
#include "anchors.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef CIUKI_HOST
/* The host clock has no CiukiOS provenance. Fixtures must provide the native
 * uname result explicitly; an ordinary host run fails closed. */
bool ciuki_host_clock_rtc(void) __attribute__((weak));
#else
#include <sys/utsname.h>
#endif
int getentropy(void *,size_t);
struct cert_proxy { const br_x509_class *vtable;struct ciuki_tls *owner; };
struct ciuki_tls {
    br_ssl_client_context client;
    br_x509_minimal_context x509;
    struct cert_proxy proxy;
    unsigned char io[BR_SSL_BUFSIZE_BIDI];
    unsigned char der[32768];
    struct ciuki_tls_cert certs[10];
    size_t der_bytes,expected;
    unsigned depth;
    bool cert_overflow;
};
static struct ciuki_tls *owner(const br_x509_class **v) { return ((struct cert_proxy *)v)->owner; }
static void chain_start(const br_x509_class **v,const char *host)
{ struct ciuki_tls *t=owner(v);t->der_bytes=0;t->depth=0;t->cert_overflow=false;t->x509.vtable->start_chain(&t->x509.vtable,host); }
static void cert_start(const br_x509_class **v,uint32_t n)
{
    struct ciuki_tls *t=owner(v);t->expected=n;
    if(t->depth==10 || n>sizeof(t->der)-t->der_bytes)t->cert_overflow=true;
    if(!t->cert_overflow)t->certs[t->depth]=(struct ciuki_tls_cert){t->der_bytes,0};
    t->x509.vtable->start_cert(&t->x509.vtable,n);
}
static void cert_append(const br_x509_class **v,const unsigned char *s,size_t n)
{
    struct ciuki_tls *t=owner(v);
    if(n>t->expected)t->cert_overflow=true;else t->expected-=n;
    if(!t->cert_overflow){memcpy(t->der+t->der_bytes,s,n);t->der_bytes+=n;t->certs[t->depth].bytes+=n;}
    t->x509.vtable->append(&t->x509.vtable,s,n);
}
static void cert_end(const br_x509_class **v)
{ struct ciuki_tls *t=owner(v);if(t->expected)t->cert_overflow=true;if(!t->cert_overflow)t->depth++;t->x509.vtable->end_cert(&t->x509.vtable); }
static unsigned chain_end(const br_x509_class **v)
{ struct ciuki_tls *t=owner(v);unsigned r=t->x509.vtable->end_chain(&t->x509.vtable);return t->cert_overflow?BR_ERR_X509_LIMIT_EXCEEDED:r; }
static const br_x509_pkey *chain_key(const br_x509_class *const *v,unsigned *uses)
{ struct ciuki_tls *t=((const struct cert_proxy *)v)->owner;return t->x509.vtable->get_pkey((const br_x509_class *const *)&t->x509.vtable,uses); }
static const br_x509_class proxy_class={sizeof(struct cert_proxy),chain_start,cert_start,cert_append,cert_end,chain_end,chain_key};
static enum ciuki_tls_error tls_error(int e)
{
    if(e==BR_ERR_X509_BAD_SERVER_NAME)return CIUKI_TLS_HOST;
    if(e==BR_ERR_X509_EXPIRED)return CIUKI_TLS_DATE;
    if(e>=BR_ERR_X509_INVALID_VALUE && e<=BR_ERR_X509_NOT_TRUSTED)return CIUKI_TLS_CERT;
    return CIUKI_TLS_PROTOCOL;
}
bool ciuki_tls_clock(uint32_t *days,uint32_t *seconds)
{
#ifdef CIUKI_HOST
    if(!ciuki_host_clock_rtc || !ciuki_host_clock_rtc())return false;
#else
    struct utsname info;
    /* proc/clock.c selects source 1 only for a qualified, valid RTC sample;
     * source 0 is the build epoch, even when its date looks recent. */
    if(uname(&info) || info.realtime_source!=1)return false;
#endif
    struct timespec now;
    if(clock_gettime(CLOCK_REALTIME,&now) || now.tv_sec<1767225600LL || now.tv_sec>253402300799LL)return false;
    *days=(uint32_t)(now.tv_sec/86400+719528);*seconds=(uint32_t)(now.tv_sec%86400);return true;
}
struct ciuki_tls *ciuki_tls_create(const char *host,const br_x509_trust_anchor *anchors,size_t count,enum ciuki_tls_error *error)
{
    uint32_t days,seconds;struct in_addr numeric;
    *error=CIUKI_TLS_CLOCK;
    if(!ciuki_tls_clock(&days,&seconds))return NULL;
    *error=CIUKI_TLS_HOST;if(inet_pton(AF_INET,host,&numeric)==1)return NULL;
    unsigned char seed[32];*error=CIUKI_TLS_ENTROPY;
    if(getentropy(seed,sizeof(seed)))return NULL;
    struct ciuki_tls *t=calloc(1,sizeof(*t));
    if(t){
        static const uint16_t suites[]={BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
            BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256};
        br_ssl_client_init_full(&t->client,&t->x509,anchors?anchors:ciuki_anchors,anchors?count:CIUKI_ANCHOR_COUNT);
        br_ssl_engine_set_versions(&t->client.eng,BR_TLS12,BR_TLS12);
        br_ssl_engine_set_suites(&t->client.eng,suites,sizeof(suites)/sizeof(*suites));
        br_ssl_engine_set_buffer(&t->client.eng,t->io,sizeof(t->io),1);
        br_ssl_engine_inject_entropy(&t->client.eng,seed,sizeof(seed));
        br_x509_minimal_set_time(&t->x509,days,seconds);
        t->proxy.vtable=&proxy_class;t->proxy.owner=t;
        br_ssl_engine_set_x509(&t->client.eng,&t->proxy.vtable);
        *error=CIUKI_TLS_PROTOCOL;
        if(!br_ssl_client_reset(&t->client,host,0)){free(t);t=NULL;}
        else *error=CIUKI_TLS_OK;
    } else *error=CIUKI_TLS_MEMORY;
    volatile unsigned char *wipe=seed;for(size_t i=0;i<sizeof(seed);i++)wipe[i]=0;
    return t;
}
void ciuki_tls_free(struct ciuki_tls *t)
{ if(t){volatile unsigned char *p=(void *)t;for(size_t i=0;i<sizeof(*t);i++)p[i]=0;free(t);} }
unsigned ciuki_tls_state(struct ciuki_tls *t) { return br_ssl_engine_current_state(&t->client.eng); }
enum ciuki_tls_error ciuki_tls_pump(struct ciuki_tls *t,int fd,size_t *budget)
{
    for(unsigned steps=0;steps<16 && *budget;steps++){
        unsigned state=ciuki_tls_state(t);size_t n;ssize_t r;unsigned char *buffer;
        if(state&BR_SSL_CLOSED){int e=br_ssl_engine_last_error(&t->client.eng);return e?tls_error(e):CIUKI_TLS_EOF;}
        if(state&BR_SSL_SENDREC){
            buffer=br_ssl_engine_sendrec_buf(&t->client.eng,&n);if(n>*budget)n=*budget;
            r=send(fd,buffer,n,MSG_NOSIGNAL);
            if(r>0){br_ssl_engine_sendrec_ack(&t->client.eng,(size_t)r);*budget-=(size_t)r;continue;}
        } else if(state&BR_SSL_RECVREC){
            buffer=br_ssl_engine_recvrec_buf(&t->client.eng,&n);if(n>*budget)n=*budget;
            r=recv(fd,buffer,n,0);
            if(r>0){br_ssl_engine_recvrec_ack(&t->client.eng,(size_t)r);*budget-=(size_t)r;continue;}
            if(!r)return CIUKI_TLS_IO;
        } else return CIUKI_TLS_OK;
        if(r<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return CIUKI_TLS_OK;
        return CIUKI_TLS_IO;
    }
    return CIUKI_TLS_OK;
}
const unsigned char *ciuki_tls_read(struct ciuki_tls *t,size_t *n) { return br_ssl_engine_recvapp_buf(&t->client.eng,n); }
void ciuki_tls_read_ack(struct ciuki_tls *t,size_t n) { br_ssl_engine_recvapp_ack(&t->client.eng,n); }
unsigned char *ciuki_tls_write(struct ciuki_tls *t,size_t *n) { return br_ssl_engine_sendapp_buf(&t->client.eng,n); }
void ciuki_tls_write_ack(struct ciuki_tls *t,size_t n) { br_ssl_engine_sendapp_ack(&t->client.eng,n); }
void ciuki_tls_flush(struct ciuki_tls *t) { br_ssl_engine_flush(&t->client.eng,0); }
unsigned ciuki_tls_certificates(struct ciuki_tls *t,const unsigned char **der,const struct ciuki_tls_cert **certs)
{ *der=t->der;*certs=t->certs;return t->depth; }
const char *ciuki_tls_message(enum ciuki_tls_error e)
{
    static const char *const text[]={"", "Unable to allocate TLS connection.",
        "HTTPS requires a qualified RTC clock and a correct date (2026 or later). Please set the system clock.",
        "HTTPS is unavailable: secure system entropy is not available.",
        "TLS certificate hostname verification failed (numeric HTTPS hosts are unsupported).",
        "TLS certificate validation failed: untrusted, malformed, or unsupported certificate.",
        "TLS certificate has expired or is not yet valid.",
        "TLS protocol failed. This browser requires TLS 1.2.","TLS connection closed or was truncated."};
    return text[e];
}
