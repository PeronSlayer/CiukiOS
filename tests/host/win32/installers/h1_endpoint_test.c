/* Native unit test of the actual i386 endpoint's copied records and INF
 * mapping lifetime. No PE execution or replacement installer interpreter.
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "h1_services.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <ciuki/w32_core.h>
#pragma clang diagnostic pop
struct host_state host;
#define PTR(p) ((uint32_t)(uintptr_t)(p))
#define HEADER(op,a) (struct ciuki_w32_header){1,sizeof(a),op,0}
LONG host_syscall(ULONG n,ULONG a,ULONG b,ULONG c,ULONG d,ULONG e,ULONG f){
    (void)d;(void)e;(void)f;
    ssize_t r=n==3?read((int)a,(void *)(uintptr_t)b,c):n==4?write((int)a,(void *)(uintptr_t)b,c):-1;
    return r<0?-errno:(LONG)r;
}
void W32API RtlInitUnicodeString(UNICODE_STRING *s,const WCHAR *p){
    unsigned n=0;while(p[n])n++;*s=(UNICODE_STRING){n*2,n*2+2,(WCHAR *)p};
}
static void *map(void *o,uint32_t at,uint32_t n,int exact){
    (void)o;int flags=MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT;
    if(exact)flags|=MAP_FIXED_NOREPLACE;
    void *p=mmap((void *)(uintptr_t)at,n,PROT_READ|PROT_WRITE,flags,-1,0);
    if(p==(void *)-1)return 0;assert((uintptr_t)p<=UINT32_MAX-n);host.mappings++;return p;
}
static void unmap(void *o,void *p,uint32_t n){(void)o;assert(!munmap(p,n));host.mappings--;}
static void *alloc(void *o,uint32_t n){return map(o,0,n,0);}
static int protect(void *o,void *p,uint32_t n,uint32_t mode){(void)o;return !mprotect(p,n,(int)mode);}
static unsigned provider_opens,provider_closes;
static uint8_t provider[]="trusted builtin";
static int open_unused(void *o,const char *n,struct w32_file *f){
    (void)o;assert(!strcmp(n,"/trusted/provider.dll"));provider_opens++;
    *f=(struct w32_file){provider,sizeof(provider),provider};return 1;
}
static void close_unused(void *o,struct w32_file *f){(void)o;assert(f->cookie==provider);provider_closes++;}
static void call(void *p,uint32_t n){int32_t status=-999;assert(h1_installer_kernel(p,n,&status) && !status);}
static void close_handle(uint32_t handle){
    struct ciuki_w32_object_args a={.action=W32_OBJ_CLOSE,.handle=handle};a.header=HEADER(W32_OBJECT,a);
    call(&a,sizeof(a));assert(!a.error && a.result);
}
static void *exercise(void *fd){
    host.services.alloc=alloc;host.services.free=unmap;host.services.map=map;host.services.unmap=unmap;
    host.services.protect=protect;host.services.open=open_unused;host.services.close=close_unused;
    assert(h1_installer_init(fd));
    const struct w32_builtin allowed[]={ {.path="/trusted/provider.dll"} };
    h1_installer_providers(allowed,1);
    struct w32_file snapshot;
    const char *refused[]={"/etc/passwd","../../../../etc/passwd","/trusted/other.dll","provider.dll"};
    for(unsigned i=0;i<sizeof(refused)/sizeof(refused[0]);i++)assert(!host.services.open(&host,refused[i],&snapshot));
    assert(!provider_opens);
    assert(host.services.open(&host,"/trusted/./provider.dll",&snapshot));
    host.services.close(&host,&snapshot);assert(provider_opens==1 && provider_closes==1);
    /* The VirtualAlloc/Free/Protect/Query path needed by reused cabinet.dll. */
    struct ciuki_w32_vm a={.bytes=8192,.flags=W32_MEM_RESERVE|W32_MEM_COMMIT,.protect=W32_PAGE_READWRITE};
    a.header=HEADER(W32_VM_ALLOC,a);call(&a,sizeof(a));assert(a.address);
    uint32_t base=a.address;memset((void *)(uintptr_t)base,0x5a,8192);
    struct ciuki_w32_query q={.address=base+4096};q.header=HEADER(W32_VM_QUERY,q);call(&q,sizeof(q));
    assert(q.info.base==base && q.info.region_size==8192 && q.info.protect==W32_PAGE_READWRITE);
    a.address=base;a.bytes=8192;a.protect=W32_PAGE_READONLY;a.header=HEADER(W32_VM_PROTECT,a);
    call(&a,sizeof(a));assert(a.old_protect==W32_PAGE_READWRITE);
    assert(*(const uint8_t *)(uintptr_t)base==0x5a);
    a.protect=W32_PAGE_READWRITE;call(&a,sizeof(a));assert(a.old_protect==W32_PAGE_READONLY);
    a.header=HEADER(W32_VM_FREE,a);a.flags=W32_MEM_RELEASE;a.bytes=0;call(&a,sizeof(a));
    call(&q,sizeof(q));assert(q.info.state==W32_MEM_FREE);
    /* setupapi closes the section before reading/parsing the mapped INF. */
    char path[]="C:\\WINDOWS\\TEMP\\endpoint.inf";
    struct ciuki_w32_file_args f={.action=W32_F_OPEN,.path=PTR(path),.access=0x80000000u,.share=7,.disposition=3};
    f.header=HEADER(W32_FILE,f);call(&f,sizeof(f));assert(!f.error && f.path==PTR(path));
    uint32_t file=f.result;
    struct ciuki_w32_section_args s={.kind=W32_SEC_FILE,.file=file,.protect=W32_PAGE_READONLY};
    s.header=HEADER(W32_SECTION,s);call(&s,sizeof(s));assert(!s.error && s.result);
    uint32_t section=s.result;
    struct ciuki_w32_view_args v={.action=W32_VIEW_MAP,.section=section,.access=W32_FILE_MAP_READ};
    v.header=HEADER(W32_VIEW,v);call(&v,sizeof(v));assert(!v.error && v.result);
    uint32_t view=v.result;close_handle(section);close_handle(file);
    const char expected[]="[Version]\r\nSignature=\"$CHICAGO$\"\r\n";
    assert(!memcmp((const void *)(uintptr_t)view,expected,sizeof(expected)-1));
    v.action=W32_VIEW_UNMAP;v.address=view;call(&v,sizeof(v));assert(!v.error && v.result);
    call(&v,sizeof(v));assert(v.error==487);
    assert(!host.mappings);
    puts("PASS native H1 VM records and INF read-only view after section/file close");return 0;
}
int main(int argc,char **argv){
    assert(argc==5);
    /* Frozen records contain 32-bit addresses; keep stack and allocations in
     * that range without running i386 code or bypassing its preflight. */
    size_t bytes=2*1024*1024;void *stack=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    assert(stack!=(void *)-1 && (uintptr_t)stack<=UINT32_MAX-bytes);
    pthread_attr_t attr;pthread_t thread;assert(!pthread_attr_init(&attr));assert(!pthread_attr_setstack(&attr,stack,bytes));
    assert(!pthread_create(&thread,&attr,exercise,argv[4]));assert(!pthread_join(thread,0));
    assert(!pthread_attr_destroy(&attr));assert(!munmap(stack,bytes));return 0;
}
