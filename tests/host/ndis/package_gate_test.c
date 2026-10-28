#include <ciuki/desktop.h>
#include <ciuki/kheap_limits.h>
/* Real package admission with synthetic VFS/PCI/qualification services.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/storage.h>
#include <ciuki/net.h>
#include <ciuki/sha256.h>
#include "src/kernel/ndis/package.h"
static int profile_enabled=1; static const char *profile_driver;
bool system_device_allowed(uint32_t id,const char *driver)
{ assert(id==0x10018); return profile_enabled && (!profile_driver || !driver || !strcmp(profile_driver,driver)); }
static struct storage storage;
static struct ndp_manifest manifest;
static struct pci_func pci={.bus=0,.dev=3,.fn=0,.vendor=0x1234,.device=0x5678,.sub_vendor=0x1111,.sub_device=0x2222,.revision=7,.class_code=2};
static struct netif native;
static uint8_t approved_hash[32],approved_seal[32];
static int qualified,native_owned,fail_read,extra_manifest,allocations,tables,opens,tamper_file,tamper_registry;
static int allocation_fail_after=-1;
static const unsigned char binary[]="synthetic native binary",inf[]="synthetic INF";
struct handle { int live,file; size_t at; };
static struct handle handles[8];
void *(kmalloc)(size_t n) { if (n > KHEAP_MAX_REQUEST) return 0; void *p=malloc(n); if(p) allocations++; return p; }
void kfree(void *p) { assert(p && allocations); allocations--; free(p); }
void *desktop_buffer_alloc(uint32_t bytes,enum mm_owner owner)
{ assert(bytes && owner==MM_IO_REQUEST); if (!allocation_fail_after) return 0; if (allocation_fail_after>0) allocation_fail_after--; void *p=calloc(1,bytes); if (p) allocations++; return p; }
void desktop_buffer_free(void *p,uint32_t bytes,enum mm_owner owner)
{ assert(bytes && owner==MM_IO_REQUEST); kfree(p); }
struct storage *storage_get(void) { return &storage; }
unsigned pci_count(void) { return 1; }
const struct pci_func *pci_get(unsigned i) { return i?NULL:&pci; }
unsigned net_count(void) { return native_owned?1:0; }
struct netif *net_get(unsigned i) { return i?NULL:&native; }
bool ndis_qualified_package(const uint8_t hash[32],const struct pci_func *p,const uint8_t seal[32])
{ return qualified && p==&pci && !memcmp(hash,approved_hash,32) && !memcmp(seal,approved_seal,32); }
int vfs_table_init(struct vfs *v,struct vfs_table *t,unsigned drive)
{ assert(v==&storage.vfs && drive==2); tables++; t->vfs=v; return 0; }
void vfs_table_destroy(struct vfs_table *t) { assert(t->vfs==&storage.vfs && tables); tables--; }
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{
    assert(t->vfs==&storage.vfs && flags==VFS_READ && share==VFS_DENY_WRITE && !attr);
    const char *prefix="C:\\System\\drivers\\synthetic-00-03-0\\";
    if(strncmp(path,prefix,strlen(prefix))) return -1;
    const char *name=path+strlen(prefix); int file=!strcmp(name,"manifest.ndp")?0:!strcmp(name,"mini.sys")?1:!strcmp(name,"registry.ndp")?2:!strcmp(name,"synthetic.inf")?3:-1;
    if(file<0) return -1;
    for(int i=0;i<8;i++) if(!handles[i].live) { handles[i]=(struct handle){1,file,0}; opens++; return i; }
    assert(0); return -1;
}
int vfs_close(struct vfs_table *t,int fd)
{ (void)t; assert(fd>=0 && fd<8 && handles[fd].live); handles[fd].live=0; opens--; return 0; }
int vfs_seek(struct vfs_table *t,int fd,int64_t offset,enum vfs_whence whence,uint64_t *position)
{ (void)t; assert(handles[fd].live && whence==VFS_SEEK_SET && offset>=0); handles[fd].at=(size_t)offset; *position=(uint64_t)offset; return 0; }
int vfs_read(struct vfs_table *t,int fd,void *out,size_t cap,size_t *got)
{
    (void)t; assert(handles[fd].live); if(fail_read) return -1;
    struct handle *h=&handles[fd]; const void *data; size_t n;
    switch(h->file) {
    case 0: data=&manifest; n=sizeof(manifest); if(extra_manifest && h->at==n) { *(unsigned char *)out=1; *got=1; return 0; } break;
    case 1: data=binary; n=sizeof(binary); break;
    case 2: data=manifest.parameters; n=manifest.parameter_count*sizeof(manifest.parameters[0]); break;
    default: data=inf; n=sizeof(inf); break;
    }
    *got=h->at>=n?0:n-h->at; if(*got>cap) *got=cap;
    if(*got) {
        memcpy(out,(const unsigned char *)data+h->at,*got);
        if((tamper_file && h->file==3) || (tamper_registry && h->file==2)) *(uint8_t *)out^=1;
    }
    h->at+=*got; return 0;
}
static const char *path="/System/drivers/synthetic-00-03-0/mini.sys";
static int admit(const struct pci_func *p,const uint8_t *seal)
{ int r=ndis_package_admit(path,approved_hash,p,seal); assert(!opens && !tables && !allocations); return r; }
int main(void)
{
    storage.ready=true; native.pci=pci;
    manifest.version=NDP_VERSION; manifest.size=sizeof(manifest); manifest.enabled=1; manifest.file_count=2; manifest.binary=0;
    manifest.device=(struct ndp_device){0,3,0,0x1234,0x5678,0x1111,0x2222,7};
    strcpy(manifest.package,"synthetic-00-03-0"); strcpy(manifest.inf,"synthetic.inf"); strcpy(manifest.service,"Synthetic");
    strcpy(manifest.files[0].name,"mini.sys"); manifest.files[0].bytes=sizeof(binary); sha256(binary,sizeof(binary),manifest.files[0].hash);
    strcpy(manifest.files[1].name,"synthetic.inf"); manifest.files[1].bytes=sizeof(inf); sha256(inf,sizeof(inf),manifest.files[1].hash);
    manifest.parameter_count=1; strcpy(manifest.parameters[0].key,"adapter/"); strcpy(manifest.parameters[0].name,"Value");
    manifest.parameters[0].type=NDP_STRING; manifest.parameters[0].bytes=4; memcpy(manifest.parameters[0].data,"one",4);
    ndp_seal(&manifest); memcpy(approved_hash,manifest.files[0].hash,32); memcpy(approved_seal,manifest.seal,32);
    assert(!admit(NULL,NULL)); qualified=1; assert(admit(NULL,NULL)); assert(admit(&pci,approved_seal));
    allocation_fail_after=0; assert(!admit(NULL,NULL));
    allocation_fail_after=1; assert(!admit(NULL,NULL));
    allocation_fail_after=-1; assert(admit(NULL,NULL));
    profile_enabled=0; assert(!admit(NULL,NULL) && !admit(&pci,approved_seal)); profile_enabled=1;
    profile_driver="Other"; assert(!admit(NULL,NULL) && !admit(&pci,approved_seal));
    profile_driver="Synthetic"; assert(admit(NULL,NULL) && admit(&pci,approved_seal)); profile_driver=NULL;
    puts("PASS G30B NDIS package gate: disabled and wrong-service profiles refused before loading/binding");
    native_owned=1; assert(!admit(NULL,NULL)); assert(!admit(&pci,approved_seal)); native_owned=0;
    fail_read=1; assert(!admit(NULL,NULL)); fail_read=0;
    extra_manifest=1; assert(!admit(NULL,NULL)); extra_manifest=0;
    tamper_file=1; assert(!admit(NULL,NULL)); tamper_file=0;
    tamper_registry=1; assert(!admit(NULL,NULL)); tamper_registry=0;
    pci.revision++; assert(!admit(NULL,NULL)); pci.revision--;
    pci.sub_vendor++; assert(!admit(&pci,approved_seal)); pci.sub_vendor--;
    pci.class_code=3; assert(!admit(NULL,NULL)); pci.class_code=2;
    manifest.enabled=0; ndp_seal(&manifest); assert(!admit(NULL,NULL)); manifest.enabled=1; ndp_seal(&manifest);
    manifest.parameters[0].data[0]='x'; ndp_seal(&manifest); assert(!admit(NULL,NULL)); manifest.parameters[0].data[0]='o'; ndp_seal(&manifest);
    manifest.files[1].hash[0]^=1; ndp_seal(&manifest); assert(!admit(NULL,NULL)); manifest.files[1].hash[0]^=1; ndp_seal(&manifest);
    uint8_t wrong[32]={0}; assert(!admit(&pci,wrong));
    const char *paths[]={"/System/drivers/../mini.sys","/System/drivers/synthetic-00-03-0/sub/mini.sys","/media/mini.sys","/System/drivers/synthetic-00-03-0/other.sys"};
    for(unsigned i=0;i<sizeof(paths)/sizeof(*paths);i++) assert(!ndis_package_admit(paths[i],approved_hash,NULL,NULL));
    for(unsigned i=0;i<100;i++) assert(admit(NULL,NULL));
    assert(!opens && !tables && !allocations);
    puts("PASS installed NDIS gate: qualified manifest required before load/bind, exact present device and sealed parameters, all hashes, native owner precedence, bounded cleanup"); return 0;
}
