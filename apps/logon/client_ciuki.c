/* SPDX-License-Identifier: MIT
 * Target binding of the CSC1 client seam (logon_proto.h) over the static
 * Ciuki SDK, shared by LOGON_UI and the session desktop; LOGON's own seam
 * (sys_ciuki.c) reuses these calls. The host harness binds the seams to the
 * kernel host harness instead.
 */
#include "sys_ciuki.h"
#include <ciuki/runtime.h>
#include <ciuki/security.h>
#include <ciuki/channel.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>

int lgn_ciuki_security(void *ctx, struct ciuki_security_args *a, const void *in, uint32_t in_bytes, void *out, uint32_t out_bytes)
{
    (void)ctx;
    if (!a) return -EINVAL;
    struct ciuki_security_args call=*a;
    if (a->operation==CIUKI_SECURITY_SESSION_CREATE) {
        if (!in || in_bytes!=176) return -EINVAL;
        call.credential_address=CU_PTR(in); call.profile_address=CU_PTR((const uint8_t *)in+64);
    } else if (in) call.spawn_address=CU_PTR(in);
    if (out) { call.output_address=CU_PTR(out); call.output_bytes=out_bytes; }
    uint32_t r=CU_CALL(SECURITY,CU_PTR(&call),sizeof(call),0,0,0,0);
    int e=ciuki_error(r);
    if (e) return -e;
    /* Addresses are transport detail; the caller keeps record fields only. */
    a->result=call.result; a->state=call.state;
    return 0;
}
int lgn_ciuki_send(void *ctx, int fd, const struct ciuki_message *m)
{
    (void)ctx;
    uint32_t r=CU_CALL(CHANNEL_SEND,(uint32_t)fd,CU_PTR(m),(uint32_t)DONTWAIT,0,0,0);
    int e=ciuki_error(r); return e ? -e : 0;
}
int lgn_ciuki_recv(void *ctx, int fd, struct ciuki_message *m)
{
    (void)ctx;
    uint32_t r=CU_CALL(CHANNEL_RECV,(uint32_t)fd,CU_PTR(m),(uint32_t)DONTWAIT,0,0,0);
    int e=ciuki_error(r); return e ? -e : (int)r;
}
int lgn_ciuki_close(void *ctx, int fd)
{
    (void)ctx;
    uint32_t r=CU_CALL(CLOSE,(uint32_t)fd,0,0,0,0,0);
    int e=ciuki_error(r); return e ? -e : 0;
}
int lgn_ciuki_wait(void *ctx, const int *fds, unsigned count, unsigned timeout_ms)
{
    (void)ctx;
    if (!count) {
        struct timespec t={.tv_sec=(time_t)(timeout_ms/1000u),.tv_nsec=(long)(timeout_ms%1000u)*1000000L};
        return nanosleep(&t,NULL) ? -errno : 0;
    }
    struct pollfd set[32];
    if (count>32) count=32;
    for (unsigned i=0;i<count;i++) set[i]=(struct pollfd){.fd=fds[i],.events=POLLIN};
    int r=poll(set,count,(int)timeout_ms);
    return r<0 ? -errno : r;
}
static int wait_one(void *ctx, int fd, unsigned timeout_ms)
{ return fd<0 ? lgn_ciuki_wait(ctx,NULL,0,timeout_ms) : lgn_ciuki_wait(ctx,&fd,1,timeout_ms); }
uint64_t lgn_ciuki_now(void *ctx)
{
    (void)ctx;
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
}
void lgn_log_ciuki(const char *line) { CU_CALL(DEBUG_WRITE,CU_PTR(line),(uint32_t)strlen(line),0,0,0,0); }

const struct lgn_client_sys lgn_client_ciuki={NULL,lgn_ciuki_security,lgn_ciuki_send,lgn_ciuki_recv,wait_one,lgn_ciuki_close,lgn_ciuki_now};
