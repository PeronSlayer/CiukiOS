/* i386 endpoint of the H1 fixture-tree services. Pointers are copied into
 * bounded messages before crossing to the native w32srv/file adapter.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "h1_services.h"
#include <ciuki/w32_core.h>
#define LIMIT (1024u*1024u)
#define PTR(p) ((uint32_t)(uintptr_t)(p))
static int service_fd=-1;
static uint8_t packet[LIMIT];
static uint32_t length;
static int transfer(int write,void *data,uint32_t n){
    uint8_t *p=data;
    while(n){LONG r=host_syscall(write?4:3,(ULONG)service_fd,PTR(p),n,0,0,0);
        if(r==-4)continue;if(r<=0)return 0;p+=r;n-=(uint32_t)r;}
    return 1;
}
static int32_t rpc(uint32_t kind,const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used){
    uint32_t request[]={0x32494643u,kind,n,cap},reply[2];*used=0;
    if(n>LIMIT || cap>LIMIT || service_fd<0 || !transfer(1,request,sizeof(request)) ||
       !transfer(1,(void *)in,n) || !transfer(0,reply,sizeof(reply)) || reply[1]>cap ||
       !transfer(0,out,reply[1]))return -5;
    *used=reply[1];return (int32_t)reply[0];
}
static int append(uint32_t *field,uint32_t n,int input){
    uint32_t address=*field;if(!address){*field=0;return n==0;}
    if(n>LIMIT-length)return 0;
    *field=length;
    if(input)w32_copy(packet+length,(const void *)(uintptr_t)address,n);
    else w32_fill(packet+length,0,n);
    length+=n;return 1;
}
static int text(uint32_t *field){
    if(!*field)return 1;
    const char *p=(const void *)(uintptr_t)*field;uint32_t n=0;
    while(n<32768 && p[n])n++;
    return n<32768 && append(field,n+1,1);
}
static int32_t record(void *r,uint32_t size);
/* fx-hlrun: a ReadFile/WriteFile larger than one relay packet goes in
 * bounded chunks, as the kernel's W32_F_READ/W32_F_WRITE relay does
 * (src/kernel/proc/win32/files.c, CIUKI_IO_MAX chunks under one VFS
 * transaction): sequential chunks advance the file pointer, positional
 * ones (flags 0x10000) the offset; the call stops at the first error or
 * short transfer and reports the bytes moved. Half-Life's launcher reads a
 * 2.9 MB bitmap from its PAK in one ReadFile. */
