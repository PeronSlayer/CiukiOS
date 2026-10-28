/* Identity-bound joystick INI records. All storage runs in a dedicated worker.
 * Research/format: docs/design/peripheral-driver-coverage.md, f1-113b.
 * Newly authored, no donor implementation. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/sha256.h>
#include <ciuki/joystick_settings.h>
#include <ciuki/security.h> /* sec-user-05: retained scope subject */

/* Version 1 payloads use the native i686 little-endian fixed-width words.
 * No pointers, bools, padding or boot-local device numbers are serialized. */
struct calibration {
    uint8_t identity[32];
    uint32_t mask, mode;
    int32_t points[CIUKI_HID_AXES][3];
};
struct preference {
    uint32_t kind, mode; /* 0 default, 1 disabled, 2 identity route */
    uint8_t first[32], second[32];
};
struct calibration_slot {
    struct calibration value;
    uint32_t sequence, revision, saved, device, generation;
    bool used, loaded;
};
struct preference_slot {
    struct preference value;
    uint32_t sequence, revision, saved;
};
_Static_assert(sizeof(struct calibration)==148,"private calibration file layout");
_Static_assert(sizeof(struct preference)==72,"private preference file layout");
static struct calibration_slot controllers[JOY_SETTINGS_CONTROLLERS];
static struct preference_slot games[JOY_SETTINGS_GAMES+1];
static struct joystick_settings_io storage;
static bool started;
static int last_error;
static const char hex[]="0123456789abcdef";
static const char prefix[]="[Joystick]\nversion=1\nrecord=";

static bool nonzero(const uint8_t id[32])
{ uint8_t v=0; for (unsigned i=0;i<32;i++) v|=id[i]; return v!=0; }
static void encode_hex(char *out, const void *in, size_t n)
{
    const uint8_t *p=in;
    for (size_t i=0;i<n;i++) { out[i*2]=hex[p[i]>>4]; out[i*2+1]=hex[p[i]&15]; }
}
static int digit(char c)
{ for (unsigned i=0;i<16;i++) if (hex[i]==c) return (int)i; return -1; }
static bool decode_hex(void *out, const char *in, size_t n)
{
    uint8_t *p=out;
    for (size_t i=0;i<n;i++) {
        int a=digit(in[i*2]), b=digit(in[i*2+1]); if (a<0 || b<0) return false;
        p[i]=(uint8_t)(a*16+b);
    }
    return true;
}
static size_t encode(char text[512], const void *data, size_t bytes, uint32_t sequence)
{
    size_t n=sizeof(prefix)-1;
    memcpy(text,prefix,n);
    uint8_t seq[4]={sequence,sequence>>8,sequence>>16,sequence>>24};
    encode_hex(text+n,seq,4); n+=8;
    encode_hex(text+n,data,bytes); n+=bytes*2; text[n++]='\n';
    uint8_t digest[32]; sha256(text,n,digest);
    memcpy(text+n,"sha256=",7); n+=7; encode_hex(text+n,digest,32); n+=64; text[n++]='\n';
    return n;
}
static bool decode(const char *text, size_t n, void *data, size_t bytes, uint32_t *sequence)
{
    size_t body=sizeof(prefix)-1+8+bytes*2+1;
    if (n!=body+72 || memcmp(text,prefix,sizeof(prefix)-1) || text[body-1]!='\n' ||
        memcmp(text+body,"sha256=",7) || text[n-1]!='\n') return false;
    uint8_t digest[32], stored[32], seq[4];
    sha256(text,body,digest);
    if (!decode_hex(stored,text+body+7,32) || memcmp(digest,stored,32) ||
        !decode_hex(seq,text+sizeof(prefix)-1,4) ||
        !decode_hex(data,text+sizeof(prefix)-1+8,bytes)) return false;
    *sequence=(uint32_t)seq[0]|(uint32_t)seq[1]<<8|(uint32_t)seq[2]<<16|(uint32_t)seq[3]<<24;
    return *sequence!=0;
}
static void filename(char name[112], const char *stem, unsigned slot)
{ ksnprintf(name,112,"%s.%u.ini",stem,slot); }
/* Per-record double buffer. A partially written newest slot cannot displace
 * its previous committed value. No allocation and no unbounded parser. */
