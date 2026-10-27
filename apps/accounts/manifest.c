/* SPDX-License-Identifier: MIT -- shared destination manifest/tree validation */
#include "accounts.h"
#include "wire.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdio.h>

static int path_compare(const char *a, const char *b)
{
    while(*a && *b) { unsigned x=(unsigned char)*a++,y=(unsigned char)*b++;
        if(x>='A' && x<='Z') x+=32; if(y>='A' && y<='Z') y+=32; if(x!=y) return x<y?-1:1; }
    return (unsigned char)*a-(unsigned char)*b;
}
int acct_tree_digest(const void *v, size_t n, uint8_t digest[32])
{
    const uint8_t *p=v; size_t offset=0; const char *prior=NULL; unsigned nodes=0;
    if(!p || !digest) return -EINVAL;
    while(offset<n) {
        if(n-offset<4) return -EINVAL;
        unsigned len=a32(p+offset); offset+=4;
        if(!len || len>1024 || n-offset<(size_t)len+44 || !apadded(p+offset,len) || strlen((const char *)p+offset)+1!=len) return -EINVAL;
        const char *path=(const char *)p+offset;
        if(!nodes ? path[0]!=0 : (!path[0] || path[0]=='/' || strstr(path,"//") || path[strlen(path)-1]=='/' ||
            path_compare(prior,path)>=0)) return -EINVAL;
        const char *part=path;
        while(*part) {
            const char *end=strchr(part,'/'); size_t count=end?(size_t)(end-part):strlen(part);
            if((count==1 && part[0]=='.') || (count==2 && part[0]=='.' && part[1]=='.')) return -EINVAL;
            for(size_t i=0;i<count;++i) if((unsigned char)part[i]<32 || (unsigned char)part[i]>126 || part[i]=='\\' || part[i]==':') return -EINVAL;
            if(!end) break; part=end+1;
        }
        prior=path; offset+=len; unsigned kind=a32(p+offset); uint64_t size=a64(p+offset+4);
        if((kind!=1 && kind!=2) || (!nodes && kind!=1) || (kind==1 && (size || !azero(p+offset+12,32)))) return -EINVAL;
        offset+=44; ++nodes;
    }
    if(!nodes) return -EINVAL;
    sha256(v,n,digest); return 0;
}
int acct_manifest_validate(const struct acct_owner_request *q, const void *v, size_t n)
{
    const uint8_t *p=v;
    if(!q || !p || n!=64+256*q->count || n>ACCT_MANIFEST_MAX || memcmp(p,"CIUKISTG",8) ||
        a32(p+8)!=1 || a32(p+12)!=64 || a32(p+16)!=q->count || a32(p+20) || a32(p+36) ||
        memcmp(p+24,q->sid,12) || a64(p+40)!=q->volume || a64(p+48)!=q->transaction || a64(p+56)) return -EINVAL;
    for(unsigned i=0;i<q->count;++i) {
        const uint8_t *b=p+64+256*i; unsigned flags=a32(b+12),old=a32(b+4); char staging[64];
        snprintf(staging,64,"/System/Security/Staging/g%016llx/p%08x",(unsigned long long)q->transaction,q->bindings[i].uid);
        if(a32(b)!=q->bindings[i].uid || a32(b+8)!=q->bindings[i].action ||
            (flags!=1 && flags!=2 && flags!=6) || (!!old!=!!(flags&2)) ||
            !apadded(b+16,64) || strcmp((const char *)b+16,q->bindings[i].root) ||
            !apadded(b+80,64) || (old?!acct_profile_root_valid((const char *)b+80):!azero(b+80,64)) ||
            !apadded(b+144,64) || strcmp((const char *)b+144,staging) || azero(b+208,32) ||
            a64(b+240)!=q->bindings[i].key || a64(b+248)) return -EINVAL;
        if(old) {
            char prior[64]; snprintf(prior,64,"/Windows/Profiles/u%08x",old);
            if(strcmp(prior,(const char *)b+80) || (!!strcmp(prior,(const char *)b+16)!=!!(flags&4))) return -EINVAL;
        }
    }
    return 0;
}
