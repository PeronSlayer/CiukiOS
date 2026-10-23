/* SPDX-License-Identifier: GPL-2.0-only */
#include "package.h"
#include "../include/ciuki/sha256.h"
#if __STDC_HOSTED__ || defined(__CIUKIOS__)
#include <string.h>
#else
#include <ciuki/kernel.h>
#endif
int ndp_component(const char *p)
{
    size_t n=0;
    if (!p || !*p || *p=='.') return 0;
    for (; p[n]; n++) {
        unsigned char c=(unsigned char)p[n];
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')) return 0;
        if (n>=63) return 0;
    }
    return p[n-1]!='.';
}
int ndp_relative(const char *p)
{
    char component[64]; size_t n=0;
    if (!p || !*p) return 0;
    for (;;) {
        if (*p=='/' || *p=='\\' || !*p) {
            component[n]=0;
            if (!ndp_component(component)) return 0;
            n=0; if (!*p) return 1;
        } else { if(n>=63) return 0; component[n++]=*p; }
        p++;
    }
}
static int terminated(const char *s,size_t n) { for(size_t i=0;i<n;i++) if(!s[i]) return 1; return 0; }
void ndp_seal(struct ndp_manifest *m) { sha256(m,offsetof(struct ndp_manifest,seal),m->seal); }
int ndp_manifest_valid(const struct ndp_manifest *m)
{
    uint8_t hash[32];
    if (!m || m->version!=NDP_VERSION || m->size!=sizeof(*m) || m->enabled>1 ||
        !m->file_count || m->file_count>NDP_FILES || m->parameter_count>NDP_PARAMS || m->binary>=m->file_count) return 0;
#define CHECK(f) if(!terminated(m->f,sizeof(m->f))) return 0
    CHECK(package); CHECK(inf); CHECK(models); CHECK(install); CHECK(services); CHECK(service);
    CHECK(hardware_id); CHECK(name); CHECK(provider); CHECK(driver_version); CHECK(reason);
#undef CHECK
    if(!ndp_component(m->package) || !ndp_component(m->service) || !ndp_component(m->inf) ||
       m->device.bus>255 || m->device.slot>31 || m->device.function>7 || m->device.vendor>65535 ||
       m->device.device>65535 || m->device.sub_vendor>65535 || m->device.sub_device>65535 || m->device.revision>255) return 0;
    for(unsigned i=0;i<m->file_count;i++) {
        if(!terminated(m->files[i].name,64) || !ndp_component(m->files[i].name) ||
           !m->files[i].bytes || m->files[i].bytes>4u*1024*1024) return 0;
        for(unsigned j=0;j<i;j++) {
            const char *a=m->files[i].name,*b=m->files[j].name;
            unsigned k=0;
            while(a[k] && b[k] && (a[k]|32)==(b[k]|32)) k++;
            if(!a[k] && !b[k]) return 0;
        }
    }
    for(unsigned i=0;i<m->parameter_count;i++) {
        const struct ndp_parameter *p=&m->parameters[i];
        if(!terminated(p->key,sizeof(p->key)) || !terminated(p->name,sizeof(p->name)) ||
           !p->bytes || p->bytes>NDP_DATA ||
           (p->type!=NDP_STRING && p->type!=NDP_BINARY && p->type!=NDP_DWORD) ||
           (p->type==NDP_DWORD && p->bytes!=4) ||
           (p->type==NDP_STRING && p->data[p->bytes-1])) return 0;
    }
    sha256(m,offsetof(struct ndp_manifest,seal),hash);
    return !memcmp(hash,m->seal,32);
}
int ndp_verify(const struct ndp_manifest *m,const struct ndp_device *d,ndp_reader read,void *ctx)
{
    if(!d || !read || !ndp_manifest_valid(m) || memcmp(d,&m->device,sizeof(*d))) return 0;
    for(unsigned i=0;i<m->file_count;i++) {
        struct sha256_ctx hash; uint8_t block[512],digest[32]; uint32_t at=0;
        sha256_init(&hash);
        while(at<m->files[i].bytes) {
            size_t got=0,want=m->files[i].bytes-at;
            if(want>sizeof(block)) want=sizeof(block);
            if(read(ctx,m->files[i].name,at,block,want,&got) || !got || got>want) return 0;
            sha256_update(&hash,block,got); at+=(uint32_t)got;
        }
        size_t got=0;
        if(read(ctx,m->files[i].name,at,block,1,&got) || got) return 0;
        sha256_final(&hash,digest);
        if(memcmp(digest,m->files[i].hash,32)) return 0;
    }
    /* The driver's private registry key is committed with its manifest. */
    size_t registry_bytes=m->parameter_count*sizeof(m->parameters[0]),at=0;
    uint8_t block[512];
    while(at<registry_bytes) {
        size_t got=0,want=registry_bytes-at;
        if(want>sizeof(block)) want=sizeof(block);
        if(read(ctx,"registry.ndp",(uint32_t)at,block,want,&got) || !got || got>want ||
           memcmp(block,(const uint8_t *)m->parameters+at,got)) return 0;
        at+=got;
    }
    size_t got=0;
    if(read(ctx,"registry.ndp",(uint32_t)at,block,1,&got) || got) return 0;
    return 1;
}