#define CHUNK (512u*1024u)
static int32_t chunked(struct ciuki_w32_file_args *a){
    uint64_t at=((uint64_t)a->high<<32)|a->low;uint32_t total=0;int32_t e=0;
    a->error=0;
    do{
        struct ciuki_w32_file_args one=*a;uint32_t n=a->bytes-total;if(n>CHUNK)n=CHUNK;
        one.buffer=a->buffer+total;one.bytes=n;
        if(a->flags&0x10000u){one.low=(uint32_t)(at+total);one.high=(uint32_t)((at+total)>>32);}
        if((e=record(&one,sizeof(one))))break;
        total+=one.result;a->error=one.error;
        if(one.error || one.result<n)break;
    }while(total<a->bytes);
    a->result=total;return e;
}
static int32_t record(void *r,uint32_t size){
    uint32_t buffer=0,bytes=0,offset=0,used=0;
    struct ciuki_w32_header *header=r;
    if(size>128)return -22;
    if(header->operation==W32_FILE && size==sizeof(struct ciuki_w32_file_args)){
        struct ciuki_w32_file_args *a=r;
        if((a->action==W32_F_READ || a->action==W32_F_WRITE) && a->bytes>CHUNK && !a->path && !a->path2)
            return chunked(a);
    }
    w32_copy(packet,r,size);length=size;
    if(header->operation==W32_FILE){
        struct ciuki_w32_file_args *a=(void *)packet;
        if(!text(&a->path) || !text(&a->path2))return -22;
        buffer=a->buffer;bytes=a->bytes;
        if(buffer && bytes){if(!append(&a->buffer,bytes,a->action==W32_F_WRITE || a->action==W32_F_TIMES))return -22;offset=a->buffer;}
    }else if(header->operation==W32_PROCESS){
        struct ciuki_w32_spawn_args *a=(void *)packet;
        if(!text(&a->image) || !text(&a->command) || !text(&a->directory) ||
           (a->environment && !append(&a->environment,a->environment_bytes,1)))return -22;
        /* Startup data is not dereferenced by the native service. */
        a->startup=0;
    }else if(header->operation==W32_WAIT){
        struct ciuki_w32_wait_args *a=(void *)packet;
        if(a->count>64 || (a->count && !append(&a->handles,a->count*4,1)))return -22;
    }else if(header->operation==W32_OBJECT){
        struct ciuki_w32_object_args *a=(void *)packet;
        if(a->name){
            const uint16_t *w=(void *)(uintptr_t)a->name;char name[512];unsigned i=0;
            for(;i<sizeof(name)-1 && w[i];i++){if(w[i]>127)return -22;name[i]=(char)w[i];}
            if(i==sizeof(name)-1)return -22;name[i]=0;a->name=PTR(name);
            if(!text(&a->name))return -22;
        }
    }
    /* Preserve every caller pointer: replies contain offsets, never pointers. */
    uint8_t saved[128];w32_copy(saved,r,size);
    int32_t status=rpc(1,packet,length,packet,LIMIT,&used);
    if(status)return status;
    if(used!=length)return -5;
    w32_copy(r,packet,size);
    if(header->operation==W32_FILE){
        struct ciuki_w32_file_args *a=r,*old=(void *)saved;
        if(buffer && bytes && a->action!=W32_F_WRITE)w32_copy((void *)(uintptr_t)buffer,packet+offset,bytes);
        a->path=old->path;a->path2=old->path2;a->buffer=old->buffer;
    }else if(header->operation==W32_PROCESS){
        struct ciuki_w32_spawn_args *a=r,*old=(void *)saved;
        a->image=old->image;a->command=old->command;a->directory=old->directory;
        a->environment=old->environment;a->startup=old->startup;
    }else if(header->operation==W32_WAIT)((struct ciuki_w32_wait_args *)r)->handles=((struct ciuki_w32_wait_args *)(void *)saved)->handles;
    else if(header->operation==W32_OBJECT)((struct ciuki_w32_object_args *)r)->name=((struct ciuki_w32_object_args *)(void *)saved)->name;
    return 0;
}
uint32_t h1_installer_server(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used){
    int32_t e=rpc(2,in,n,out,cap,used);return e<0?1062u:(uint32_t)e;
}
static struct allocation {uint32_t at,bytes,protect;} regions[512];
/* setupapi's INF parser uses an unnamed, read-only section and keeps the
 * view after NtClose(section). Own the snapshot and the view separately. */
