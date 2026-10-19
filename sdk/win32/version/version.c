/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Bounded PE32 version resources. */
#include "../services/files.h"
extern uint8_t w32r_scalar_ansi(uint32_t);
extern uint32_t w32r_ansi_scalar(uint8_t);
static uint32_t align4(uint32_t n){return (n+3)&~3u;}
/* f5-20A: Unicode file names go through ACP 1252 to the ANSI CreateFileA
 * path of KERNEL32 (whose CreateFileW is a Windows 9x refusal); a name
 * outside the code page fails with ERROR_INVALID_NAME. */
static int open_name(const void *name,int wide,struct w32v_file *f)
{
    if(!wide)return w32v_open(name,0,f);
    char a[1040];uint32_t i=0;
    for(const WCHAR *w=name;w[i];i++){
        uint8_t c=w32r_scalar_ansi(w[i]);
        if(i+1>=sizeof(a)){w32r_last_error(206);return 0;}
        if(c=='?'&&w[i]!='?'){w32r_last_error(123);return 0;}
        a[i]=(char)c;
    }
    a[i]=0;return w32v_open(a,0,f);
}
struct resources {struct w32v_file *file;uint32_t pe,sections,count,rva,bytes,offset;};
static int rva_file(struct resources *r,uint32_t rva,uint32_t n,uint32_t *out)
{
    for(uint32_t i=0;i<r->count;i++){
        uint8_t section[40];if(!w32v_read(r->file,r->sections+i*40,section,40))return 0;
        uint32_t va=w32_u32(section+12),raw=w32_u32(section+16),off=w32_u32(section+20);
        if(rva>=va&&rva-va<=raw&&n<=raw-(rva-va)&&off<=r->file->size&&rva-va<=r->file->size-off&&n<=r->file->size-off-(rva-va)){
            *out=off+rva-va;return 1;}
    }
    return 0;
}
static int resource_read(struct resources *r,uint32_t offset,void *out,uint32_t n)
{return offset<=r->bytes&&n<=r->bytes-offset&&w32v_read(r->file,r->offset+offset,out,n);}
static int child(struct resources *r,uint32_t directory,uint32_t id,int any,int is_dir,uint32_t *out)
{
    uint8_t h[16];if(!resource_read(r,directory,h,16))return 0;uint32_t count=w32_u16(h+12)+(uint32_t)w32_u16(h+14);
    if(count>4096||directory>r->bytes-16||count>(r->bytes-directory-16)/8)return 0;
    uint32_t fallback=UINT32_MAX;
    for(uint32_t i=0;i<count;i++){
        uint8_t item[8];if(!resource_read(r,directory+16+i*8,item,8))return 0;
        uint32_t name=w32_u32(item),value=w32_u32(item+4);
        if((value>>31)!=(unsigned)is_dir)return 0;
        if(any&&fallback==UINT32_MAX)fallback=value&0x7fffffff;
        if(name==id){*out=value&0x7fffffff;return 1;}
    }
    if(any&&fallback!=UINT32_MAX){*out=fallback;return 1;}return 0;
}
static int locate(struct w32v_file *file,uint32_t *off,uint32_t *size)
{
    uint8_t dos[64],nt[248];struct resources r={.file=file};
    if(!w32v_read(file,0,dos,64)||dos[0]!='M'||dos[1]!='Z')return 0;
    r.pe=w32_u32(dos+60);if(!w32v_read(file,r.pe,nt,sizeof(nt))||w32_u32(nt)!=0x4550||w32_u16(nt+4)!=0x14c||w32_u16(nt+24)!=0x10b)return 0;
    r.count=w32_u16(nt+6);uint32_t optional=w32_u16(nt+20);
    if(!r.count||r.count>96||optional<224||r.pe>UINT32_MAX-24-optional||w32_u32(nt+116)<3)return 0;
    r.sections=r.pe+24+optional;r.rva=w32_u32(nt+136);r.bytes=w32_u32(nt+140);
    if(r.bytes<16||!rva_file(&r,r.rva,r.bytes,&r.offset))return 0;
    uint32_t type,name,leaf;uint8_t entry[16];
    if(!child(&r,0,16,0,1,&type)||!child(&r,type,1,1,1,&name)||!child(&r,name,1033,1,0,&leaf)||!resource_read(&r,leaf,entry,16))return 0;
    *size=w32_u32(entry+4);return *size>=6&&*size<=65535&&rva_file(&r,w32_u32(entry),*size,off);
}
/* ANSI conversions live in the caller's GetFileVersionInfo block, preserving
 * simultaneous query pointers. Track only buffers produced by this DLL;
 * arbitrary W queries need no tracking and never write past wLength. */
