/* Resolved component policy. No storage I/O, allocation or locale folding on
 * warm authorization. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/security.h>
#include "security_policy.h"
#include "owner.h"
#include "path.h"
#include "vfs.h"

static bool administrator(const struct ciuki_credential *c)
{
    if (!c || c->group_count>CIUKI_SECURITY_GROUP_MAX) return false;
    if (!c->uid) return true;
    for (unsigned i=0;i<c->group_count;i++)
        if (c->groups[i]==CIUKI_GID_ADMINISTRATORS) return true;
    return false;
}
static bool hive_backing(const char *name)
{
    static const char *const names[]={"SYSTEM.CRG","SYSTEM.CRG.0","SYSTEM.CRG.1",
                                     "USER.CRG","USER.CRG.0","USER.CRG.1"};
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++) if(path_equal(name,names[i])) return true;
    return false;
}
/* ---- sec-sysvc2: boot inputs among the machine settings (rule table,
 * "files consumed as privileged configuration during the next boot"): the
 * loader's and kernel's HWPROF.CFG in C:\SYSTEM, and in C:\SYSTEM\SETTINGS
 * performance.cfg, its hwprofiles.cfg fallback and the display attempt
 * files (accel*.cfg: the DM8 keyed markers, the shared accel.cfg and the
 * retry request, plus each registered driver's declared attempt files).
 * Sealed by name like the hive slots, without spending the bounded trie;
 * administrators change them through the SYSTEM SETTING service. */
