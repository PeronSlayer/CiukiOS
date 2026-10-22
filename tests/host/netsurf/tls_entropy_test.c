/* Deterministic ordering at the real BearSSL boundary; synthetic bytes are
 * fixture-only and never linked into the browser. SPDX-License-Identifier: MIT */
#include "sdk/netsurf/port/tls.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static unsigned attempts,injections,resets;
static int failure;
static time_t uptime=12;
static bool rtc=true,clock_failure;
static unsigned char expected[32],previous[32];
static br_ssl_engine_context *seeded;
bool ciuki_host_clock_rtc(void) { return rtc; }
int __wrap_clock_gettime(clockid_t clock,struct timespec *now)
{
    if(clock==CLOCK_MONOTONIC && clock_failure){errno=EIO;return -1;}
    *now=(struct timespec){.tv_sec=clock==CLOCK_REALTIME?1790000000:uptime,.tv_nsec=999999999};return 0;
}
int __wrap_getentropy(void *buffer,size_t bytes)
{
    assert(bytes==sizeof(expected));attempts++;
    for(size_t i=0;i<bytes;i++)expected[i]=(unsigned char)(attempts+i);
    memcpy(buffer,expected,bytes); /* Also exercise a failed partial write. */
    if(failure){errno=failure;return -1;}return 0;
}
void __real_br_ssl_engine_inject_entropy(br_ssl_engine_context *,const void *,size_t);
void __wrap_br_ssl_engine_inject_entropy(br_ssl_engine_context *engine,const void *seed,size_t bytes)
{
    assert(!failure && attempts==injections+1 && injections==resets);
    assert(bytes==32 && !memcmp(seed,expected,bytes));
    if(injections)assert(memcmp(seed,previous,bytes));
    memcpy(previous,seed,bytes);seeded=engine;injections++;
    __real_br_ssl_engine_inject_entropy(engine,seed,bytes);
    assert(engine->rng_init_done==2);
}
int __real_br_ssl_client_reset(br_ssl_client_context *,const char *,int);
int __wrap_br_ssl_client_reset(br_ssl_client_context *client,const char *host,int resume)
{
    assert(!failure && injections==resets+1 && seeded==&client->eng);
    assert(client->eng.rng_init_done==2 && br_ssl_engine_get_rsavrfy(&client->eng));
    assert(br_ssl_engine_get_flags(&client->eng)&BR_OPT_NO_RENEGOTIATION);
    static const uint16_t offered[]={BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256};
    assert(client->eng.version_min==BR_TLS12 && client->eng.version_max==BR_TLS12);
    assert(client->eng.suites_num==4 && !memcmp(client->eng.suites_buf,offered,sizeof(offered)));
    assert(!strcmp(host,"fixture.example"));
    assert(!resume);resets++;return __real_br_ssl_client_reset(client,host,resume);
}
int main(void)
{
    uint32_t days,seconds;assert(ciuki_tls_clock(&days,&seconds));
    assert(days==1790000000u/86400u+719528u && seconds==1790000000u%86400u);
    enum ciuki_tls_error error;
    for(unsigned i=0;i<3;i++){
        struct ciuki_tls *tls=ciuki_tls_create("fixture.example",NULL,0,&error);
        assert(tls && error==CIUKI_TLS_OK && (ciuki_tls_state(tls)&BR_SSL_SENDREC));ciuki_tls_free(tls);
    }
    assert(attempts==3 && injections==3 && resets==3);
    for(unsigned i=0;i<4;i++){
        failure=(int[]){EAGAIN,ENOSYS,EIO,EINTR}[i];
        assert(!ciuki_tls_create("fixture.example",NULL,0,&error));
        assert(error==(failure==EAGAIN?CIUKI_TLS_NOT_READY:CIUKI_TLS_ENTROPY));
        assert(injections==3 && resets==3);
    }
    /* Reload is a fresh creation: no cached EAGAIN or poisoned context. */
    attempts=3;failure=0;
    struct ciuki_tls *tls=ciuki_tls_create("fixture.example",NULL,0,&error);
    assert(tls && !error && resets==4);ciuki_tls_free(tls);
    rtc=false;assert(!ciuki_tls_create("fixture.example",NULL,0,&error));
    assert(error==CIUKI_TLS_CLOCK && attempts==4 && resets==4);
    char early[192],later[192];
    assert(ciuki_tls_describe(CIUKI_TLS_NOT_READY,early,sizeof(early))==early);
    assert(strstr(early,"48 seconds") && strstr(early,"Reload to retry"));
    uptime=59;ciuki_tls_describe(CIUKI_TLS_NOT_READY,later,sizeof(later));
    assert(strstr(later,"1 seconds") && strstr(early,"48 seconds"));
    uptime=60;ciuki_tls_describe(CIUKI_TLS_NOT_READY,later,sizeof(later));
    assert(strstr(later,"estimate has elapsed") && strstr(later,"not guaranteed"));
    clock_failure=true;ciuki_tls_describe(CIUKI_TLS_NOT_READY,later,sizeof(later));
    assert(strstr(later,"Estimated wait unavailable"));
    puts("PASS fresh 32-byte injection before every reset; EAGAIN/permanent errors never reset; reload recovers; RTC and bounded wait estimates");
    fflush(stdout);
    return 0;
}
