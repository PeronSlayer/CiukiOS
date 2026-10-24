/* SPDX-License-Identifier: MIT */
#include "users.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

void users_erase(void *p,size_t n)
{ volatile unsigned char *s=p; while (n--) *s++=0; }
static unsigned lower(unsigned c) { return c>='A' && c<='Z' ? c+32 : c; }
static int equal(const char *a,const char *b)
{ while (*a && lower((unsigned char)*a)==lower((unsigned char)*b)) { a++; b++; } return !*a && !*b; }
int users_name_valid(const char *s)
{
    if (!s || !*s || equal(s,"Default")) return 0;
    unsigned n=0;
    for (;*s;s++,n++) if (n>=USERS_NAME-1 || !((lower((unsigned char)*s)>='a' && lower((unsigned char)*s)<='z') ||
        (*s>='0' && *s<='9') || *s=='_' || *s=='-')) return 0;
    return 1;
}
static int password_valid(const char *s)
{
    for (unsigned i=0;i<USERS_PASSWORD;i++) {
        if (!s[i]) return 1;
        if ((unsigned char)s[i]<32 || (unsigned char)s[i]>126) return 0;
    }
    return 0;
}
/* PBKDF2, one SHA-256 block. Passwords fit the HMAC block size. */
static void hmac(const char *password,const void *data,size_t n,uint8_t out[32])
{
    uint8_t pad[64], inner[32]; struct sha256_ctx c;
    size_t length=strlen(password);
    memset(pad,0,sizeof(pad)); memcpy(pad,password,length);
    for (unsigned i=0;i<64;i++) pad[i]^=0x36;
    sha256_init(&c); sha256_update(&c,pad,64); sha256_update(&c,data,n); sha256_final(&c,inner);
    for (unsigned i=0;i<64;i++) pad[i]^=0x36^0x5c;
    sha256_init(&c); sha256_update(&c,pad,64); sha256_update(&c,inner,32); sha256_final(&c,out);
    users_erase(pad,sizeof(pad)); users_erase(inner,sizeof(inner)); users_erase(&c,sizeof(c));
}
void users_derive(const char *password,const uint8_t salt[16],unsigned iterations,uint8_t out[32])
{
    uint8_t input[20], u[32]; memcpy(input,salt,16);
    input[16]=input[17]=input[18]=0; input[19]=1;
    hmac(password,input,sizeof(input),u); memcpy(out,u,32);
    for (unsigned i=1;i<iterations;i++) {
        hmac(password,u,32,u);
        for (unsigned j=0;j<32;j++) out[j]^=u[j];
    }
    users_erase(u,sizeof(u));
}
static int verify(const struct users_record *r,const char *password)
{
    uint8_t out[32]; unsigned diff=0;
    users_derive(password,r->salt,USERS_ITERATIONS,out);
    for (unsigned j=0;j<32;j++) diff|=out[j]^r->verifier[j];
    users_erase(out,sizeof(out)); return !diff;
}
void users_snapshot(const struct users_db *db,struct users_view *v)
{
    memset(v,0,sizeof(*v)); v->count=db->count; v->flags=db->flags;
    for (unsigned i=0;i<db->count;i++) { v->ids[i]=db->records[i].id; memcpy(v->names[i],db->records[i].name,USERS_NAME); }
}
int users_apply(struct users_db *db,const struct users_request *q,const uint8_t salt[16],struct users_view *v)
{
    if (!memchr(q->name,0,sizeof(q->name)) || !memchr(q->current,0,sizeof(q->current)) ||
        !password_valid(q->password) || !password_valid(q->confirm) || !password_valid(q->old_password)) return -EINVAL;
    unsigned i=0; while (i<db->count && !equal(q->name,db->records[i].name)) i++;
    uint32_t authenticated=0;
    switch (q->operation) {
    case USERS_READ: break;
    case USERS_POLICY:
        if (q->flags&~USERS_FLAGS) return -EINVAL;
        db->flags=q->flags; break;
    case USERS_CREATE: {
        if (!users_name_valid(q->name) || strcmp(q->password,q->confirm) || !salt) return -EINVAL;
        if (i<db->count) return -EEXIST;
        if (db->count==USERS_MAX) return -ENOSPC;
        if (!db->next_id) db->next_id=1;
        if (db->next_id==UINT32_MAX) return -EOVERFLOW;
        struct users_record *r=&db->records[db->count]; memset(r,0,sizeof(*r));
        r->id=db->next_id++; strcpy(r->name,q->name); memcpy(r->salt,salt,16);
        users_derive(q->password,r->salt,USERS_ITERATIONS,r->verifier); db->count++; break;
    }
    case USERS_DELETE:
        if (i==db->count) return -ENOENT;
        if (equal(q->name,q->current)) return -EBUSY;
        memmove(db->records+i,db->records+i+1,(db->count-i-1)*sizeof(*db->records));
        users_erase(&db->records[--db->count],sizeof(*db->records)); break;
    case USERS_CHANGE:
        if (i==db->count) return -ENOENT;
        if (!equal(q->name,q->current)) return -EACCES;
        if (strcmp(q->password,q->confirm) || !salt) return -EINVAL;
        if (!verify(&db->records[i],q->old_password)) return -EACCES;
        memcpy(db->records[i].salt,salt,16);
        users_derive(q->password,salt,USERS_ITERATIONS,db->records[i].verifier); break;
    case USERS_AUTH:
        if (i==db->count || !verify(&db->records[i],q->password)) return -EACCES;
        authenticated=db->records[i].id; break;
    default: return -EINVAL;
    }
    users_snapshot(db,v); v->authenticated=authenticated; return 0;
}
static void put(unsigned char *p,uint32_t x)
{ for (unsigned i=0;i<4;i++) p[i]=(unsigned char)(x>>(8*i)); }
static uint32_t get(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
#define RECORD_BYTES 84u
size_t users_encode(const struct users_db *db,unsigned char *out,size_t cap)
{
    size_t n=24+RECORD_BYTES*db->count+32;
    if (db->count>USERS_MAX || cap<n) return 0;
    memset(out,0,n); memcpy(out,"CIUKIUSR",8); put(out+8,1); put(out+12,db->flags); put(out+16,db->next_id); put(out+20,db->count);
    for (unsigned i=0;i<db->count;i++) {
        unsigned char *p=out+24+RECORD_BYTES*i; const struct users_record *r=&db->records[i];
        put(p,r->id); memcpy(p+4,r->name,32); memcpy(p+36,r->salt,16); memcpy(p+52,r->verifier,32);
    }
    sha256(out,n-32,out+n-32); return n;
}
int users_decode(struct users_db *db,const unsigned char *in,size_t n)
{
    if (n<56 || memcmp(in,"CIUKIUSR",8) || get(in+8)!=1 || get(in+20)>USERS_MAX ||
        n!=24+RECORD_BYTES*get(in+20)+32 || (get(in+12)&~USERS_FLAGS)) return -EINVAL;
    uint8_t sum[32]; sha256(in,n-32,sum); if (memcmp(sum,in+n-32,32)) return -EINVAL;
    memset(db,0,sizeof(*db)); db->count=get(in+20); db->flags=get(in+12); db->next_id=get(in+16);
    if (!db->next_id) return -EINVAL;
    for (unsigned i=0;i<db->count;i++) {
        const unsigned char *p=in+24+RECORD_BYTES*i; struct users_record *r=&db->records[i];
        r->id=get(p); memcpy(r->name,p+4,32); memcpy(r->salt,p+36,16); memcpy(r->verifier,p+52,32);
        if (!memchr(r->name,0,32) || !users_name_valid(r->name) || !r->id || r->id>=db->next_id) return -EINVAL;
        for (unsigned j=0;j<i;j++) if (r->id==db->records[j].id || equal(r->name,db->records[j].name)) return -EINVAL;
    }
    return 0;
}
void users_profile(struct user_profile *p,const char *name,uint32_t id,unsigned flags)
{
    memset(p,0,sizeof(*p)); snprintf(p->name,sizeof(p->name),"%s",name);
    if (id && (flags&USERS_PROFILES)) {
        snprintf(p->root,sizeof(p->root),"/Windows/Profiles/u%08x",id);
        snprintf(p->documents,sizeof(p->documents),"/Windows/Profiles/u%08x/Documents",id);
        snprintf(p->desktop,sizeof(p->desktop),"/Windows/Profiles/u%08x/Documents/Desktop",id);
        snprintf(p->programs,sizeof(p->programs),"/Windows/Profiles/u%08x/Start Menu/Programs",id);
        snprintf(p->settings,sizeof(p->settings),"/Windows/Profiles/u%08x/desktop.cfg",id);
        snprintf(p->temporary,sizeof(p->temporary),"/Windows/Profiles/u%08x/desktop.new",id);
    } else {
        strcpy(p->documents,"/Documents"); strcpy(p->desktop,"/Documents/Desktop");
        strcpy(p->programs,"/Windows/Start Menu/Programs");
        strcpy(p->settings,"/system/settings/desktop.cfg"); strcpy(p->temporary,"/system/settings/desktop.new");
    }
}
