/* Creator Owner side records: durable deny-first namespace transactions.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "owner_internal.h"
#include "path.h"
#include <ciuki/security.h>
static int root_name(const char *name)
{
    if(!name || name[0]!='/' || !name[1] || strlen(name)>=64) return -EINVAL;
    for(const char *p=name+1;*p;p++) if(*p=='/') return -EINVAL;
    uint16_t u[256]; unsigned n; return path_validate_name(name+1,u,&n)?-EINVAL:0;
}
static int root_subject(const struct security_subject *subject,uint32_t owner)
{
    if(!subject) return -EACCES;
    if(!security_subject_live(subject)) return -ESRCH;
    const struct ciuki_credential *c=security_subject_credential(subject);
    if(!c || c->group_count>8) return -EACCES;
    if(!c->uid) return 0;
    bool admin=false, guest=c->uid==501;
    for(unsigned i=0;i<c->group_count;i++) {
        admin|=c->groups[i]==544; guest|=c->groups[i]==546;
    }
    if(guest) return -EACCES;
    if(owner!=CIUKI_UID_INVALID && c->uid!=owner && !admin) return -EACCES;
    return 0;
}
static bool root_enter(struct owner_store *s)
{ bool expected=false; return __atomic_compare_exchange_n(&s->busy,&expected,true,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED); }
static void root_leave(struct owner_store *s) { __atomic_store_n(&s->busy,false,__ATOMIC_RELEASE); }
int owner_root_reserve(struct owner_store *s,const struct security_subject *subject,const char *name,const uint8_t proof[32],struct owner_record *out)
{
    int e=root_subject(subject,CIUKI_UID_INVALID); if(!e) e=root_name(name); if(e) return e;
    if(!s || !s->ready || !proof || !out || ozero(proof,32)) return -EIO;
    if(!root_enter(s)) return -EBUSY;
    if(s->active || s->root_key || s->candidate) e=-EBUSY;
    int occupied=owner_root_occupied(s,s->volume,name);
    if(!e && occupied) e=occupied<0?occupied:-EEXIST;
    if(!e && s->root_count==OWNER_ROOTS) e=-ENOSPC;
    if(!e && (s->key_high==UINT64_MAX || s->locator_high==UINT64_MAX || s->generation_high==UINT64_MAX)) e=-EOVERFLOW;
    if(!e) {
        struct owner_record *r=&s->roots[s->root_count++]; memset(r,0,sizeof(*r));
        r->binding=(struct owner_binding){.uid=security_subject_credential(subject)->uid,.action=OWNER_CREATE,
            .key=++s->key_high,.locator=++s->locator_high,.generation=++s->generation_high,.volume=s->volume};
        memcpy(r->binding.path,name,strlen(name)+1); memcpy(r->proof,proof,32); r->pending=true;
        s->root_key=r->binding.key; s->root_operation=1; memcpy(s->root_new,r->binding.path,64);
        e=owner_persist(s); if(!e) *out=*r;
    }
    root_leave(s); return e;
}
int owner_root_change(struct owner_store *s,const struct security_subject *subject,uint64_t key,const char *name,const uint8_t proof[32])
{
    if(!s || !s->ready || !proof || ozero(proof,32)) return -EIO;
    if(!root_enter(s)) return -EBUSY;
    struct owner_record *r=0; for(unsigned i=0;i<s->root_count;i++) if(s->roots[i].binding.key==key) { r=&s->roots[i]; break; }
    int e=!r || r->retired || r->pending?-EIO:root_subject(subject,r->binding.uid);
    if(!e && (s->active || s->root_key || s->candidate)) e=-EBUSY;
    if(!e && memcmp(r->proof,proof,32)) e=-EIO;
    if(!e && name) e=root_name(name);
    if(!e && name && owner_root_occupied(s,s->volume,name)) e=-EEXIST;
    if(!e) {
        s->root_key=key; s->root_operation=name?2:3; memcpy(s->root_old,r->binding.path,64);
        memset(s->root_new,0,64); if(name) memcpy(s->root_new,name,strlen(name)+1);
        r->pending=true; e=owner_persist(s);
    }
    root_leave(s); return e;
}
int owner_root_finish(struct owner_store *s,uint64_t key,const uint8_t proof[32],bool abort)
{
    if(!s || !s->ready || !key || key!=s->root_key || (!abort && (!proof || ozero(proof,32)))) return -EIO;
    if(!root_enter(s)) return -EBUSY;
    struct owner_record *r=0; for(unsigned i=0;i<s->root_count;i++) if(s->roots[i].binding.key==key) { r=&s->roots[i]; break; }
    int e=0;
    if(!r || !r->pending) e=-EIO;
    else {
        if(s->root_operation==1 && abort) r->retired=true;
        if(!abort) {
            if(s->root_operation==3) r->retired=true;
            else memcpy(r->binding.path,s->root_new,64);
            memcpy(r->proof,proof,32);
        }
        r->pending=false; s->root_key=0; s->root_operation=0; memset(s->root_old,0,64); memset(s->root_new,0,64);
        e=owner_persist(s);
    }
    root_leave(s); return e;
}