static int read_record(const char *stem, void *data, size_t bytes, uint32_t *sequence)
{
    char name[112], text[512]; uint8_t candidate[sizeof(struct calibration)];
    bool found=false; *sequence=0;
    int error=0;
    for (unsigned slot=0;slot<2;slot++) {
        filename(name,stem,slot); size_t n=0; uint32_t seq=0;
        int e=storage.read(storage.context,name,text,sizeof(text),&n);
        if (e==-ENOENT) continue;
        if (e || !decode(text,n,candidate,bytes,&seq) || (seq&1)!=slot) {
            klog("[joystick-settings] rejected record=%s error=%d; defaults/previous slot",name,e ? e : -EINVAL);
            error=e ? e : -EINVAL; continue;
        }
        if (!found || seq>*sequence) { memcpy(data,candidate,bytes); *sequence=seq; found=true; }
    }
    return found ? 0 : error ? error : -ENOENT;
}
static int write_record(const char *stem, const void *data, size_t bytes, uint32_t *sequence)
{
    if (*sequence==UINT32_MAX) return -EOVERFLOW;
    uint32_t seq=*sequence+1; char name[112], text[512];
    filename(name,stem,seq&1);
    size_t n=encode(text,data,bytes,seq);
    int e=storage.write(storage.context,name,text,n);
    if (!e) *sequence=seq;
    else klog("[joystick-settings] save failed record=%s error=%d",name,e);
    return e;
}
static void controller_stem(const uint8_t id[32], char stem[96])
{
    memcpy(stem,"joystick-",9); encode_hex(stem+9,id,32); stem[73]=0;
}
static bool valid_calibration(const struct calibration *v, const uint8_t id[32])
{
    if (memcmp(v->identity,id,32) || v->mask>>CIUKI_HID_AXES || v->mode>2) return false;
    for (unsigned a=0;a<CIUKI_HID_AXES;a++) {
        const int32_t *p=v->points[a];
        if (v->mask&(1u<<a)) { if (p[0]>=p[1] || p[1]>=p[2]) return false; }
        else if (p[0] || p[1] || p[2]) return false;
    }
    return true;
}
static bool valid_preference(const struct preference *p)
{
    if (p->kind>2 || p->mode>1) return false;
    if (p->kind!=2) return !p->mode && !nonzero(p->first) && !nonzero(p->second);
    return nonzero(p->first) && (p->mode || !nonzero(p->second));
}
static struct calibration_slot *controller(const uint8_t id[32])
{
    struct calibration_slot *empty=0;
    for (unsigned i=0;i<JOY_SETTINGS_CONTROLLERS;i++) {
        struct calibration_slot *s=&controllers[i];
        if (s->used && !memcmp(s->value.identity,id,32)) return s;
        if (!s->used && !empty) empty=s;
    }
    if (empty) { empty->used=true; memcpy(empty->value.identity,id,32); }
    return empty;
}
static int identity(uint32_t device, uint32_t generation, uint8_t id[32])
{
    int e=joystick_identity(device,generation,id); if (e) return e;
    /* Broken devices with duplicate serials cannot safely share calibration.
     * Refuse ambiguous identities instead of silently choosing by locator. */
    for (unsigned d=1;d<=CIUKI_JOYSTICK_LAST;d++) if (d!=device) {
        struct ciuki_joystick_control c={.version=1,.size=sizeof(c),.operation=CIUKI_JOY_QUERY,.device=d};
        uint8_t other[32];
        if (!joystick_control(&c) && !joystick_identity(d,c.generation,other) && !memcmp(id,other,32)) return -ENODEV;
    }
    return 0;
}
int joystick_settings_calibrate(const struct ciuki_joystick_control *c)
{
    if (!started) return -EAGAIN;
    uint8_t id[32]; int e=identity(c->device,c->generation,id); if (e) return e;
    struct calibration_slot *s=controller(id); if (!s) return -ENOSPC;
    if (!s->loaded) return -EAGAIN;
    struct ciuki_hid_calibration cal={.version=1,.size=sizeof(cal),.device=c->device,.generation=c->generation,
        .axis=c->axis,.minimum=c->minimum,.centre=c->centre,.maximum=c->maximum};
    e=joystick_hid_query(INPUT_HID_CALIBRATE,&cal); if (e) return e;
    s->value.mask|=1u<<c->axis;
    int32_t *p=s->value.points[c->axis]; p[0]=c->minimum; p[1]=c->centre; p[2]=c->maximum;
    s->revision++; last_error=0; return 0;
}
int joystick_settings_mode(uint32_t device, uint32_t generation, unsigned mode)
{
    if (!started) return -EAGAIN;
    if (device<CIUKI_GAMEPORT_FIRST || mode>1) return -EINVAL;
    unsigned base=CIUKI_GAMEPORT_FIRST+((device-CIUKI_GAMEPORT_FIRST)&~1u);
    struct ciuki_hid_caps c={.device=base}; uint8_t id[32];
    int e=gameport_hid_query(INPUT_HID_GET_CAPS,&c); if (e) return e;
    e=joystick_identity(base,c.generation,id); if (e) return e;
    struct calibration_slot *s=controller(id); if (!s) return -ENOSPC;
    if (!s->loaded) return -EAGAIN;
    e=gameport_restore_mode(device,generation,mode); if (e) return e;
    /* One authoritative wiring value per physical port, always in child 0.
     * Two independently committed child values could disagree after a crash. */
    s->value.mode=mode+1; s->revision++;
    last_error=0; return 0;
}
int joystick_settings_route(unsigned game, const struct ciuki_joystick_control *c)
{
    if (!started) return -EAGAIN;
    if (game>JOY_SETTINGS_GAMES || c->mode>1 || (c->session && c->session!=joystick_session())) return -EINVAL;
    struct preference p={0};
    if (!c->device || c->device==CIUKI_JOY_DISABLED) {
        if (c->generation || c->second_device || c->second_generation || c->mode || c->session) return -EINVAL;
        p.kind=c->device==CIUKI_JOY_DISABLED ? 1 : 0;
    } else {
        struct joystick_route r={c->device,c->generation,c->second_device,c->second_generation,c->mode};
        int e=joystick_route_validate(&r); if (e) return e;
        e=identity(c->device,c->generation,p.first); if (e) return e;
        if (c->second_device) { e=identity(c->second_device,c->second_generation,p.second); if (e) return e; }
        p.kind=2; p.mode=c->mode;
    }
    games[game].value=p; games[game].revision++; last_error=0; return 0;
}
void joystick_settings_status(struct ciuki_joystick_control *c)
{
    c->minimum=last_error;
    for (unsigned i=0;i<=JOY_SETTINGS_GAMES;i++) c->buttons+=games[i].revision!=games[i].saved;
    for (unsigned i=0;i<JOY_SETTINGS_CONTROLLERS;i++) c->buttons+=controllers[i].revision!=controllers[i].saved;
}
static int resolve(const uint8_t id[32], uint32_t *device, uint32_t *generation)
{
    *device=*generation=0;
    for (unsigned d=1;d<=CIUKI_JOYSTICK_LAST;d++) {
        struct ciuki_joystick_control c={.version=1,.size=sizeof(c),.operation=CIUKI_JOY_QUERY,.device=d};
        uint8_t current[32];
        if (!joystick_control(&c) && !joystick_identity(d,c.generation,current) && !memcmp(id,current,32)) {
            if (*device) return -ENODEV;
            *device=d; *generation=c.generation;
        }
    }
    return *device ? 0 : -ENODEV;
}
static bool route(const struct preference *p, struct joystick_route *r)
{
    *r=(struct joystick_route){0};
    if (p->kind!=2) return false;
    r->mode=p->mode;
    if (!resolve(p->first,&r->device,&r->generation) &&
        (!nonzero(p->second) || !resolve(p->second,&r->second_device,&r->second_generation))) return false;
    struct ciuki_joystick_control first={.version=1,.size=sizeof(first),.operation=CIUKI_JOY_NEXT};
    *r=(struct joystick_route){0};
    if (!joystick_control(&first)) { r->device=first.device; r->generation=first.generation; }
    return true;
}
bool joystick_settings_game(unsigned game, struct joystick_route *r)
{
    if (!started || game>JOY_SETTINGS_GAMES) return false;
    const struct preference *p=&games[game].value;
    if (game && !p->kind) p=&games[0].value;
    return route(p,r);
}
int joystick_settings_get_game(struct ciuki_joystick_control *c)
{
    if (!started) return -EAGAIN;
    if (!c->axis || c->axis>JOY_SETTINGS_GAMES) return -EINVAL;
    const struct preference *p=&games[c->axis].value;
    struct joystick_route r={0};
    c->attached=p->kind;
    if (p->kind==1) c->device=CIUKI_JOY_DISABLED;
    else if (p->kind==2) {
        if (route(p,&r)) c->attached=3;
        c->device=r.device; c->generation=r.generation;
        c->second_device=r.second_device; c->second_generation=r.second_generation; c->mode=r.mode;
        c->session=joystick_session();
    }
    return 0;
}
/* --- sec-user-05 (user-security-contract.md K-Q8; sec-k04 open item): one
 * settings scope is active at a time. Scope 0 is the machine store (SYSTEM
 * and the boot state). An interactive session's scope is its profile store
 * (joystick_storage.c), which reads the machine record of any controller or
 * game the profile has never saved and writes only to the profile, as that
 * session's retained subject. Interactive CALIBRATE/ROUTE/CONFIGURE/
 * SAVE_GAME therefore change and persist the user's own preferences; the
 * machine defaults stay SYSTEM-only. Storage I/O stays serialized: the
 * worker's pass and a scope switch exclude each other through `busy`. A
 * controller with no record in either store keeps its current runtime
 * calibration until it is attached again (the drivers expose no default
 * reset). --- */
