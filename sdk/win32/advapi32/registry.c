/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include "internal.h"
extern uint32_t w32r_ansi_scalar(uint8_t);
extern uint8_t w32r_scalar_ansi(uint32_t);
static volatile uint32_t busy;
static uint8_t request[W32R_MAX],response[W32R_MAX];
static struct w32r_reply *reply;
static uint8_t *tail;
static LONG end(uint32_t e){__atomic_store_n(&busy,0,__ATOMIC_RELEASE);return (LONG)e;}
static int string_type(uint32_t t){return t==1||t==2||t==7;}
static uint32_t call(unsigned op,uint32_t key,const void *path,const void *name,int wide,uint32_t index,uint32_t type,uint32_t access,uint32_t flags,const void *data,uint32_t size)
{
    while(__atomic_exchange_n(&busy,1,__ATOMIC_ACQUIRE))w32r_pause();
    char p[W32R_PATH],n[W32R_NAME];
    if(!w32r_to_utf8(p,sizeof(p),path,wide)||!w32r_to_utf8(n,sizeof(n),name,wide)||size>W32R_DATA||(!data&&size))return 87;
    struct w32r_request a={W32R_VERSION,op,key,index,type,access,flags,w32_strlen(p)+1,w32_strlen(n)+1,size};
    uint8_t *body=request+sizeof(a);w32_copy(body,p,a.path_bytes);w32_copy(body+a.path_bytes,n,a.name_bytes);body+=a.path_bytes+a.name_bytes;
    if(op==W32R_SET&&!wide&&string_type(type)){
        if(size>W32R_DATA/2)return 87;a.data_bytes=size*2;
        for(uint32_t i=0;i<size;i++)w32_put16(body+i*2,(uint16_t)w32r_ansi_scalar(((const uint8_t *)data)[i]));
    }else if(size)w32_copy(body,data,size);
    w32_copy(request,&a,sizeof(a));uint32_t bytes=0;
    uint32_t e=w32r_exchange(request,sizeof(a)+a.path_bytes+a.name_bytes+a.data_bytes,response,sizeof(response),&bytes);
    if(e)return e;
    if(bytes<sizeof(*reply))return 13;
    reply=(struct w32r_reply *)response;tail=response+sizeof(*reply);
    uint32_t names=reply->name_bytes?reply->name_bytes+1:op==W32R_ENUM_VALUE&&!reply->error?1:0;
    if(reply->version!=W32R_VERSION||reply->name_bytes>=W32R_NAME||reply->data_bytes>W32R_DATA||
       bytes!=sizeof(*reply)+(uint64_t)names+reply->data_bytes||(names&&(tail[names-1]||w32_strlen((const char *)tail)!=names-1)))return 13;
    return reply->error;
}
/* Windows 9x ignores samDesired: generic and MAXIMUM_ALLOWED rights map to
 * KEY_* rights and the KEY_WOW64_* view bits are dropped (f5-20A). */