static struct {uint8_t *data;uint32_t bytes,handle;} sections[128];
static struct {void *data;uint32_t bytes;} views[128];
static uint32_t section_generation;
static int32_t readonly_section(struct ciuki_w32_section_args *a){
    a->error=0;a->result=0;
    if(a->kind!=W32_SEC_FILE || a->protect!=W32_PAGE_READONLY || a->name){a->error=120;return 0;}
    struct ciuki_w32_file_info info;
    struct ciuki_w32_file_args f={.header={1,sizeof(f),W32_FILE,0},.action=W32_F_STAT,.handle=a->file,.buffer=PTR(&info),.bytes=sizeof(info)};
    int32_t e=record(&f,sizeof(f));if(e)return e;if(f.error){a->error=f.error;return 0;}
    uint32_t n=a->bytes?a->bytes:info.size_low;
    if(info.size_high || !n || n>info.size_low || n>16u*1024u*1024u){a->error=87;return 0;}
    for(unsigned i=0;i<128;i++)if(!sections[i].data){
        uint8_t *data=host.services.alloc(&host,n);if(!data){a->error=8;return 0;}
        for(uint32_t at=0;at<n;){
            uint32_t count=n-at;if(count>65536)count=65536;
            f=(struct ciuki_w32_file_args){.header={1,sizeof(f),W32_FILE,0},.action=W32_F_READ,.handle=a->file,
                .buffer=PTR(data+at),.bytes=count,.low=at,.flags=0x10000};
            e=record(&f,sizeof(f));
            if(e || f.error || f.result!=count){host.services.free(&host,data,n);a->error=f.error?f.error:30;return e;}
            at+=count;
        }
        sections[i].data=data;sections[i].bytes=n;
        sections[i].handle=0x50000000u+(++section_generation)*4u;a->result=sections[i].handle;return 0;
    }
    a->error=8;return 0;
}
static int32_t readonly_view(struct ciuki_w32_view_args *a){
    a->error=0;a->result=0;
    if(a->action==W32_VIEW_MAP){
        for(unsigned i=0;i<128;i++)if(sections[i].data && sections[i].handle==a->section){
            uint32_t offset=a->offset_lo,n=a->bytes;
            if(a->offset_hi || (offset&65535u) || offset>=sections[i].bytes){a->error=87;return 0;}
            if(!n)n=sections[i].bytes-offset;
            if(n>sections[i].bytes-offset || a->access!=W32_FILE_MAP_READ){a->error=5;return 0;}
            for(unsigned v=0;v<128;v++)if(!views[v].data){
                void *p=host.services.map(&host,a->address,n,a->address!=0);if(!p){a->error=8;return 0;}
                w32_copy(p,sections[i].data+offset,n);
                if(!host.services.protect(&host,p,(n+4095u)&~4095u,1)){
                    host.services.unmap(&host,p,n);a->error=5;return 0;}
                views[v].data=p;views[v].bytes=n;a->result=PTR(p);return 0;
            }
            a->error=8;return 0;
        }
        a->error=6;return 0;
    }
    for(unsigned v=0;v<128;v++)if(views[v].data && PTR(views[v].data)==a->address){
        if(a->action==W32_VIEW_UNMAP){host.services.unmap(&host,views[v].data,views[v].bytes);views[v].data=0;}
        else if(a->action!=W32_VIEW_FLUSH){a->error=87;return 0;}
        a->result=1;return 0;
    }
    a->error=487;return 0;
}
static int memory(void *record,uint32_t op){
    struct ciuki_w32_vm *a=record;
    if(op==W32_VM_QUERY){
        struct ciuki_w32_query *q=record;
        for(unsigned i=0;i<512;i++)if(regions[i].at && q->address>=regions[i].at && q->address-regions[i].at<regions[i].bytes){
            q->info=(struct ciuki_w32_memory_info){regions[i].at,regions[i].at,regions[i].protect,regions[i].bytes,W32_MEM_COMMIT,regions[i].protect,0x20000};return 0;}
        q->info=(struct ciuki_w32_memory_info){.base=q->address&~4095u,.region_size=4096,.state=0x10000};return 0;
    }
    if(op==W32_VM_ALLOC){
        uint32_t n=(a->bytes+4095u)&~4095u;if(!n || n>0x10000000)return -12;
        for(unsigned i=0;i<512;i++)if(regions[i].at && a->address>=regions[i].at && a->address-regions[i].at<regions[i].bytes){
            if(n>regions[i].bytes-(a->address-regions[i].at))return -22;
            return 0; /* commit an already reserved anonymous view */
        }
        for(unsigned i=0;i<512;i++)if(!regions[i].at){
            void *p=host.services.map(&host,a->address,n,a->address!=0);if(!p)return -12;
            regions[i]=(struct allocation){PTR(p),n,a->protect};a->address=PTR(p);return 0;}
        return -12;
    }
    for(unsigned i=0;i<512;i++)if(regions[i].at && a->address>=regions[i].at && a->address-regions[i].at<regions[i].bytes){
        if(op==W32_VM_FREE){
            if(a->flags==W32_MEM_RELEASE){if(a->address!=regions[i].at || a->bytes)return -22;
                host.services.unmap(&host,(void *)(uintptr_t)regions[i].at,regions[i].bytes);regions[i].at=0;}
            else if(a->flags!=0x4000)return -22;
            return 0;
        }
        if(a->bytes>regions[i].bytes-(a->address-regions[i].at))return -22;
        uint32_t prot=a->protect==1?0:a->protect==2?1:a->protect==4?3:a->protect==0x10?4:a->protect==0x20?5:7;
        if(!host.services.protect(&host,(void *)(uintptr_t)a->address,a->bytes,prot))return -22;
        a->old_protect=regions[i].protect;regions[i].protect=a->protect;return 0;
    }
    /* fx-hlrun: VirtualProtect of mapped image pages (the kernel changes any
     * committed extent, src/kernel/proc/win32/memory.c): the Quake-family
     * software renderer makes its own code writable (Sys_MakeCodeWriteable).
     * Image protections are not tracked here; the old value reported is the
     * image code default PAGE_EXECUTE_READ. An unmapped range still fails. */
    if(op==W32_VM_PROTECT && a->bytes){
        uint32_t base=a->address&~4095u,end=(a->address+a->bytes+4095u)&~4095u;
        uint32_t prot=a->protect==1?0:a->protect==2?1:a->protect==4?3:a->protect==0x10?4:a->protect==0x20?5:7;
        if(end<=base || !host.services.protect(&host,(void *)(uintptr_t)base,end-base,prot))return -22;
        a->old_protect=0x20;a->address=base;a->bytes=end-base;return 0;
    }
    return -22;
}
int h1_installer_kernel(void *r,uint32_t size,int32_t *status){
    struct ciuki_w32_header *h=r;
    if(size<sizeof(*h) || h->size!=size || h->version!=1 || h->reserved){*status=-22;return 1;}
    uint32_t expected=0;
    switch(h->operation){
    case W32_FILE:expected=sizeof(struct ciuki_w32_file_args);break;
    case W32_PROCESS:expected=sizeof(struct ciuki_w32_spawn_args);break;
    case W32_OBJECT:expected=sizeof(struct ciuki_w32_object_args);break;
    case W32_WAIT:expected=sizeof(struct ciuki_w32_wait_args);break;
    case W32_SYSTEM:expected=sizeof(struct ciuki_w32_system_args);break;
    case W32_SECTION:expected=sizeof(struct ciuki_w32_section_args);break;
    case W32_VIEW:expected=sizeof(struct ciuki_w32_view_args);break;
    case W32_VM_QUERY:expected=sizeof(struct ciuki_w32_query);break;
    case W32_VM_ALLOC:case W32_VM_FREE:case W32_VM_PROTECT:expected=sizeof(struct ciuki_w32_vm);break;
    case W32_COUNTER:expected=sizeof(struct ciuki_w32_counter);break;
    default:break;
    }
    if(expected && size!=expected){*status=-22;return 1;}
    switch(h->operation){
    case W32_OBJECT:{
        struct ciuki_w32_object_args *a=r;
        for(unsigned i=0;i<128;i++)if(sections[i].data && sections[i].handle==a->handle){
            a->error=0;
            if(a->action==W32_OBJ_CLOSE){host.services.free(&host,sections[i].data,sections[i].bytes);sections[i].data=0;a->result=1;}
            else if(a->action==W32_OBJ_QUERY){a->type=W32_OBJ_SECTION;a->result=0;}
            else a->error=120;
            *status=0;return 1;
        }
        *status=record(r,size);return 1;}
    case W32_SECTION:*status=readonly_section(r);return 1;
    case W32_VIEW:*status=readonly_view(r);return 1;
    case W32_FILE:case W32_PROCESS:case W32_WAIT:case W32_SYSTEM:
        *status=record(r,size);return 1;
    case W32_VM_ALLOC:case W32_VM_FREE:case W32_VM_PROTECT:case W32_VM_QUERY:
        *status=memory(r,h->operation);return 1;
    case W32_COUNTER:{
        struct ciuki_w32_counter *a=r;struct {uint32_t sec,nsec;} t={0,0};
        LONG e=host_syscall(265,1,PTR(&t),0,0,0,0);
        a->frequency_lo=1000;a->frequency_hi=0;
        uint64_t value=(uint64_t)t.sec*1000+t.nsec/1000000;
        a->value_lo=(uint32_t)value;a->value_hi=(uint32_t)(value>>32);*status=e;return 1;}
    default:return 0;
    }
}
static int (*original_open)(void *,const char *,struct w32_file *);
static void (*original_close)(void *,struct w32_file *);
static struct {void *data;uint32_t size;} snapshots[128];
static const struct w32_builtin *providers;
static uint32_t provider_count;
void h1_installer_providers(const struct w32_builtin *table,uint32_t count){providers=table;provider_count=count;}
static int normalized_provider(const char *name,char out[1024]){
    /* Only fully qualified host paths from the trusted builtin table. Do
     * not turn a guest drive-less name into a host working-directory path. */
    if(name[0]!='/')return 0;
    unsigned n=0;out[n++]='/';
    for(unsigned i=1;name[i];){
        while(name[i]=='/' || name[i]=='\\')i++;
        unsigned start=i;while(name[i] && name[i]!='/' && name[i]!='\\')i++;
        unsigned len=i-start;if(!len)break;
        if(len==1 && name[start]=='.')continue;
        if(len==2 && name[start]=='.' && name[start+1]=='.')return 0;
        if(n+len+1>=1024)return 0;
        if(n>1)out[n++]='/';
        for(unsigned j=0;j<len;j++)out[n++]=name[start+j];
    }
    out[n]=0;return 1;
}
static int same_provider(const char *a,const char *b){
    unsigned i=0;while(a[i] && a[i]==b[i])i++;return a[i]==b[i];
}
static int fixture_open(void *o,const char *name,struct w32_file *file){
    char candidate[1024],allowed[1024];
    if(normalized_provider(name,candidate))for(unsigned i=0;i<provider_count;i++){
        if(normalized_provider(providers[i].path,allowed) && same_provider(candidate,allowed))
            return original_open(o,allowed,file);
    }
    struct ciuki_w32_file_args a={.header={1,sizeof(a),W32_FILE,0},.action=W32_F_OPEN,.path=PTR(name),.access=0x80000000u,.share=7,.disposition=3};
    if(record(&a,sizeof(a)) || a.error)return 0;uint32_t handle=a.result;
    struct ciuki_w32_file_info info;
    a=(struct ciuki_w32_file_args){.header={1,sizeof(a),W32_FILE,0},.action=W32_F_STAT,.handle=handle,.buffer=PTR(&info),.bytes=sizeof(info)};
    int ok=!record(&a,sizeof(a)) && !a.error && !info.size_high && info.size_low && info.size_low<=256u*1024*1024;
    void *data=ok?host.services.alloc(&host,info.size_low):0;
    if(!data)ok=0;
    for(uint32_t at=0;ok && at<info.size_low;){
        uint32_t n=info.size_low-at;if(n>65536)n=65536;
        a=(struct ciuki_w32_file_args){.header={1,sizeof(a),W32_FILE,0},.action=W32_F_READ,.handle=handle,.buffer=PTR((uint8_t *)data+at),.bytes=n};
        ok=!record(&a,sizeof(a)) && !a.error && a.result==n;at+=n;
    }
    struct ciuki_w32_object_args close={.header={1,sizeof(close),W32_OBJECT,0},.action=W32_OBJ_CLOSE,.handle=handle};
    if(record(&close,sizeof(close)) || close.error)ok=0;
    if(ok)for(unsigned i=0;i<128;i++)if(!snapshots[i].data){
        snapshots[i].data=data;snapshots[i].size=info.size_low;
        *file=(struct w32_file){data,info.size_low,data};return 1;}
    if(data)host.services.free(&host,data,info.size_low);return 0;
}
static void fixture_close(void *o,struct w32_file *file){
    for(unsigned i=0;i<128;i++)if(snapshots[i].data && snapshots[i].data==file->cookie){
        host.services.free(&host,snapshots[i].data,snapshots[i].size);snapshots[i].data=0;return;}
    original_close(o,file);
}
static WCHAR environment[256],cwd[778];
void h1_installer_parameters(const char *command){
    static WCHAR line[4096];unsigned n=0;while(command[n] && n<4095){line[n]=(uint8_t)command[n];n++;}line[n]=0;
    RtlInitUnicodeString(&host.parameters.CommandLine,line);
}
int h1_installer_init(const char *fd){
    if(!fd || !*fd)return 0;service_fd=0;
    for(;*fd;fd++){if(*fd<'0' || *fd>'9' || service_fd>6553)return 0;service_fd=service_fd*10+*fd-'0';}
    char directory[778];uint32_t used=0;
    if(rpc(3,0,0,directory,sizeof(directory),&used) || !used || used>sizeof(directory) || directory[used-1])return 0;
    for(unsigned i=0;i<used;i++)cwd[i]=(uint8_t)directory[i];
    RtlInitUnicodeString(&host.parameters.CurrentDirectory.DosPath,cwd);
    const char env[]="TEMP=C:\\WINDOWS\\TEMP\0TMP=C:\\WINDOWS\\TEMP\0WINDIR=C:\\WINDOWS\0PATH=C:\\WINDOWS;C:\\WINDOWS\\SYSTEM\0\0";
    for(unsigned i=0;i<sizeof(env);i++)environment[i]=(uint8_t)env[i];host.parameters.Environment=environment;
    original_open=host.services.open;original_close=host.services.close;
    host.services.open=fixture_open;host.services.close=fixture_close;return 1;
}