struct buffer {const void *address;uint32_t capacity;};
static struct buffer buffers[64];static uint32_t next_buffer;
static volatile uint32_t buffer_lock;
static void lock_buffers(void){while(__atomic_exchange_n(&buffer_lock,1,__ATOMIC_ACQUIRE)){} }
static void unlock_buffers(void){__atomic_store_n(&buffer_lock,0,__ATOMIC_RELEASE);}
static uint32_t version_size(const void *name,int wide,uint32_t *handle)
{
    if(handle)*handle=0;if(!name){w32r_last_error(87);return 0;}
    struct w32v_file f;if(!open_name(name,wide,&f))return 0;
    uint32_t off,n;int ok=locate(&f,&off,&n);w32v_close(&f);
    if(!ok){w32r_last_error(1813);return 0;}return align4(n)+n+8;
}
uint32_t W32API GetFileVersionInfoSizeA(const char *n,uint32_t *h){return version_size(n,0,h);}
uint32_t W32API GetFileVersionInfoSizeW(const WCHAR *n,uint32_t *h){return version_size(n,1,h);}
static int version_get(const void *name,int wide,uint32_t ignored,uint32_t cap,void *out)
{
    (void)ignored;if(!name||!out){w32r_last_error(87);return 0;}
    struct w32v_file f;if(!open_name(name,wide,&f))return 0;uint32_t off,n;int ok=locate(&f,&off,&n);
    if(!ok){w32v_close(&f);w32r_last_error(1813);return 0;}
    if(cap<n){w32v_close(&f);w32r_last_error(122);return 0;}
    ok=w32v_read(&f,off,out,n);w32v_close(&f);if(!ok){w32r_last_error(13);return 0;}
    lock_buffers();unsigned slot=64;for(unsigned i=0;i<64;i++)if(buffers[i].address==out){slot=i;break;}
    if(slot==64)slot=next_buffer++%64;buffers[slot]=(struct buffer){out,cap};unlock_buffers();return 1;
}
int W32API GetFileVersionInfoA(const char *n,uint32_t h,uint32_t c,void *o){return version_get(n,0,h,c,o);}
int W32API GetFileVersionInfoW(const WCHAR *n,uint32_t h,uint32_t c,void *o){return version_get(n,1,h,c,o);}
struct block {uint32_t start,end,value,length,children,type;};
static int block(const uint8_t *b,uint32_t start,uint32_t limit,struct block *out)
{
    if(start>limit||limit-start<6)return 0;uint32_t n=w32_u16(b+start),length=w32_u16(b+start+2),type=w32_u16(b+start+4);
    if(n<8||n>limit-start||type>1)return 0;uint32_t p=start+6,end=start+n;
    while(p+2<=end&&w32_u16(b+p))p+=2;if(p+2>end)return 0;p=align4(p+2);
    uint32_t bytes=type?length*2:length;if(p>end||bytes>end-p)return 0;
    *out=(struct block){start,end,p,length,align4(p+bytes),type};return 1;
}
static uint16_t lower(uint16_t c){return c>='A'&&c<='Z'?(uint16_t)(c+32):c;}
static int match(const uint8_t *b,uint32_t start,uint32_t end,const WCHAR *name,uint32_t len)
{
    if(start>end||len>(end-start)/2||len*2+2>end-start)return 0;
    for(uint32_t i=0;i<len;i++)if(lower(w32_u16(b+start+i*2))!=lower(name[i]))return 0;return !w32_u16(b+start+len*2);
}
static int query(const void *data,const WCHAR *path,void **out,uint32_t *length,int ansi)
{
    if(!data||!path||!out||!length){w32r_last_error(87);return 0;}*out=NULL;*length=0;
    const uint8_t *b=data;uint32_t n=w32_u16(b);struct block current;
    if(!block(b,0,n,&current))goto invalid;
    while(*path){if(*path=='\\'){path++;continue;}const WCHAR *p=path;while(*path&&*path!='\\')path++;uint32_t len=(uint32_t)(path-p),at=current.children;int found=0;
        while(at<current.end){struct block c;if(!block(b,at,current.end,&c))goto invalid;
            if(match(b,at+6,c.value,p,len)){current=c;found=1;break;}at=align4(c.end);}
        if(!found){w32r_last_error(1814);return 0;}}
    *length=current.length;*out=(void *)(b+current.value);
    if(ansi&&current.type){
        uint32_t cap=0;lock_buffers();for(unsigned i=0;i<64;i++)if(buffers[i].address==data){cap=buffers[i].capacity;break;}unlock_buffers();
        uint32_t scratch=align4(n)+current.value/2;
        if(scratch>cap||current.length>cap-scratch){w32r_last_error(122);*out=NULL;*length=0;return 0;}
        uint8_t *p=(uint8_t *)data+scratch;
        uint32_t written=0;
        for(uint32_t i=0;i<current.length;i++){
            uint32_t c=w32_u16(b+current.value+i*2);
            if(c>=0xd800&&c<=0xdbff&&i+1<current.length){uint32_t low=w32_u16(b+current.value+(i+1)*2);if(low>=0xdc00&&low<=0xdfff){c=0x10000+((c-0xd800)<<10)+low-0xdc00;i++;}}
            p[written++]=w32r_scalar_ansi(c);
        }
        *length=written;*out=p;
    }
    return 1;
invalid:w32r_last_error(13);return 0;
}
int W32API VerQueryValueW(const void *d,const WCHAR *p,void **o,uint32_t *n){return query(d,p,o,n,0);}
int W32API VerQueryValueA(const void *d,const char *p,void **o,uint32_t *n)
{
    if(!p){w32r_last_error(87);return 0;}WCHAR path[1024];uint32_t i=0;
    for(;p[i];i++){if(i>=1023){w32r_last_error(87);return 0;}path[i]=(uint8_t)p[i];}path[i]=0;return query(d,path,o,n,1);
}
static uint32_t find_file(uint32_t flags,const void *file,const void *windows,const void *app,void *current,uint32_t *cl,void *dest,uint32_t *dl,int wide)
{
    if(!file||!app||!cl||!dl||flags&~1u){w32r_last_error(87);return 4;}
    WCHAR directories[3][260],name[260],full[520];uint32_t lengths[3]={0,0,0},name_length=0;
    const char *fallback="C:\\WINDOWS";
    for(unsigned d=0;d<2;d++){
        const void *input=d?app:windows;uint32_t n=0;
        for(;;){uint32_t c=input?(wide?((const WCHAR *)input)[n]:w32r_ansi_scalar(((const uint8_t *)input)[n])):(uint8_t)fallback[n];
            if(!c)break;if(n>=251){w32r_last_error(206);return 4;}directories[d][n++]=(WCHAR)c;}
        while(n&&(directories[d][n-1]=='\\'||directories[d][n-1]=='/'))n--;
        directories[d][n]=0;lengths[d]=n;
    }
    for(;;){uint32_t c=wide?((const WCHAR *)file)[name_length]:w32r_ansi_scalar(((const uint8_t *)file)[name_length]);
        if(!c)break;if(name_length>=259||c=='\\'||c=='/'||c==':'){w32r_last_error(87);return 4;}name[name_length++]=(WCHAR)c;}
    name[name_length]=0;if(!name_length){w32r_last_error(87);return 4;}
    w32_copy(directories[2],directories[0],lengths[0]*2);const char *system="\\SYSTEM";
    for(unsigned i=0;i<7;i++)directories[2][lengths[0]+i]=(uint8_t)system[i];lengths[2]=lengths[0]+7;directories[2][lengths[2]]=0;
    int found=-1;const unsigned order[]={1,2,0};
    for(unsigned i=0;i<3;i++){
        unsigned d=order[i];uint32_t len=lengths[d];w32_copy(full,directories[d],len*2);full[len++]='\\';w32_copy(full+len,name,(name_length+1)*2);
        struct w32v_file f;if(open_name(full,1,&f)){w32v_close(&f);found=(int)d;break;}
    }
    unsigned destination=flags&1?2:1;uint32_t nc=found<0?0:lengths[found],nd=lengths[destination],result=found>=0&&(unsigned)found!=destination?1:0;
    if(!current||*cl<=nc)result|=4;
    else if(wide){if(nc)w32_copy(current,directories[found],nc*2);((WCHAR *)current)[nc]=0;}
    else{for(uint32_t i=0;i<nc;i++)((char *)current)[i]=(char)w32r_scalar_ansi(directories[found][i]);((char *)current)[nc]=0;}
    if(!dest||*dl<=nd)result|=4;
    else if(wide)w32_copy(dest,directories[destination],(nd+1)*2);
    else{for(uint32_t i=0;i<=nd;i++)((char *)dest)[i]=(char)w32r_scalar_ansi(directories[destination][i]);}
    *cl=nc+1;*dl=nd+1;return result;
}
uint32_t W32API VerFindFileA(uint32_t f,const char *n,const char *w,const char *a,char *c,uint32_t *cl,char *d,uint32_t *dl){return find_file(f,n,w,a,c,cl,d,dl,0);}
uint32_t W32API VerFindFileW(uint32_t f,const WCHAR *n,const WCHAR *w,const WCHAR *a,WCHAR *c,uint32_t *cl,WCHAR *d,uint32_t *dl){return find_file(f,n,w,a,c,cl,d,dl,1);}