uint32_t w32r_registry_access(uint32_t a)
{
    static const uint32_t map[5][2]={{0x02000000u,0xf003fu},{0x80000000u,0x20019u},{0x40000000u,0x20006u},
                                     {0x20000000u,0x20019u},{0x10000000u,0xf003fu}};
    for(unsigned i=0;i<5;i++)if(a&map[i][0])a=(a&~map[i][0])|map[i][1];
    return a&~0x300u;
}
static LONG open_key(uint32_t key,const void *path,uint32_t options,uint32_t access,uint32_t *out,int create,int wide,uint32_t *disp)
{
    access=w32r_registry_access(access);
    if(!out||options)return 87;*out=0;
    uint32_t e=call(create?W32R_CREATE:W32R_OPEN,key,path,NULL,wide,0,0,access,options,NULL,0);
    if(!e){*out=reply->key;if(disp)*disp=reply->disposition;}return end(e);
}
LONG W32API RegOpenKeyExA(uint32_t k,const char *p,uint32_t o,uint32_t a,uint32_t *h){return open_key(k,p,o,a,h,0,0,NULL);}
LONG W32API RegOpenKeyExW(uint32_t k,const WCHAR *p,uint32_t o,uint32_t a,uint32_t *h){return open_key(k,p,o,a,h,0,1,NULL);}
LONG W32API RegOpenKeyA(uint32_t k,const char *p,uint32_t *h){return open_key(k,p,0,0xf003f,h,0,0,NULL);}
LONG W32API RegOpenKeyW(uint32_t k,const WCHAR *p,uint32_t *h){return open_key(k,p,0,0xf003f,h,0,1,NULL);}
LONG W32API RegCreateKeyExA(uint32_t k,const char *p,uint32_t reserved,char *cls,uint32_t options,uint32_t access,void *security,uint32_t *h,uint32_t *disp)
{(void)cls;(void)security;if(reserved)return 87;return open_key(k,p,options,access,h,1,0,disp);}
LONG W32API RegCreateKeyExW(uint32_t k,const WCHAR *p,uint32_t reserved,WCHAR *cls,uint32_t options,uint32_t access,void *security,uint32_t *h,uint32_t *disp)
{(void)cls;(void)security;if(reserved)return 87;return open_key(k,p,options,access,h,1,1,disp);}
LONG W32API RegCreateKeyA(uint32_t k,const char *p,uint32_t *h){return open_key(k,p,0,0xf003f,h,1,0,NULL);}
LONG W32API RegCreateKeyW(uint32_t k,const WCHAR *p,uint32_t *h){return open_key(k,p,0,0xf003f,h,1,1,NULL);}
void w32_registry_forget(uint32_t);
/* Keys from NtOpenKey/NtCreateKey may be closed here; ntdll forgets them. */
LONG W32API RegCloseKey(uint32_t key){LONG e=end(call(W32R_CLOSE,key,NULL,NULL,0,0,0,0,0,NULL,0));if(!e)w32_registry_forget(key);return e;}
LONG W32API RegFlushKey(uint32_t key){return end(call(W32R_FLUSH,key,NULL,NULL,0,0,0,0,0,NULL,0));}
static LONG set(uint32_t key,const void *name,uint32_t reserved,uint32_t type,const void *data,uint32_t n,int wide)
{
    if(reserved||(!data&&n))return 87;
    uint32_t e=call(W32R_SET,key,NULL,name,wide,0,type,0,0,data,n);return end(e);
}
LONG W32API RegSetValueExA(uint32_t k,const char *n,uint32_t r,uint32_t t,const uint8_t *d,uint32_t b){return set(k,n,r,t,d,b,0);}
LONG W32API RegSetValueExW(uint32_t k,const WCHAR *n,uint32_t r,uint32_t t,const uint8_t *d,uint32_t b){return set(k,n,r,t,d,b,1);}
static uint32_t query_data(void *out,uint32_t *size,uint32_t *type,const uint8_t *data,int wide)
{
    if(type)*type=reply->type;uint32_t n=reply->data_bytes;
    int convert=!wide&&string_type(reply->type);if(convert)n=(n+1)/2;
    if(out&&(!size||*size<n)){if(size)*size=n;return size?234:87;}
    if(size)*size=n;
    if(out){if(convert){for(uint32_t i=0;i<n;i++){uint32_t c=data[i*2];if(i*2+1<reply->data_bytes)c|=(uint32_t)data[i*2+1]<<8;((uint8_t *)out)[i]=w32r_scalar_ansi(c);}}
        else if(n)w32_copy(out,data,n);}return 0;
}
static LONG query(uint32_t key,const void *name,void *reserved,uint32_t *type,void *data,uint32_t *size,int wide)
{
    if(reserved||(!size&&data))return 87;
    uint32_t e=call(W32R_QUERY,key,NULL,name,wide,0,0,0,0,NULL,0);
    if(!e)e=query_data(data,size,type,tail,wide);return end(e);
}
LONG W32API RegQueryValueExA(uint32_t k,const char *n,void *r,uint32_t *t,uint8_t *d,uint32_t *s){return query(k,n,r,t,d,s,0);}
LONG W32API RegQueryValueExW(uint32_t k,const WCHAR *n,void *r,uint32_t *t,uint8_t *d,uint32_t *s){return query(k,n,r,t,d,s,1);}
static LONG remove_key(uint32_t k,const void *p,int wide,int value)
{return end(call(value?W32R_DELETE_VALUE:W32R_DELETE_KEY,k,value?NULL:p,value?p:NULL,wide,0,0,0,0,NULL,0));}
LONG W32API RegDeleteKeyA(uint32_t k,const char *p){return remove_key(k,p,0,0);}
LONG W32API RegDeleteKeyW(uint32_t k,const WCHAR *p){return remove_key(k,p,1,0);}
LONG W32API RegDeleteValueA(uint32_t k,const char *p){return remove_key(k,p,0,1);}
LONG W32API RegDeleteValueW(uint32_t k,const WCHAR *p){return remove_key(k,p,1,1);}
static LONG enumeration(uint32_t key,uint32_t index,void *name,uint32_t *count,void *reserved,void *cls,uint32_t *cls_count,void *time,uint32_t *type,void *data,uint32_t *size,int wide,int values)
{
    if(!count||reserved||(!size&&data)||(cls&&!cls_count))return 87;
    uint32_t e=call(values?W32R_ENUM_VALUE:W32R_ENUM_KEY,key,NULL,NULL,wide,index,0,0,0,NULL,0);
    if(!e){
        uint32_t required=w32r_from_utf8(NULL,0,(const char *)tail,wide),capacity=*count;
        *count=required;
        if(!name||capacity<=required)e=234;
        if(values){uint32_t q=query_data(e?NULL:data,size,type,tail+reply->name_bytes+1,wide);if(q)e=q;}
        if(cls_count){if(cls&&!*cls_count)e=234;else if(cls){if(wide)((WCHAR *)cls)[0]=0;else((char *)cls)[0]=0;}*cls_count=0;}
        if(!e){w32r_from_utf8(name,capacity,(const char *)tail,wide);if(time)w32_fill(time,0,8);}
    }
    return end(e);
}
LONG W32API RegEnumKeyExA(uint32_t k,uint32_t i,char *n,uint32_t *c,void *r,char *cl,uint32_t *cc,void *t){return enumeration(k,i,n,c,r,cl,cc,t,NULL,NULL,NULL,0,0);}
LONG W32API RegEnumKeyExW(uint32_t k,uint32_t i,WCHAR *n,uint32_t *c,void *r,WCHAR *cl,uint32_t *cc,void *t){return enumeration(k,i,n,c,r,cl,cc,t,NULL,NULL,NULL,1,0);}
LONG W32API RegEnumKeyA(uint32_t k,uint32_t i,char *n,uint32_t c){return enumeration(k,i,n,&c,NULL,NULL,NULL,NULL,NULL,NULL,NULL,0,0);}
LONG W32API RegEnumKeyW(uint32_t k,uint32_t i,WCHAR *n,uint32_t c){return enumeration(k,i,n,&c,NULL,NULL,NULL,NULL,NULL,NULL,NULL,1,0);}
LONG W32API RegEnumValueA(uint32_t k,uint32_t i,char *n,uint32_t *c,void *r,uint32_t *t,uint8_t *d,uint32_t *b){return enumeration(k,i,n,c,r,NULL,NULL,NULL,t,d,b,0,1);}
LONG W32API RegEnumValueW(uint32_t k,uint32_t i,WCHAR *n,uint32_t *c,void *r,uint32_t *t,uint8_t *d,uint32_t *b){return enumeration(k,i,n,c,r,NULL,NULL,NULL,t,d,b,1,1);}
static LONG info(uint32_t k,void *cls,uint32_t *cl,void *reserved,uint32_t *sub,uint32_t *maxsub,uint32_t *maxclass,uint32_t *values,uint32_t *maxvalue,uint32_t *maxdata,uint32_t *security,void *time,int wide)
{
    if(reserved||(cls&&!cl))return 87;
    uint32_t e=call(W32R_INFO,k,NULL,NULL,0,0,0,0,(uint32_t)wide,NULL,0);
    if(!e){if(sub)*sub=reply->count;if(maxsub)*maxsub=reply->max_name;if(maxclass)*maxclass=0;if(values)*values=reply->disposition;
        if(maxvalue)*maxvalue=reply->key;if(maxdata)*maxdata=reply->max_data;if(security)*security=20;if(time)w32_fill(time,0,8);
        if(cl){if(cls&&!*cl)e=234;else if(cls){if(wide)((WCHAR *)cls)[0]=0;else((char *)cls)[0]=0;}*cl=0;}}
    return end(e);
}
LONG W32API RegQueryInfoKeyA(uint32_t k,char *c,uint32_t *cl,void *r,uint32_t *s,uint32_t *ms,uint32_t *mc,uint32_t *v,uint32_t *mv,uint32_t *md,uint32_t *sd,void *t){return info(k,c,cl,r,s,ms,mc,v,mv,md,sd,t,0);}
LONG W32API RegQueryInfoKeyW(uint32_t k,WCHAR *c,uint32_t *cl,void *r,uint32_t *s,uint32_t *ms,uint32_t *mc,uint32_t *v,uint32_t *mv,uint32_t *md,uint32_t *sd,void *t){return info(k,c,cl,r,s,ms,mc,v,mv,md,sd,t,1);}
static LONG legacy_query(uint32_t key,const void *path,void *data,int32_t *size,int wide)
{
    if(!size||*size<0)return 87;uint32_t h,e=(uint32_t)open_key(key,path,0,1,&h,0,wide,NULL);if(e)return (LONG)e;
    uint32_t n=(uint32_t)*size,type;e=(uint32_t)query(h,NULL,NULL,&type,data,&n,wide);RegCloseKey(h);*size=(int32_t)n;return (LONG)e;
}
LONG W32API RegQueryValueA(uint32_t k,const char *p,char *d,int32_t *s){return legacy_query(k,p,d,s,0);}
LONG W32API RegQueryValueW(uint32_t k,const WCHAR *p,WCHAR *d,int32_t *s){return legacy_query(k,p,d,s,1);}
static LONG legacy_set(uint32_t k,const void *p,uint32_t type,const void *d,uint32_t n,int wide)
{
    (void)n;if(type!=1||!d)return 87;uint32_t len=0;if(wide){while(((const WCHAR *)d)[len]){if(len>=W32R_DATA/2-1)return 87;len++;}len=(len+1)*2;}
    else{len=w32_strlen(d)+1;if(len>W32R_DATA)return 87;}
    if(!p||(wide?!*(const WCHAR *)p:!*(const char *)p))return set(k,NULL,0,type,d,len,wide);
    uint32_t h,e=(uint32_t)open_key(k,p,0,0xf003f,&h,1,wide,NULL);if(e)return (LONG)e;
    e=(uint32_t)set(h,NULL,0,type,d,len,wide);RegCloseKey(h);return (LONG)e;
}
LONG W32API RegSetValueA(uint32_t k,const char *p,uint32_t t,const char *d,uint32_t n){return legacy_set(k,p,t,d,n,0);}
LONG W32API RegSetValueW(uint32_t k,const WCHAR *p,uint32_t t,const WCHAR *d,uint32_t n){return legacy_set(k,p,t,d,n,1);}
static int username(void *name,uint32_t *count,int wide)
{
    if(!count){w32r_last_error(87);return 0;}
    uint32_t e=call(W32R_USERNAME,0,NULL,NULL,0,0,0,0,0,NULL,0);
    if(!e){if(!name||*count<6){*count=6;e=122;}else{if(wide){for(unsigned i=0;i<6;i++)((WCHAR *)name)[i]=tail[i];}else w32_copy(name,tail,6);*count=6;}}
    end(e);if(e)w32r_last_error(e);return !e;
}
int W32API GetUserNameA(char *n,uint32_t *c){return username(n,c,0);}
int W32API GetUserNameW(WCHAR *n,uint32_t *c){return username(n,c,1);}
int W32API RevertToSelf(void){return 1;}
int W32API ImpersonateSelf(uint32_t level){if(level>3){w32r_last_error(87);return 0;}return 1;}
int W32API AccessCheck(void *sd,uint32_t token,uint32_t desired,void *mapping,void *privileges,uint32_t *length,uint32_t *granted,int *status)
{(void)sd;(void)token;(void)mapping;(void)privileges;if(!length||!granted||!status){w32r_last_error(87);return 0;}if(*length<8){*length=8;w32r_last_error(122);return 0;}*length=8;if(privileges)w32_fill(privileges,0,8);*granted=desired;*status=1;return 1;}
/* GetFileSecurityA and GetFileSecurityW are the typed W21 failures of
 * security.c (decision w21-nt-security; directive f5-20 amendment 1, item
 * 2: no file security on FAT, as on Windows 98). */

