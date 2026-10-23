/* Installed-package gate before image allocation and again before binding.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "package.h"
#include "native.h"
#include <ciuki/kernel.h>
#include <ciuki/storage.h>
#include <ciuki/net.h>
#include <ciuki/mm.h>
struct package_reader { struct vfs_table table; char directory[160]; };
static int read_package(void *arg,const char *name,uint32_t offset,void *out,size_t bytes,size_t *got)
{
    struct package_reader *r=arg; char path[240]; size_t len=strlen(r->directory),n=strlen(name);
    if(!ndp_component(name) || len+n+2>sizeof(path)) return -1;
    memcpy(path,r->directory,len); path[len]='\\'; memcpy(path+len+1,name,n+1);
    int fd=vfs_open(&r->table,path,VFS_READ,VFS_DENY_WRITE,0); if(fd<0) return -1;
    uint64_t position=0; int e=vfs_seek(&r->table,fd,offset,VFS_SEEK_SET,&position);
    if(!e) e=vfs_read(&r->table,fd,out,bytes,got);
    if(vfs_close(&r->table,fd)) e=-1;
    return e;
}
static int match_device(const struct ndp_device *d,const struct pci_func *p)
{
    if(!p || p->class_code!=2 || p->subclass!=0 || d->bus!=p->bus || d->slot!=p->dev || d->function!=p->fn ||
       d->vendor!=p->vendor || d->device!=p->device || d->revision!=p->revision || d->sub_vendor!=p->sub_vendor || d->sub_device!=p->sub_device) return 0;
    /* An installed package never steals an active, stopped or quarantined
     * native adapter. The existing registry checks remain the final owner. */
    for(unsigned i=0;i<net_count();i++) {
        const struct netif *n=net_get(i);
        if(n && n->pci.bus==p->bus && n->pci.dev==p->dev && n->pci.fn==p->fn) return 0;
    }
    return 1;
}
int ndis_package_admit(const char *path,const uint8_t hash[32],const struct pci_func *p,const uint8_t qualified_seal[32])
{
    const char *prefix=NDP_ROOT "/";
    if(!path || !hash || strncmp(path,prefix,strlen(prefix))) return 0;
    const char *package=path+strlen(prefix),*slash=package;
    while(*slash && *slash!='/') slash++;
    if(!*slash) return 0;
    if(!slash || slash-package>=64 || !ndp_component(slash+1)) return 0;
    char name[64]; memcpy(name,package,(size_t)(slash-package)); name[slash-package]=0;
    if(!ndp_component(name)) return 0;
    struct storage *storage=storage_get();
    if(!storage || !storage->ready || storage->stopped) return 0;
    struct package_reader *r=kmalloc(sizeof(*r)); struct ndp_manifest *m=kmalloc(sizeof(*m));
    if(!r || !m) { if(r) kfree(r); if(m) kfree(m); return 0; }
    int ok=0; memset(r,0,sizeof(*r));
    if(vfs_table_init(&storage->vfs,&r->table,2)) goto done;
    const char *root="C:\\System\\drivers\\"; size_t len=strlen(root);
    memcpy(r->directory,root,len); memcpy(r->directory+len,name,strlen(name)+1);
    size_t got=0,at=0;
    while(at<sizeof(*m)) {
        if(read_package(r,"manifest.ndp",(uint32_t)at,(uint8_t *)m+at,sizeof(*m)-at,&got) || !got || got>sizeof(*m)-at) goto close;
        at+=got;
    }
    if(!ndp_manifest_valid(m) ||
       !m->enabled || strncmp(m->package,name,sizeof(m->package)) || strncmp(m->files[m->binary].name,slash+1,64) || memcmp(m->files[m->binary].hash,hash,32) ||
       (qualified_seal && memcmp(m->seal,qualified_seal,32))) goto close;
    uint8_t extra; if(read_package(r,"manifest.ndp",sizeof(*m),&extra,1,&got) || got) goto close;
    int present=p ? match_device(&m->device,p) && ndis_qualified_package(hash,p,m->seal) : 0;
    if(!p) for(unsigned i=0;i<pci_count();i++) {
        const struct pci_func *candidate=pci_get(i);
        if(match_device(&m->device,candidate) && ndis_qualified_package(hash,candidate,m->seal)) { present=1; break; }
    }
    if(present && ndp_verify(m,&m->device,read_package,r)) ok=1;
 close: vfs_table_destroy(&r->table);
 done: kfree(m); kfree(r); return ok;
}
