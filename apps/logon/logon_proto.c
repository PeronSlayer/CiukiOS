/* SPDX-License-Identifier: MIT
 * Private LOGON protocol codecs and the CSC1 client (logon_proto.h). Every
 * decoder re-encodes and compares, so padding and reserved bytes must be
 * canonical zero; unknown values fail EINVAL.
 */
#include "logon_proto.h"
#include <ciuki/abi.h>
#include <errno.h>
#include <string.h>

#define CSC1 0x31435343u
static uint32_t rd32(const void *v) { const uint8_t *p=v; return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t rd64(const void *v) { return rd32(v)|(uint64_t)rd32((const uint8_t *)v+4)<<32; }
static void wr32(void *v, uint32_t x) { uint8_t *p=v; for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(x>>(8*i)); }
static void wr64(void *v, uint64_t x) { wr32(v,(uint32_t)x); wr32((uint8_t *)v+4,(uint32_t)(x>>32)); }
static int padded(const char *s, size_t n)
{
    const char *end=memchr(s,0,n);
    if (!end) return 0;
    for (const char *p=end;p<s+n;p++) if (*p) return 0;
    return 1;
}

void lgn_wipe(void *p, size_t n)
{
    volatile uint8_t *b=p;
    while (n--) *b++=0;
}
int lgn_name_syntax(const char *s)
{
    if (!s || !s[0]) return 0;
    for (unsigned i=0;i<32;i++) {
        unsigned c=(unsigned char)s[i];
        if (!c) return 1;
        if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-')) return 0;
    }
    return 0;
}
int lgn_secret_syntax(const char *s)
{
    if (!s) return 0;
    for (unsigned i=0;i<64;i++) {
        unsigned c=(unsigned char)s[i];
        if (!c) return 1;
        if (c<0x20 || c>0x7e) return 0;
    }
    return 0;
}

int lgn_ui_command_encode(const struct lgn_ui_command *c, void *v, size_t n)
{
    if (!c || !v || n!=LGN_UI_COMMAND_BYTES || c->command>LGN_CMD_RESTART) return -EINVAL;
    int targeted=c->command==LGN_CMD_LOGOFF || c->command==LGN_CMD_TASKMGR || c->command==LGN_CMD_FORCE_LOGOFF ||
                 c->command==LGN_CMD_SET_PASSWORD || c->command==LGN_CMD_SHUTDOWN || c->command==LGN_CMD_RESTART;
    if (targeted ? (!c->session || !c->console) : (c->session || c->console)) return -EINVAL;
    uint8_t *p=v; memset(p,0,n); wr32(p,1); wr32(p+4,c->command); wr32(p+8,c->session); wr32(p+12,c->console);
    return 0;
}
int lgn_ui_command_decode(struct lgn_ui_command *c, const void *v, size_t n)
{
    if (!c || !v || n!=LGN_UI_COMMAND_BYTES || rd32(v)!=1) return -EINVAL;
    struct lgn_ui_command t={rd32((const uint8_t *)v+4),rd32((const uint8_t *)v+8),rd32((const uint8_t *)v+12)};
    uint8_t canonical[LGN_UI_COMMAND_BYTES];
    int e=lgn_ui_command_encode(&t,canonical,sizeof(canonical));
    if (!e && memcmp(canonical,v,n)) e=-EINVAL;
    if (!e) *c=t;
    return e;
}
int lgn_state_encode(const struct lgn_state *s, void *v, size_t n)
{
    if (!s || !v || n!=LGN_UI_STATE_BYTES || s->mode<LGN_MODE_STARTING || s->mode>LGN_MODE_POWER ||
        s->message>LGN_MSG_SERVICE_RESTART || s->error>0 || (s->flags&~31u) || !padded(s->owner_name,32)) return -EINVAL;
    uint8_t *p=v; memset(p,0,n);
    wr32(p,1); wr32(p+4,LGN_UI_STATE_BYTES); wr32(p+8,s->mode); wr32(p+12,s->flags); wr32(p+16,s->session);
    wr32(p+20,s->console); wr32(p+24,s->owner_uid); wr32(p+28,s->message); wr32(p+32,(uint32_t)s->error);
    wr64(p+40,s->generation); memcpy(p+48,s->owner_name,32);
    return 0;
}
int lgn_state_decode(struct lgn_state *s, const void *v, size_t n)
{
    const uint8_t *p=v;
    if (!s || !p || n!=LGN_UI_STATE_BYTES || rd32(p)!=1 || rd32(p+4)!=LGN_UI_STATE_BYTES) return -EINVAL;
    struct lgn_state t; memset(&t,0,sizeof(t));
    t.mode=rd32(p+8); t.flags=rd32(p+12); t.session=rd32(p+16); t.console=rd32(p+20); t.owner_uid=rd32(p+24);
    t.message=rd32(p+28); t.error=(int32_t)rd32(p+32); t.generation=rd64(p+40); memcpy(t.owner_name,p+48,32);
    uint8_t canonical[LGN_UI_STATE_BYTES];
    int e=lgn_state_encode(&t,canonical,sizeof(canonical));
    if (!e && memcmp(canonical,p,n)) e=-EINVAL;
    if (!e) *s=t;
    return e;
}
int lgn_notice_encode(const struct lgn_session_notice *s, void *v, size_t n)
{
    if (!s || !v || n!=LGN_SESSION_NOTICE_BYTES || s->notice>LGN_NOTICE_PASSWORD || !s->session) return -EINVAL;
    uint8_t *p=v; memset(p,0,n); wr32(p,1); wr32(p+4,s->notice); wr32(p+8,s->session);
    return 0;
}
int lgn_notice_decode(struct lgn_session_notice *s, const void *v, size_t n)
{
    if (!s || !v || n!=LGN_SESSION_NOTICE_BYTES || rd32(v)!=1) return -EINVAL;
    struct lgn_session_notice t={rd32((const uint8_t *)v+4),rd32((const uint8_t *)v+8)};
    uint8_t canonical[LGN_SESSION_NOTICE_BYTES];
    int e=lgn_notice_encode(&t,canonical,sizeof(canonical));
    if (!e && memcmp(canonical,v,n)) e=-EINVAL;
    if (!e) *s=t;
    return e;
}
int lgn_event_encode(const struct lgn_session_event *s, void *v, size_t n)
{
    if (!s || !v || n!=LGN_SESSION_EVENT_BYTES || s->action>LGN_ACT_SET_PASSWORD || (s->flags&~3u) ||
        !s->session || !padded(s->name,32)) return -EINVAL;
    uint8_t *p=v; memset(p,0,n);
    wr32(p,1); wr32(p+4,LGN_SESSION_EVENT_BYTES); wr32(p+8,s->action); wr32(p+12,s->flags); wr32(p+16,s->uid);
    wr32(p+20,s->session); memcpy(p+32,s->name,32);
    return 0;
}
int lgn_event_decode(struct lgn_session_event *s, const void *v, size_t n)
{
    const uint8_t *p=v;
    if (!s || !p || n!=LGN_SESSION_EVENT_BYTES || rd32(p)!=1 || rd32(p+4)!=LGN_SESSION_EVENT_BYTES) return -EINVAL;
    struct lgn_session_event t; memset(&t,0,sizeof(t));
    t.action=rd32(p+8); t.flags=rd32(p+12); t.uid=rd32(p+16); t.session=rd32(p+20); memcpy(t.name,p+32,32);
    uint8_t canonical[LGN_SESSION_EVENT_BYTES];
    int e=lgn_event_encode(&t,canonical,sizeof(canonical));
    if (!e && memcmp(canonical,p,n)) e=-EINVAL;
    if (!e) *s=t;
    return e;
}
int lgn_progress_encode(const struct lgn_progress *g, void *v, size_t n)
{
    if (!g || !v || n!=LGN_PROGRESS_BYTES || g->disposition>LGN_FAILED || g->status>0 || g->retry_after_ms>1000) return -EINVAL;
    uint8_t *p=v; memset(p,0,n); wr32(p,1); wr32(p+4,g->disposition); wr32(p+8,(uint32_t)g->status); wr32(p+12,g->retry_after_ms);
    return 0;
}
int lgn_progress_decode(struct lgn_progress *g, const void *v, size_t n)
{
    const uint8_t *p=v;
    if (!g || !p || n!=LGN_PROGRESS_BYTES || rd32(p)!=1) return -EINVAL;
    struct lgn_progress t={rd32(p+4),(int32_t)rd32(p+8),rd32(p+12)};
    uint8_t canonical[LGN_PROGRESS_BYTES];
    int e=lgn_progress_encode(&t,canonical,sizeof(canonical));
    if (!e && memcmp(canonical,p,n)) e=-EINVAL;
    if (!e) *g=t;
    return e;
}
/* Offsets of contract 4.1: version@0 flags@4 attempt@8 session@16 console@20
 * name_bytes@24 secret_bytes@28 new_secret_bytes@32 target_uid@36
 * expected_accounts_generation@40 name[32]@48 secret[64]@80 new_secret[64]@144. */
int lgn_submit_encode(const void *challenge, const char *name, const char *secret, const char *new_secret, void *out)
{
    const uint8_t *c=challenge;
    if (!c || !out || !name || !secret || !new_secret || rd32(c)!=1) return -EINVAL;
    size_t nn=strnlen(name,32), sn=strnlen(secret,64), xn=strnlen(new_secret,64);
    if (nn>31 || sn>63 || xn>63 || (nn && !lgn_name_syntax(name)) || !lgn_secret_syntax(secret) ||
        !lgn_secret_syntax(new_secret)) return -EINVAL;
    uint8_t *p=out; memset(p,0,LGN_SUBMIT_BYTES);
    wr32(p,1); wr64(p+8,rd64(c+8)); wr32(p+16,rd32(c+20)); wr32(p+20,rd32(c+24));
    wr32(p+24,(uint32_t)nn); wr32(p+28,(uint32_t)sn); wr32(p+32,(uint32_t)xn); wr32(p+36,rd32(c+16));
    wr64(p+40,rd64(c+48)); memcpy(p+48,name,nn); memcpy(p+80,secret,sn); memcpy(p+144,new_secret,xn);
    return 0;
}

/* ---- CSC1 client ------------------------------------------------------ */
void lgn_client_init(struct lgn_client *c, const struct lgn_client_sys *sys, uint32_t service, uint32_t server_role)
{
    memset(c,0,sizeof(*c)); c->sys=sys; c->fd=-1; c->service=service; c->server_role=server_role;
}
void lgn_client_close(struct lgn_client *c)
{
    if (c->fd>=0 && c->sys) c->sys->close(c->sys->ctx,c->fd);
    c->fd=-1; c->endpoint_generation=c->service_generation=c->peer_pid=0; c->next_id=0;
}
int lgn_client_connect(struct lgn_client *c)
{
    if (!c || !c->sys) return -EINVAL;
    if (c->fd>=0) return 0;
    struct ciuki_security_link in={.version=1,.size=64,.service=c->service}, out;
    memset(&out,0,sizeof(out));
    struct ciuki_security_args a={.version=1,.size=64,.operation=CIUKI_SECURITY_SERVICE_CONNECT};
    int e=c->sys->security(c->sys->ctx,&a,&in,64,&out,64);
    if (e) return e;
    if (out.endpoint_fd<0 || !out.endpoint_generation || out.service!=c->service || out.server_role!=c->server_role ||
        out.peer_role!=c->server_role) {
        if (out.endpoint_fd>=0) c->sys->close(c->sys->ctx,out.endpoint_fd);
        return -EPERM;
    }
    c->fd=out.endpoint_fd; c->endpoint_generation=out.endpoint_generation; c->service_generation=out.service_generation;
    c->peer_pid=out.peer_pid; c->session=out.session_id; c->next_id=0;
    return 0;
}
int lgn_client_call(struct lgn_client *c, uint32_t op, uint64_t generation, const void *body, size_t n,
                    void *reply, size_t cap, size_t *used, int *status, unsigned timeout_ms)
{
    if (!c || !c->sys || !used || !status || n>CIUKI_SECURITY_SERVICE_BODY_MAX || (n && !body) || (cap && !reply)) return -EINVAL;
    *used=0; *status=0;
    int e=lgn_client_connect(c);
    if (e) return e;
    if (c->next_id==UINT64_MAX) { lgn_client_close(c); return -EOVERFLOW; }
    uint64_t id=++c->next_id, deadline=c->sys->now_ms(c->sys->ctx)+timeout_ms;
    struct ciuki_message m;
    for (size_t at=0;;) {
        size_t take=n-at; if (take>CIUKI_SECURITY_SERVICE_FRAGMENT_MAX) take=CIUKI_SECURITY_SERVICE_FRAGMENT_MAX;
        memset(&m,0,sizeof(m)); uint8_t *h=m.data;
        wr32(h,CSC1); wr32(h+4,1); wr32(h+8,64); wr32(h+12,c->service); wr32(h+16,op); wr32(h+20,1);
        wr64(h+24,id); wr32(h+32,c->endpoint_generation); wr32(h+36,(uint32_t)take); wr64(h+40,generation);
        wr32(h+52,(uint32_t)n); wr32(h+56,(uint32_t)at);
        if (take) memcpy(h+64,(const uint8_t *)body+at,take);
        m.length=(uint32_t)(64+take);
        for (;;) {
            e=c->sys->send(c->sys->ctx,c->fd,&m);
            if (e!=-EAGAIN || c->sys->now_ms(c->sys->ctx)>=deadline) break;
            (void)c->sys->wait(c->sys->ctx,-1,1);
        }
        lgn_wipe(&m,sizeof(m));
        if (e) { lgn_client_close(c); return e==-EAGAIN ? -ETIMEDOUT : e; }
        at+=take;
        if (at>=n) break;
    }
    size_t got=0, total=0;
    for (;;) {
        int r=c->sys->recv(c->sys->ctx,c->fd,&m);
        if (r==-EAGAIN) {
            uint64_t now=c->sys->now_ms(c->sys->ctx);
            if (now>=deadline) { lgn_client_close(c); return -ETIMEDOUT; }
            r=c->sys->wait(c->sys->ctx,c->fd,(unsigned)(deadline-now));
            if (r<0) { lgn_client_close(c); return r; }
            continue;
        }
        if (r!=1) { lgn_client_close(c); return r ? r : -ESRCH; }
        const uint8_t *h=m.data;
        uint32_t bytes=rd32(h+36), length=rd32(h+52), offset=rd32(h+56);
        int32_t frame_status=(int32_t)rd32(h+48);
        if (m.fd_count || m.length<64 || m.length!=64+bytes || rd32(h)!=CSC1 || rd32(h+4)!=1 || rd32(h+8)!=64 ||
            rd32(h+12)!=c->service || rd32(h+16)!=op || rd32(h+20)!=2 || rd64(h+24)!=id ||
            rd32(h+32)!=c->endpoint_generation || bytes>CIUKI_SECURITY_SERVICE_FRAGMENT_MAX || rd32(h+60) ||
            offset!=got || (got && length!=total) || length>cap || bytes>length-offset || (frame_status && (length || bytes)) ||
            frame_status>0 || (!bytes && length)) {
            lgn_wipe(&m,sizeof(m)); lgn_client_close(c); return -EIO;
        }
        if (!got) total=length;
        if (bytes) memcpy((uint8_t *)reply+got,h+64,bytes);
        got+=bytes;
        lgn_wipe(&m,sizeof(m));
        if (frame_status) { *status=frame_status; return 0; }
        if (got>=total) break;
    }
    *used=got;
    return 0;
}