static struct joystick_settings_io machine_storage;
static uint32_t scope_session, busy;
static struct security_subject *scope_subject;
#pragma weak security_subject_get
#pragma weak security_subject_put
#pragma weak security_subject_live
/* Installed by the settings worker (gameport.c); host fixtures never wait. */
void (*joystick_settings_pause)(uint32_t ms);
static bool busy_acquire(unsigned attempts)
{
    for (unsigned i=0;;i++) {
        uint32_t expected=0;
        if (__atomic_compare_exchange_n(&busy,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return true;
        if (i>=attempts || !joystick_settings_pause) return false;
        joystick_settings_pause(10);
    }
}
static void busy_release(void) { __atomic_store_n(&busy,0,__ATOMIC_RELEASE); }
static void load_games(struct preference_slot out[JOY_SETTINGS_GAMES+1], bool legacy);
static void save_dirty(void);
uint32_t joystick_settings_scope_session(void)
{ uint32_t flags=irq_save(), session=scope_session; irq_restore(flags); return session; }
static bool scope_live(void)
{ return !scope_session || !security_subject_live || security_subject_live(scope_subject); }
/* Caller holds `busy`. A personal scope retains the session desktop's subject
 * (alive for the whole session): every load and save runs as that user.
 * `bind` prepares the new store; nothing can fail after it succeeds. */
static int scope_switch(uint32_t session, struct security_subject *subject,
                        int (*bind)(void *, struct joystick_settings_io *), void *ctx)
{
    struct joystick_settings_io next=machine_storage;
    int e=session ? security_subject_get(subject) : 0;
    if (e) return e;
    save_dirty(); /* the old scope's pending changes, to its own store */
    if (session && (e=bind(ctx,&next))) { security_subject_put(subject); return e; }
    static struct preference_slot loaded[JOY_SETTINGS_GAMES+1];
    struct joystick_settings_io previous=storage;
    storage=next; load_games(loaded,!session); storage=previous;
    uint32_t flags=irq_save();
    struct security_subject *old=scope_subject;
    scope_session=session; scope_subject=session ? subject : 0; storage=next;
    memset(controllers,0,sizeof(controllers)); memcpy(games,loaded,sizeof(games)); last_error=0;
    irq_restore(flags);
    if (old && security_subject_put) security_subject_put(old);
    klog("[joystick-settings] scope session=%u",session);
    return 0;
}
int joystick_settings_scope(uint32_t session, struct security_subject *subject,
                            int (*bind)(void *, struct joystick_settings_io *), void *ctx)
{
    if (!started) return -EAGAIN;
    uint32_t flags=irq_save();
    bool current=scope_session==session;
    irq_restore(flags);
    if (current && scope_live()) return 0;
    if (session && (!subject || !bind || !security_subject_get || !security_subject_put)) return -EOPNOTSUPP;
    if (!busy_acquire(300)) return -EAGAIN;
    flags=irq_save(); current=scope_session==session; irq_restore(flags);
    int e=current && scope_live() ? 0 : scope_switch(session,subject,bind,ctx);
    busy_release();
    return e;
}
/* Session teardown (sec-k07) or a revoked subject: back to the machine store. */
void joystick_settings_session_end(uint32_t session)
{
    if (session && joystick_settings_scope_session()==session) (void)joystick_settings_scope(0,0,0,0);
}
/* --- end sec-user-05 --- */
static void load_games(struct preference_slot out[JOY_SETTINGS_GAMES+1], bool legacy)
{
    memset(out,0,sizeof(struct preference_slot)*(JOY_SETTINGS_GAMES+1));
    for (unsigned i=0;i<=JOY_SETTINGS_GAMES;i++) {
        char stem[96]; ksnprintf(stem,sizeof(stem),"dos-game-%u-joystick",i);
        struct preference_slot *s=&out[i];
        int e=read_record(stem,&s->value,sizeof(s->value),&s->sequence);
        if (e || !valid_preference(&s->value)) {
            if (!e) klog("[joystick-settings] invalid route game=%u; defaults",i);
            memset(&s->value,0,sizeof(s->value));
        }
        if (legacy && e==-ENOENT && i) {
            /* f1-113 had exactly six boot-local decimal words. Only default
             * and off are safely portable; never guess a new hardware ID. */
            char name[112], old[128]; size_t n=0;
            ksnprintf(name,sizeof(name),"dos-game-%u-joystick.cfg",i);
            if (!storage.read(storage.context,name,old,sizeof(old),&n)) {
                static const char off[]="4294967295 0 0 0 0 0\n";
                static const char defaults[]="0 0 0 0 0 0\n";
                if (n==sizeof(off)-1 && !memcmp(old,off,n)) { s->value.kind=1; s->revision++; }
                else if (n==sizeof(defaults)-1 && !memcmp(old,defaults,n)) s->revision++;
                else klog("[joystick-settings] legacy boot-local/corrupt route game=%u rejected; defaults",i);
            }
        }
    }
}
void joystick_settings_start(const struct joystick_settings_io *io)
{
    /* Only initialization or the host's reboot model calls this function. */
    started=false; memset(controllers,0,sizeof(controllers)); memset(games,0,sizeof(games));
    last_error=0; storage=*io; machine_storage=*io; /* sec-user-05 */
    if (scope_subject && security_subject_put) security_subject_put(scope_subject);
    scope_session=0; scope_subject=0;
    load_games(games,true);
    started=true;
}
static void attach(uint32_t device, uint32_t generation)
{
    for (unsigned i=0;i<JOY_SETTINGS_CONTROLLERS;i++)
        if (controllers[i].used && controllers[i].device==device && controllers[i].generation==generation) return;
    uint8_t id[32]; uint32_t flags=irq_save();
    int identity_error=identity(device,generation,id);
    struct calibration_slot *s=identity_error ? 0 : controller(id); irq_restore(flags);
    if (!s || (s->device==device && s->generation==generation)) return;
    if (!s->loaded) {
        struct calibration v={0}; uint32_t seq=0; char stem[96]; controller_stem(id,stem);
        int e=read_record(stem,&v,sizeof(v),&seq);
        flags=irq_save();
        if (!e && valid_calibration(&v,id)) s->value=v;
        else if (!e) klog("[joystick-settings] invalid calibration; defaults device=%u",device);
        s->sequence=seq; s->loaded=true; irq_restore(flags);
    }
    flags=irq_save();
    /* Revalidate after sleeping storage. A different generation cannot inherit
     * a stale attachment's configuration. Mode restore invalidates generation
     * and is followed by rediscovery on a later worker pass. */
    uint8_t current[32];
    if (joystick_identity(device,generation,current) || memcmp(id,current,32)) { irq_restore(flags); return; }
    if (device>=CIUKI_GAMEPORT_FIRST) {
        struct ciuki_joystick_control c={.version=1,.size=sizeof(c),.device=device,.generation=generation};
        if (joystick_control(&c)) {
            s->device=device; s->generation=generation; irq_restore(flags); return;
        }
        if (!((device-CIUKI_GAMEPORT_FIRST)&1) && s->value.mode && c.mode!=s->value.mode-1) {
            (void)gameport_restore_mode(device,generation,s->value.mode-1); irq_restore(flags); return;
        }
    }
    for (unsigned a=0;a<CIUKI_HID_AXES;a++) if (s->value.mask&(1u<<a)) {
        const int32_t *p=s->value.points[a];
        struct ciuki_hid_calibration c={.version=1,.size=sizeof(c),.device=device,.generation=generation,
            .axis=a,.minimum=p[0],.centre=p[1],.maximum=p[2]};
        if (joystick_hid_query(INPUT_HID_CALIBRATE,&c))
            klog("[joystick-settings] calibration rejected by current capabilities device=%u axis=%u; default",device,a);
    }
    s->device=device; s->generation=generation; irq_restore(flags);
}
static void save_dirty(void)
{
    for (unsigned i=0;i<JOY_SETTINGS_CONTROLLERS;i++) {
        struct calibration_slot *s=&controllers[i];
        uint32_t flags=irq_save(), rev=s->revision, seq=s->sequence;
        struct calibration copy=s->value; bool dirty=rev!=s->saved; irq_restore(flags);
        if (!dirty) continue;
        char stem[96]; controller_stem(copy.identity,stem);
        int e=write_record(stem,&copy,sizeof(copy),&seq);
        flags=irq_save(); last_error=e;
        if (!e) { s->sequence=seq; s->saved=rev; } irq_restore(flags);
        if (e) return; /* retry only on the next bounded service interval */
    }
    for (unsigned i=0;i<=JOY_SETTINGS_GAMES;i++) {
        struct preference_slot *s=&games[i];
        uint32_t flags=irq_save(), rev=s->revision, seq=s->sequence;
        struct preference copy=s->value; bool dirty=rev!=s->saved; irq_restore(flags);
        if (!dirty) continue;
        char stem[96]; ksnprintf(stem,sizeof(stem),"dos-game-%u-joystick",i);
        int e=write_record(stem,&copy,sizeof(copy),&seq);
        flags=irq_save(); last_error=e;
        if (!e) { s->sequence=seq; s->saved=rev; } irq_restore(flags);
        if (e) return;
    }
}
void joystick_settings_service(void)
{
    if (!started || !busy_acquire(0)) return; /* sec-user-05: a scope switch owns storage */
    if (!scope_live()) (void)scope_switch(0,0,0,0); /* the session ended: machine store */
    /* NEXT reads retained cache only, never polls hardware. */
    uint32_t cursor=0;
    for (;;) {
        struct ciuki_joystick_control c={.version=1,.size=sizeof(c),.operation=CIUKI_JOY_NEXT,.device=cursor};
        uint32_t flags=irq_save(); int e=joystick_control(&c); irq_restore(flags);
        if (e) break;
        cursor=c.device; attach(c.device,c.generation);
    }
    /* Load inactive children too; child 0 owns the physical wiring setting. */
    for (unsigned d=CIUKI_GAMEPORT_FIRST;d<=CIUKI_JOYSTICK_LAST;d++) {
        struct ciuki_hid_caps c={.device=d};
        uint32_t flags=irq_save(); int e=gameport_hid_query(INPUT_HID_GET_CAPS,&c); irq_restore(flags);
        if (!e && !c.attached) attach(d,c.generation);
    }
    save_dirty();
    busy_release();
}