/* f5-07B: local requests use the same virtual session coordinator. */
static int shutdown_request(const void *machine,const void *message,uint32_t seconds,int force,int reboot,int wide,int aborting)
{
    if(machine && (wide?*(const WCHAR *)machine:*(const char *)machine)){w32r_last_error(53);return 0;}
    uint32_t e=call(W32R_SHUTDOWN,0,message,NULL,wide,aborting?W32R_SHUTDOWN_ABORT:W32R_SHUTDOWN_REQUEST,
        seconds,0,aborting?0u:((force?4u:0u)|(reboot?2u:1u)),NULL,0);
    end(e);if(e)w32r_last_error(e);return !e;
}
int W32API InitiateSystemShutdownA(const char *m,const char *text,uint32_t seconds,int force,int reboot)
{return shutdown_request(m,text,seconds,force,reboot,0,0);}
int W32API InitiateSystemShutdownW(const WCHAR *m,const WCHAR *text,uint32_t seconds,int force,int reboot)
{return shutdown_request(m,text,seconds,force,reboot,1,0);}
int W32API AbortSystemShutdownA(const char *m){return shutdown_request(m,NULL,0,0,0,0,1);}
int W32API AbortSystemShutdownW(const WCHAR *m){return shutdown_request(m,NULL,0,0,0,1,1);}