extern bool display_attempt_file(const char *) __attribute__((weak));
static bool node_named(const struct policy_volume *v,uint16_t trie,const char *component,const char *parent)
{
    if (trie>=v->count || !trie || !path_equal(v->rules[trie].component,component)) return false;
    uint16_t up=v->rules[trie].parent;
    if (!parent) return up==0;
    return up && up<v->count && v->rules[up].parent==0 && path_equal(v->rules[up].component,parent);
}
static bool boot_input(const struct policy_volume *v,const struct policy_label *p,const char *name)
{
    if (node_named(v,p->trie,"System",0)) return path_equal(name,"HWPROF.CFG");
    if (!node_named(v,p->trie,"Settings","System")) return false;
    return path_equal(name,"performance.cfg") || path_equal(name,"hwprofiles.cfg") || path_equal(name,"ipx.cfg") ||
           path_wildcard("accel*.cfg",name) || (display_attempt_file && display_attempt_file(name)) ||
           /* sec-k08: clock.cfg, the time zone store read at boot (S2-5), and
            * power.cfg, the machine power buttons every desktop reads (U4-6). */
           path_equal(name,"clock.cfg") || path_equal(name,"power.cfg");
}
/* ---- end sec-sysvc2 ---- */
uint64_t policy_epoch(const struct policy_volume *v)
{
    if(!v) return 0;
    uint64_t owner=v->owners?v->owners->sequence:0;
    return owner>UINT64_MAX-v->epoch?0:v->epoch+owner;
}
int policy_rule_add(struct policy_volume *v,const char *path,enum policy_kind kind)
{
    if (!v || !path || *path++!='/' || !*path || kind>POLICY_DENY) return -EINVAL;
    /* Build off-line, so a full table cannot publish a partial new prefix. */
    unsigned initial=v->count, parent=0;
    uint16_t nodes[POLICY_RULES]; unsigned depth=0;
    while (*path) {
        char name[64]; unsigned n=0;
        while (*path && *path!='/') {
            if (n+1==sizeof(name)) { v->count=initial; return -ENAMETOOLONG; }
            name[n++]=*path++;
        }
        name[n]=0;
        uint16_t units[256]; unsigned count;
        if (!n || path_validate_name(name,units,&count)) { v->count=initial; return -EINVAL; }
        unsigned i;
        for (i=1;i<v->count;i++) if (v->rules[i].parent==parent && path_equal(v->rules[i].component,name)) break;
        if (i==v->count) {
            if (i==POLICY_RULES) { v->count=initial; return -ENOSPC; }
            v->rules[i]=(struct policy_rule){.parent=(uint16_t)parent,.kind=POLICY_PUBLIC};
            memcpy(v->rules[i].component,name,n+1); v->count++;
        }
        nodes[depth++]=(uint16_t)i; parent=i;
        if (*path) { path++; if (!*path) { v->count=initial; return -EINVAL; } }
    }
    v->rules[parent].kind=(uint8_t)kind;
    /* No ancestor may be replaced to shed a more-specific policy. */
    for (unsigned i=0;i<depth;i++) v->rules[nodes[i]].anchor=1;
    if (v->epoch==UINT64_MAX) { v->ready=false; return -EOVERFLOW; }
    v->epoch++; return 0;
}
int policy_init(struct policy_volume *v,uint64_t id,uint64_t mount,bool system,
                 bool removable,bool readonly,struct owner_store *owners)
{
    if (!v || !id || !mount || (system && removable) || (owners && owners->volume!=id)) return -EINVAL;
    memset(v,0,sizeof(*v)); v->id=id; v->mount=mount; v->epoch=1;
    v->system=system; v->removable=removable; v->readonly=readonly;
    v->owners=owners; v->count=1; v->rules[0].kind=POLICY_ROOT;
    static const struct { const char *path; enum policy_kind kind; } defaults[]={
        {"/Windows",POLICY_PROGRAM},{"/Windows/Profiles",POLICY_PROFILES},
        {"/Windows/Profiles/Default",POLICY_PROGRAM},
        {"/Windows/Profiles/All Users",POLICY_PROGRAM},
        {"/System",POLICY_PROGRAM},{"/System/Security",POLICY_SECRET},
        {"/System/Services",POLICY_SEALED},{"/System/Drivers",POLICY_SEALED},{"/System/Registry",POLICY_SECRET},
        {"/System/Settings",POLICY_PROGRAM},{"/System/Logs",POLICY_SECRET},
        {"/System/Settings/Hardware",POLICY_SEALED},
        {"/System/Settings/users.dat",POLICY_SECRET},{"/System/Settings/users.new",POLICY_SECRET},
        {"/System/Settings/users.lock",POLICY_SECRET},
        {"/Windows/USER.CRG",POLICY_SECRET},
        {"/System/PAGEFILE.SWP",POLICY_SECRET},{"/System/VMM.ELF",POLICY_SEALED},
        {"/System/BOOT.CFG",POLICY_SEALED},{"/Programs",POLICY_PROGRAM},{"/Programs/setup",POLICY_SEALED},
        {"/Program Files",POLICY_PROGRAM},{"/Documents",POLICY_SHARED},
        {"/Documents/Desktop",POLICY_SHARED},
        {"/Documents/Desktop/Quake III Arena Demo",POLICY_PROGRAM},{"/CIUKILDR",POLICY_SEALED},
        {"/BOOT.CFG",POLICY_SEALED},
        /* sec-sysvc: System File Checker's baseline, the trust input of the
         * SYSTEM restore service; only the image builder writes it. */
        {"/System/SFC.LST",POLICY_SEALED},
        /* sec-sysvc2: Registry Checker's backups, the trust input of the
         * registry broker's restore (XP keeps System Restore's data in the
         * SYSTEM-only System Volume Information). */
        {"/System/SysBckup",POLICY_SECRET}
    };
    if (system) for (unsigned i=0;i<sizeof(defaults)/sizeof(*defaults);i++) {
        int e=policy_rule_add(v,defaults[i].path,defaults[i].kind); if (e) return e;
    }
    v->ready=true; return 0;
}
int policy_bind_locked(struct vfs *v,unsigned d,struct policy_volume *p)
{
    if (!v || d>=26 || !p || !p->ready || !vfs_drive_present(v,d)) return -EINVAL;
    if (v->policy[d] && v->policy[d]!=p) return -EBUSY;
    for(unsigned i=0;i<26;i++) if(i!=d && v->policy[i] && v->volumes[i] && v->volumes[d] &&
        v->volumes[i]!=v->volumes[d] && v->volumes[i]->dev==v->volumes[d]->dev &&
        v->volumes[i]->start==v->volumes[d]->start && v->volumes[i]->sectors==v->volumes[d]->sectors) return -EACCES;
    /* A second drive spelling of the same mounted volume cannot change class. */
    for (unsigned i=0;i<26;i++) if (i!=d && v->policy[i] &&
        ((v->volumes[i] && v->volumes[i]==v->volumes[d]) ||
         (v->volumes[i] && v->volumes[d] && v->volumes[i]->dev==v->volumes[d]->dev &&
          v->volumes[i]->start==v->volumes[d]->start && v->volumes[i]->sectors==v->volumes[d]->sectors) ||
         (v->ro_volumes[i] && v->ro_volumes[i]==v->ro_volumes[d]) || v->policy[i]->id==p->id) && v->policy[i]!=p) return -EACCES;
    v->policy[d]=p;
    if(v->volumes[d]) v->volumes[d]->strict_names=true;
    return 0;
}
void policy_unbind_locked(struct vfs *v,unsigned d)
{
    if (!v || d>=26) return;
    struct policy_volume *p=v->policy[d]; v->policy[d]=0;
    if (p) { p->ready=false; if (p->epoch!=UINT64_MAX) p->epoch++; }
}
int policy_root(const struct policy_volume *v,uint64_t object,struct policy_label *out)
{
    if (!v || !v->ready || !object || !out) return -EACCES;
    if(!policy_epoch(v)) return -EOVERFLOW;
    *out=(struct policy_label){.mount=v->mount,.epoch=policy_epoch(v),.object=object,
        .uid=CIUKI_UID_INVALID,.kind=v->readonly?POLICY_READONLY:v->removable?POLICY_REMOVABLE:POLICY_ROOT,.anchor=1};
    return 0;
}
int policy_child(const struct policy_volume *v,const struct policy_label *p,
                  const char *name,uint64_t object,uint64_t locator,struct policy_label *out)
{
    if (!v || !v->ready || !p || !out || !object || p->mount!=v->mount || p->epoch!=policy_epoch(v)) return -EACCES;
    uint16_t u[256]; unsigned n;
    if (path_validate_name(name,u,&n)) return -EILSEQ;
    struct policy_label l=*p; l.object=object; l.anchor=0;
    l.trie=UINT16_MAX;
    if (p->kind==POLICY_ROOT) l.kind=POLICY_PUBLIC;
    if (p->kind==POLICY_PROFILES) l.kind=POLICY_DENY;
    if (p->trie!=UINT16_MAX) for (unsigned i=1;i<v->count;i++)
        if (v->rules[i].parent==p->trie && path_equal(v->rules[i].component,name)) {
            l.trie=(uint16_t)i; l.anchor=v->rules[i].anchor;
            if (v->rules[i].kind!=POLICY_PUBLIC) l.kind=v->rules[i].kind;
            break;
        }
    /* sec-own: a transaction in flight (owners->busy) is not unavailable
     * metadata. Its in-flight names stay fenced by pending/root intents; the
     * non-preemptible kernel mutates the in-memory map between yields only. */
    if(p->kind==POLICY_ROOT && l.kind==POLICY_PUBLIC && !v->removable &&
        (!v->owners || !v->owners->ready)) l.kind=POLICY_DENY;
    if (locator) {
        if(p->kind!=POLICY_ROOT && p->kind!=POLICY_PROFILES) return -EIO;
        struct owner_record record;
        int e=owner_label(v->owners,locator,&record); if (e) return e;
        const char *leaf=record.binding.path+1;
        for(const char *q=leaf;*q;q++) if(*q=='/') leaf=q+1;
        if(record.binding.volume!=v->id || !path_equal(leaf,name) ||
           (p->kind==POLICY_PROFILES?!owner_profile_path(record.binding.path):leaf!=record.binding.path+1)) return -EIO;
        /* Installed anchors and explicit shared/program prefixes always win
         * over any historical creator record for the same root object. */
        if(l.trie==UINT16_MAX) {
            l.uid=record.binding.uid; l.owner_key=record.binding.key;
            l.kind=p->kind==POLICY_PROFILES?POLICY_PROFILE:POLICY_CREATOR;
            l.anchor=p->kind==POLICY_PROFILES;
        }
    }
    /* Fixed child rules keep all real hive slots and migration inputs sealed
     * without spending the bounded trie on individual slot filenames. Match
     * resolved names, including when reached through an 8.3 alias. */
    bool windows=p->trie<v->count && v->rules[p->trie].parent==0 &&
                 path_equal(v->rules[p->trie].component,"Windows");
    if((windows || (p->kind==POLICY_PROFILE && p->anchor)) && hive_backing(name)) {
        l.kind=POLICY_SECRET; l.anchor=1;
    }
    if (l.trie==UINT16_MAX && boot_input(v,p,name)) { l.kind=POLICY_SEALED; l.anchor=1; } /* sec-sysvc2 */
    /* Ownership never overrides a more specific service-only subtree. */
    if (p->kind==POLICY_PROFILE && p->anchor && p->trie!=POLICY_RULES && path_equal(name,"Settings")) { l.trie=POLICY_RULES; l.anchor=1; }
    if (p->trie==POLICY_RULES && (path_equal(name,"Registry") || path_equal(name,"Restart"))) {
        l.kind=POLICY_SECRET; l.anchor=1;
    }
    *out=l; return 0;
}
int policy_check(const struct policy_volume *v,const struct policy_label *l,
                  const struct security_subject *s,unsigned access)
{
    if (!s || !access || (access&~POLICY_ALL)) return -EACCES;
    if (!security_subject_live(s)) return -ESRCH;
    if (!v || !l || !v->ready || !l->object || l->mount!=v->mount || l->epoch!=policy_epoch(v)) return -EACCES;
    /* sec-k08 (coverage audit): a label kind outside the table is corrupt
     * metadata, never a public object; it denies every subject. */
    if (l->kind>POLICY_DENY) return -EACCES;
    const struct ciuki_credential *c=security_subject_credential(s);
    if (!c || c->group_count>8) return -EACCES;
    if ((l->kind==POLICY_PROFILE || l->kind==POLICY_CREATOR || l->kind==POLICY_PUBLIC) &&
        (!v->owners || !v->owners->ready)) return -EIO; /* sec-own: busy is not unavailable (policy_child) */
    if ((v->readonly || l->kind==POLICY_READONLY) && (access&POLICY_MUTATE)) return -EROFS;
    if (l->kind==POLICY_DENY) return -EACCES;
    if (!c->uid) return 0;
    bool admin=administrator(c), guest=c->uid==CIUKI_UID_GUEST;
    for (unsigned i=0;i<c->group_count;i++) if (c->groups[i]==CIUKI_GID_GUESTS) guest=true;
    if (l->kind==POLICY_SECRET) return -EACCES;
    if (l->kind==POLICY_SEALED && (access&POLICY_MUTATE)) return -EACCES;
    if (l->anchor && (access&(POLICY_REMOVE|POLICY_RENAME))) return -EACCES;
    if (l->kind==POLICY_PROFILE && !admin && c->uid!=l->uid) return -EACCES;
    if (!(access&POLICY_MUTATE)) return 0;
    if (l->kind==POLICY_PROFILE && (admin || c->uid==l->uid)) return 0;
    if (guest) return -EACCES;
    if (l->kind==POLICY_ROOT) {
        if (!v->owners || !v->owners->ready) return -EIO;
        /* Namespace publication also requires owner_root_reserve/finish. */
        if (access&~(POLICY_ADD|POLICY_READ|POLICY_TRAVERSE)) return admin?0:-EACCES;
        return 0;
    }
    if (admin && l->kind!=POLICY_PROFILES) return 0;
    if (l->kind==POLICY_CREATOR) return c->uid==l->uid?0:-EACCES;
    if (l->kind==POLICY_SHARED || l->kind==POLICY_REMOVABLE) return 0;
    return -EACCES;
}
