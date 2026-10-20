/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Production server and public API tests. No guest or image construction. */
#define _POSIX_C_SOURCE 200809L
#include "../../../apps/w32srv/registry.h"
#include "../../../sdk/win32/services/files.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { checks++; if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);abort();} } while(0)
static unsigned checks;
struct file {char name[32];uint8_t *data;size_t n;};
struct disk {struct file f[6];int event,fail;size_t cut;unsigned logs;};
static struct file *file(struct disk *d,const char *name)
{
    struct file *empty=NULL;for(unsigned i=0;i<6;i++){if(!strcmp(d->f[i].name,name))return &d->f[i];if(!d->f[i].name[0])empty=&d->f[i];}
    CHECK(empty);snprintf(empty->name,sizeof(empty->name),"%s",name);return empty;
}
static void clear(struct disk *d){for(unsigned i=0;i<6;i++)free(d->f[i].data);memset(d,0,sizeof(*d));}
static void clone(struct disk *out,const struct disk *in)
{
    memset(out,0,sizeof(*out));for(unsigned i=0;i<6;i++)if(in->f[i].name[0]){
        struct file *f=&out->f[i];*f=in->f[i];f->data=malloc(f->n?f->n:1);CHECK(f->data);if(f->n)memcpy(f->data,in->f[i].data,f->n);}
}
static int read_memory(void *o,const char *name,uint8_t **out,size_t *n)
{
    struct disk *d=o;for(unsigned i=0;i<6;i++)if(!strcmp(d->f[i].name,name)){
        *n=d->f[i].n;*out=malloc(*n?*n:1);CHECK(*out);if(*n)memcpy(*out,d->f[i].data,*n);return 1;}
    return 0;
}
static int write_memory(void *o,const char *name,const void *data,size_t n)
{
    struct disk *d=o;int fail=++d->event==d->fail;struct file *f=file(d,name);free(f->data);if(fail&&n>d->cut)n=d->cut;
    f->data=malloc(n?n:1);CHECK(f->data);if(n)memcpy(f->data,data,n);f->n=n;return fail?-1:0;
}
static int patch_memory(void *o,const char *name,size_t off,const void *data,size_t n)
{
    struct disk *d=o;struct file *f=file(d,name);int fail=++d->event==d->fail;if(fail&&n>d->cut)n=d->cut;
    CHECK(off+n<=f->n);if(n)memcpy(f->data+off,data,n);return fail?-1:0;
}
static int sync_memory(void *o,const char *n){(void)n;struct disk *d=o;return ++d->event==d->fail?-1:0;}
static void log_memory(void *o,const char *n){CHECK(!strncmp(n,"[win32] registry",16));((struct disk *)o)->logs++;}
static struct wr_io io(struct disk *d){return (struct wr_io){d,read_memory,write_memory,patch_memory,sync_memory,log_memory};}
static struct wr_server *current;
static uint32_t client=10,last_error,request_id,corrupt_reply;
static struct w32r_reply r;
static uint8_t result[W32R_MAX];
static uint32_t rpc(struct wr_server *s,uint32_t pid,unsigned op,uint32_t key,const char *path,const char *name,unsigned type,const void *data,uint32_t n,uint32_t index)
{
    uint8_t input[W32R_MAX];struct w32r_request a={1,op,key,index,type,0xf003f,0,(uint32_t)strlen(path)+1,(uint32_t)strlen(name)+1,n};
    if (op==W32R_PROFILE_ACQUIRE || op==W32R_PROFILE_RELEASE) a.access=0;
    memcpy(input,&a,40);memcpy(input+40,path,a.path_bytes);memcpy(input+40+a.path_bytes,name,a.name_bytes);if(n)memcpy(input+40+a.path_bytes+a.name_bytes,data,n);
    size_t used;CHECK(!wr_dispatch(s,pid,input,40+a.path_bytes+a.name_bytes+n,result,sizeof(result),&used));memcpy(&r,result,40);return r.error;
}
uint32_t w32r_exchange(const void *input,uint32_t n,void *output,uint32_t cap,uint32_t *used)
{
    if(!current)return 1062;
    struct wr_session session={.pid=client};struct w32r_packet p,q;uint32_t at=0,total=0,id=++request_id;
    while(at<n){memset(&p,0,sizeof(p));p.version=1;p.id=id;p.kind=at?W32R_WRITE:W32R_BEGIN;p.total=n;p.offset=at;
        uint32_t chunk=n-at;if(chunk>232)chunk=232;memcpy(p.bytes,(const uint8_t *)input+at,chunk);CHECK(!wr_packet(current,&session,&p,&q));CHECK(!q.error);at+=chunk;CHECK(q.offset==at);total=q.total;}
    CHECK(total<=cap);at=0;
    while(at<total){memset(&p,0,sizeof(p));p.version=1;p.id=id;p.kind=W32R_READ;p.total=total;p.offset=at;CHECK(!wr_packet(current,&session,&p,&q));CHECK(!q.error);
        uint32_t chunk=total-at;if(chunk>232)chunk=232;memcpy((uint8_t *)output+at,q.bytes,chunk);at+=chunk;CHECK(q.offset==at);}
    *used=total;wr_session_close(&session);
    if(corrupt_reply==1)*used=0;
    else if(corrupt_reply==2)((struct w32r_reply *)output)->version=2;
    else if(corrupt_reply==3)((struct w32r_reply *)output)->name_bytes=W32R_NAME;
    else if(corrupt_reply==4)((struct w32r_reply *)output)->data_bytes=W32R_DATA+1;
    else if(corrupt_reply==5)*used=total+1;
    else if(corrupt_reply==6){((struct w32r_reply *)output)->name_bytes=1;((uint8_t *)output)[40]='a';((uint8_t *)output)[41]='b';*used=42;}
    return 0;
}
void w32r_pause(void){}
void w32r_last_error(uint32_t e){last_error=e;}
uint32_t last_error_value(void){return last_error;}
void *w32lz_alloc(uint32_t n){return malloc(n?n:1);}
void w32lz_free(void *p){free(p);}
int w32v_open(const void *p,int wide,struct w32v_file *f)
{
    char path[1024];if(wide){unsigned i=0;for(;((const WCHAR *)p)[i]&&i<1023;i++)path[i]=(char)((const WCHAR *)p)[i];path[i]=0;p=path;}
    FILE *in=fopen(p,"rb");if(!in)return 0;if(fseek(in,0,SEEK_END)){fclose(in);return 0;}long n=ftell(in);if(n<0||n>UINT32_MAX){fclose(in);return 0;}
    f->handle=(uintptr_t)in;f->size=(uint32_t)n;return 1;
}
int w32v_read(struct w32v_file *f,uint32_t off,void *out,uint32_t n){return off<=f->size&&n<=f->size-off&&!fseek((FILE *)f->handle,(long)off,SEEK_SET)&&fread(out,1,n,(FILE *)f->handle)==n;}
void w32v_close(struct w32v_file *f){fclose((FILE *)f->handle);}
int32_t _lread(int32_t f,void *b,uint32_t n){return (int32_t)read(f,b,n);}
int32_t _lwrite(int32_t f,const void *b,uint32_t n){return (int32_t)write(f,b,n);}
int32_t _llseek(int32_t f,int32_t o,int32_t w){return (int32_t)lseek(f,o,w);}
int32_t _lclose(int32_t f){return close(f);}
int32_t OpenFile(const char *p,void *o,uint32_t style){(void)o;return open(p,(style&3)==1?O_WRONLY|O_CREAT|O_TRUNC:O_RDONLY,0600);}
int WideCharToMultiByte(uint32_t cp,uint32_t flags,const WCHAR *s,int32_t n,char *out,int32_t cap,const char *fallback,int *used)
{(void)cp;(void)flags;(void)fallback;(void)used;int32_t i=0;while(n<0||i<n){if(i>=cap)return 0;out[i]=(char)s[i];if(!s[i++])break;}return i;}
int MultiByteToWideChar(uint32_t cp,uint32_t flags,const char *s,int32_t n,WCHAR *out,int32_t cap)
{(void)cp;(void)flags;int32_t i=0;while(n<0||i<n){if(i>=cap)return 0;out[i]=(uint8_t)s[i];if(!s[i++])break;}return i;}
#define DECL(ret,n,args) extern ret n args
DECL(LONG,RegCreateKeyA,(uint32_t,const char *,uint32_t *));
DECL(LONG,RegCreateKeyExA,(uint32_t,const char *,uint32_t,char *,uint32_t,uint32_t,void *,uint32_t *,uint32_t *));
DECL(LONG,RegOpenKeyExA,(uint32_t,const char *,uint32_t,uint32_t,uint32_t *));
DECL(LONG,RegOpenKeyW,(uint32_t,const WCHAR *,uint32_t *));
DECL(LONG,RegCloseKey,(uint32_t));DECL(LONG,RegFlushKey,(uint32_t));
DECL(LONG,RegDeleteKeyA,(uint32_t,const char *));DECL(LONG,RegDeleteValueA,(uint32_t,const char *));
DECL(LONG,RegSetValueExA,(uint32_t,const char *,uint32_t,uint32_t,const uint8_t *,uint32_t));
DECL(LONG,RegSetValueExW,(uint32_t,const WCHAR *,uint32_t,uint32_t,const uint8_t *,uint32_t));
DECL(LONG,RegQueryValueExA,(uint32_t,const char *,void *,uint32_t *,uint8_t *,uint32_t *));
DECL(LONG,RegQueryValueExW,(uint32_t,const WCHAR *,void *,uint32_t *,uint8_t *,uint32_t *));
DECL(LONG,RegEnumKeyExA,(uint32_t,uint32_t,char *,uint32_t *,void *,char *,uint32_t *,void *));
DECL(LONG,RegEnumValueA,(uint32_t,uint32_t,char *,uint32_t *,void *,uint32_t *,uint8_t *,uint32_t *));
DECL(LONG,RegQueryInfoKeyA,(uint32_t,char *,uint32_t *,void *,uint32_t *,uint32_t *,uint32_t *,uint32_t *,uint32_t *,uint32_t *,uint32_t *,void *));
DECL(int,GetUserNameA,(char *,uint32_t *));
static uint32_t create(struct wr_server *s,const char *path){CHECK(!rpc(s,10,W32R_CREATE,W32R_HKLM,path,"",0,NULL,0,0));return r.key;}
static void start(struct wr_server *s,struct disk *d){struct wr_io i=io(d);CHECK(wr_init(s,&i,0));current=s;}
static uint32_t get(struct wr_server *s,uint32_t key,const char *name){return rpc(s,10,W32R_QUERY,key,"",name,0,NULL,0,0);}
static void basics(void)
{
    struct disk d={0};struct wr_server s;start(&s,&d);uint32_t k=0,disposition=0;
    CHECK(!RegCreateKeyExA(W32R_HKLM,"Software\\Example\\Child",0,NULL,0,0xf003f,NULL,&k,&disposition));CHECK(disposition==1);
    CHECK(!RegCloseKey(k));CHECK(!RegCreateKeyExA(W32R_HKLM,"Software\\Example\\Child",0,NULL,0,0xf003f,NULL,&k,&disposition));CHECK(disposition==2);
    CHECK(!RegSetValueExA(k,NULL,0,1,(const uint8_t *)"hello",5));uint8_t out[32];memset(out,0xcc,sizeof(out));uint32_t size=0,type=99;
    CHECK(!RegQueryValueExA(k,"",NULL,&type,NULL,&size));CHECK(size==5&&type==1);size=4;
    CHECK(RegQueryValueExA(k,NULL,NULL,&type,out,&size)==234);CHECK(size==5&&out[0]==0xcc);size=sizeof(out);
    CHECK(!RegQueryValueExA(k,NULL,NULL,&type,out,&size));CHECK(!memcmp(out,"hello",5)&&out[5]==0xcc);
    CHECK(RegQueryValueExA(k,"missing",NULL,NULL,NULL,&size)==2);CHECK(RegQueryValueExA(k,NULL,(void *)1,NULL,NULL,NULL)==87);
    uint32_t read_only;CHECK(!RegOpenKeyExA(W32R_HKLM,"software\\EXAMPLE\\child",0,1,&read_only));
    CHECK(RegSetValueExA(read_only,"bad",0,3,(uint8_t *)"x",1)==5);CHECK(!RegCloseKey(read_only));
    CHECK(RegOpenKeyExA(W32R_HKLM,"missing",0,1,&read_only)==2);
    CHECK(RegOpenKeyExA(123456,"",0,1,&read_only)==6);
    CHECK(RegCreateKeyExA(W32R_HKLM,"Bad",1,NULL,0,1,NULL,&read_only,NULL)==87);
    CHECK(RegOpenKeyExA(k,"",1,1,&read_only)==87);
    CHECK(RegSetValueExA(k,"type",0,99,out,1)==87);CHECK(RegSetValueExA(k,"dword",0,4,out,3)==87);
    char name[32];uint32_t count=sizeof(name);size=sizeof(out);
    CHECK(!RegEnumValueA(k,0,name,&count,NULL,&type,out,&size));CHECK(!name[0]&&!count&&type==1&&size==5);
    count=sizeof(name);CHECK(RegEnumValueA(k,1,name,&count,NULL,NULL,NULL,NULL)==259);
    uint32_t parent;CHECK(!RegOpenKeyExA(W32R_HKLM,"Software\\Example",0,0xf003f,&parent));count=1;
    CHECK(RegEnumKeyExA(parent,0,name,&count,NULL,NULL,NULL,NULL)==234);CHECK(count==5);count=sizeof(name);
    CHECK(!RegEnumKeyExA(parent,0,name,&count,NULL,NULL,NULL,NULL));CHECK(!strcmp(name,"Child"));
    uint32_t sub,maxsub,values,maxvalue,maxdata;
    CHECK(!RegQueryInfoKeyA(parent,NULL,NULL,NULL,&sub,&maxsub,NULL,&values,&maxvalue,&maxdata,NULL,NULL));CHECK(sub==1&&maxsub==5&&values==0);
    CHECK(!RegDeleteKeyA(W32R_HKLM,"Software\\Example"));size=32;CHECK(RegQueryValueExA(k,NULL,NULL,NULL,out,&size)==1018);CHECK(!RegCloseKey(k));CHECK(!RegCloseKey(parent));CHECK(RegCloseKey(k)==6);
    CHECK(RegDeleteKeyA(W32R_HKLM,"")==5);CHECK(RegDeleteKeyA(W32R_HKLM,"missing")==2);
    CHECK(RegDeleteValueA(W32R_HKLM,"missing")==2);
    uint32_t cc;CHECK(!RegOpenKeyExA(W32R_HKCC,"Display\\Settings",0,0xf003f,&cc));size=4;CHECK(!RegQueryValueExA(cc,"BitsPerPixel",NULL,&type,out,&size));CHECK(type==4&&out[0]==16);
    CHECK(RegSetValueExA(cc,"BitsPerPixel",0,4,out,4)==5);CHECK(!RegCloseKey(cc));CHECK(!RegCloseKey(W32R_HKDD));
    char user[6];count=1;CHECK(!GetUserNameA(user,&count)&&count==6&&last_error==122);count=6;CHECK(GetUserNameA(user,&count)&&!strcmp(user,"Ciuki"));
    uint32_t cu,alias;CHECK(!RegCreateKeyA(W32R_HKCU,"Example",&cu));CHECK(!RegOpenKeyExA(W32R_HKU,".DEFAULT\\example",0,1,&alias));CHECK(!RegCloseKey(alias));CHECK(!RegCloseKey(cu));
    CHECK(!RegCreateKeyA(W32R_HKLM,"CrossEncoding",&cu));
    const uint8_t cp1252[]={0x80,0xe9,0};WCHAR value_name[]={'n',0};
    CHECK(!RegSetValueExA(cu,"n",0,1,cp1252,sizeof(cp1252)));size=sizeof(out);
    CHECK(!RegQueryValueExW(cu,value_name,NULL,&type,out,&size));CHECK(size==6&&w32_u16(out)==0x20ac&&w32_u16(out+2)==0xe9);
    const uint8_t multi[]={0x61,0,0,0,0x62,0,0,0,0,0};CHECK(!RegSetValueExW(cu,value_name,0,7,multi,sizeof(multi)));
    size=sizeof(out);CHECK(!RegQueryValueExA(cu,"n",NULL,&type,out,&size));CHECK(size==5&&!memcmp(out,"a\0b\0\0",5));CHECK(!RegCloseKey(cu));
    WCHAR unicode[]={0x53,0x74,0x72,0x61,0xdf,0x65,0};CHECK(!RegCreateKeyA(W32R_HKLM,"STRASSE",&alias));CHECK(!RegOpenKeyW(W32R_HKLM,unicode,&cu));CHECK(!RegCloseKey(cu));CHECK(!RegCloseKey(alias));
    CHECK(!RegFlushKey(W32R_HKLM));current=NULL;CHECK(RegFlushKey(W32R_HKLM)==1062);current=&s;
    for(unsigned i=1;i<=6;i++){corrupt_reply=i;CHECK(RegFlushKey(W32R_HKLM)==13);}corrupt_reply=0;
    wr_destroy(&s);clear(&d);
}
static void limits(void)
{
    struct disk d={0};struct wr_server s;start(&s,&d);char key[257],name[257];memset(key,'k',255);key[255]=0;memset(name,'v',255);name[255]=0;
    uint32_t k=create(&s,key);uint8_t *data=malloc(W32R_DATA);CHECK(data);for(uint32_t i=0;i<W32R_DATA;i++)data[i]=(uint8_t)(i*17);
    unsigned types[]={0,1,2,3,4,7};for(unsigned i=0;i<6;i++){
        char v[32];snprintf(v,sizeof(v),"type%u",types[i]);uint32_t n=types[i]==4?4:W32R_DATA;
        CHECK(!rpc(&s,10,W32R_SET,k,"",v,types[i],data,n,0));CHECK(!get(&s,k,v));CHECK(r.type==types[i]&&r.data_bytes==n&&!memcmp(result+40,data,n));}
    CHECK(!rpc(&s,10,W32R_SET,k,"",name,3,data,W32R_DATA,0));
    name[255]='v';name[256]=0;CHECK(rpc(&s,10,W32R_SET,k,"",name,3,data,1,0)==87);
    key[255]='k';key[256]=0;CHECK(rpc(&s,10,W32R_CREATE,W32R_HKLM,key,"",0,NULL,0,0)==87);
    /* Persistence preserves each byte, not merely an interpreted value. */
    CHECK(!wr_flush(&s));wr_destroy(&s);start(&s,&d);key[255]=0;k=create(&s,key);
    for(unsigned i=0;i<6;i++){char v[32];snprintf(v,sizeof(v),"type%u",types[i]);CHECK(!get(&s,k,v));uint32_t n=types[i]==4?4:W32R_DATA;CHECK(r.type==types[i]&&r.data_bytes==n&&!memcmp(result+40,data,n));}
    for(unsigned i=0;i<40;i++){char v[32];snprintf(v,sizeof(v),"large%u",i);uint32_t e=rpc(&s,10,W32R_SET,k,"",v,3,data,W32R_DATA,0);if(e){CHECK(e==8&&s.hive[0].text<=W32R_TEXT);CHECK(get(&s,k,v)==2);break;}CHECK(i<39);}
    free(data);wr_destroy(&s);clear(&d);
}
static void faults(void)
{
    struct disk base={0};struct wr_server s;start(&s,&base);uint32_t k=create(&s,"Recovery");CHECK(!rpc(&s,10,W32R_SET,k,"","value",3,"old",3,0));CHECK(!wr_flush(&s));
    CHECK(!rpc(&s,10,W32R_SET,k,"","value",3,"new",3,0));size_t bytes=s.hive[0].text+40;wr_destroy(&s);
    /* Every header byte, every body byte, and every partial marker byte.
     * Files are allowed to expose unsynced prefixes, the pessimistic model. */
    for(size_t cut=0;cut<=bytes;cut++){
        struct disk d;clone(&d,&base);start(&s,&d);k=create(&s,"Recovery");CHECK(!rpc(&s,10,W32R_SET,k,"","value",3,"new",3,0));
        d.event=0;d.fail=1;d.cut=cut;CHECK(wr_flush(&s)==1016);CHECK(s.hive[0].dirty);wr_destroy(&s);d.fail=0;start(&s,&d);
        k=create(&s,"Recovery");CHECK(!get(&s,k,"value"));CHECK(r.data_bytes==3&&!memcmp(result+40,"old",3));CHECK(d.logs>=1);wr_destroy(&s);clear(&d);
    }
    for(int stage=2;stage<=4;stage++)for(size_t cut=0;cut<=4;cut++){
        struct disk d;clone(&d,&base);start(&s,&d);k=create(&s,"Recovery");CHECK(!rpc(&s,10,W32R_SET,k,"","value",3,"new",3,0));d.event=0;d.fail=stage;d.cut=cut;
        CHECK(wr_flush(&s)==1016);CHECK(s.hive[0].dirty);wr_destroy(&s);d.fail=0;start(&s,&d);k=create(&s,"Recovery");CHECK(!get(&s,k,"value"));
        CHECK(!memcmp(result+40,"old",3)||!memcmp(result+40,"new",3));wr_destroy(&s);clear(&d);
    }
    struct disk committed;clone(&committed,&base);start(&s,&committed);k=create(&s,"Recovery");CHECK(!rpc(&s,10,W32R_SET,k,"","value",3,"new",3,0));CHECK(!wr_flush(&s));wr_destroy(&s);
    struct file *newest=file(&committed,"SYSTEM.CRG.1");
    size_t positions[]={0,4,8,12,16,20,24,28,32,36,40,63,newest->n-1};
    for(unsigned i=0;i<sizeof(positions)/sizeof(*positions);i++){
        struct disk d;clone(&d,&committed);struct file *f=file(&d,"SYSTEM.CRG.1");f->data[positions[i]]^=1;
        start(&s,&d);k=create(&s,"Recovery");CHECK(!get(&s,k,"value")&&!memcmp(result+40,"old",3));CHECK(d.logs);wr_destroy(&s);clear(&d);
    }
    clear(&committed);
    clear(&base);printf("fault byte positions=%zu\n",bytes+1);
}
static void lifecycle(void)
{
    struct disk d={0};struct wr_server s;start(&s,&d);uint32_t k=create(&s,"Persist");CHECK(!rpc(&s,10,W32R_SET,k,"","v",3,"x",1,0));
    CHECK(!wr_tick(&s,1999)&&s.hive[0].sequence==0);CHECK(!wr_tick(&s,2000)&&s.hive[0].sequence==1);
    CHECK(!rpc(&s,10,W32R_SET,k,"","v",3,"y",1,0));CHECK(!wr_exit(&s,10));CHECK(get(&s,k,"v")==6);
    CHECK(!rpc(&s,10,W32R_ATOM_ADD,0,"","Atom",0,NULL,0,0));uint32_t atom=r.key;CHECK(atom>=0xc000);
    CHECK(!rpc(&s,10,W32R_ATOM_ADD,0,"","atom",0,NULL,0,0)&&r.key==atom);
    CHECK(!rpc(&s,10,W32R_ATOM_ADD,0,"","Atom",0,NULL,0,1)&&r.key!=atom);uint32_t local=r.key;
    CHECK(!rpc(&s,10,W32R_WINDOW_ALLOC,0,"","Title",0,NULL,0,123));uint32_t window=r.key;
    uint32_t old=k;wr_destroy(&s);start(&s,&d);CHECK(get(&s,old,"v")==6);k=create(&s,"Persist");CHECK(k!=old);CHECK(!get(&s,k,"v")&&result[40]=='y');
    CHECK(!rpc(&s,20,W32R_ATOM_FIND,0,"","ATOM",0,NULL,0,0)&&r.key==atom);
    CHECK(!rpc(&s,20,W32R_WINDOW_GET,window,"","",0,NULL,0,0)&&r.key==window);
    CHECK(rpc(&s,20,W32R_WINDOW_FREE,window,"","",0,NULL,0,0)==5);
    CHECK(!rpc(&s,10,W32R_WINDOW_FREE,window,"","",0,NULL,0,0));CHECK(rpc(&s,10,W32R_WINDOW_GET,window,"","",0,NULL,0,0)==1400);
    CHECK(!wr_exit(&s,10));CHECK(rpc(&s,10,W32R_ATOM_NAME,local,"","",0,NULL,0,1)==6);
    CHECK(!rpc(&s,20,W32R_ATOM_DELETE,atom,"","",0,NULL,0,0));CHECK(!rpc(&s,20,W32R_ATOM_DELETE,atom,"","",0,NULL,0,0));
    CHECK(!rpc(&s,20,W32R_ATOM_ADD,0,"","Another",0,NULL,0,0)&&r.key!=atom);wr_destroy(&s);clear(&d);
}
static void transfer(int fd,void *data,size_t n,int sending)
{
    uint8_t *p=data;while(n){ssize_t r=sending?write(fd,p,n):read(fd,p,n);CHECK(r>0);p+=r;n-=(size_t)r;}
}
static int only_twenty(void *unused,uint32_t pid){(void)unused;return pid==20;}
static void default_deletions(void)
{
    struct disk d={0};struct wr_server s;start(&s,&d);
    uint32_t key=create(&s,"Software\\Microsoft\\Windows\\CurrentVersion");
    CHECK(!rpc(&s,10,W32R_DELETE_VALUE,key,"","RegisteredOwner",0,NULL,0,0));
    CHECK(!rpc(&s,10,W32R_DELETE_KEY,key,"Run","",0,NULL,0,0));CHECK(!wr_flush(&s));wr_destroy(&s);start(&s,&d);
    CHECK(!rpc(&s,10,W32R_OPEN,W32R_HKLM,"Software\\Microsoft\\Windows\\CurrentVersion","",0,NULL,0,0));key=r.key;
    CHECK(get(&s,key,"RegisteredOwner")==2);
    CHECK(rpc(&s,10,W32R_OPEN,key,"Run","",0,NULL,0,0)==2);
    CHECK(wr_defaults(&s,800,600,32));CHECK(!s.hive[0].dirty&&!s.hive[1].dirty);
    CHECK(get(&s,key,"RegisteredOwner")==2);
    CHECK(!rpc(&s,10,W32R_OPEN,W32R_HKCC,"Display\\Settings","",0,NULL,0,0));
    CHECK(!get(&s,r.key,"BitsPerPixel")&&result[40]==32);
    wr_destroy(&s);clear(&d);
}
static void reconciliation(void)
{
    struct disk d={0};struct wr_server s;start(&s,&d);
    CHECK(!rpc(&s,10,W32R_WINDOW_ALLOC,0,"","Dead owner",0,NULL,0,123));uint32_t dead=r.key;
    CHECK(!rpc(&s,20,W32R_WINDOW_ALLOC,0,"","Live owner",0,NULL,0,321));uint32_t live=r.key;
    CHECK(!rpc(&s,10,W32R_ATOM_ADD,0,"","Local dead",0,NULL,0,1));uint32_t atom=r.key;
    CHECK(!rpc(&s,10,W32R_ATOM_ADD,0,"","Global survives",0,NULL,0,0));uint32_t global=r.key;
    wr_destroy(&s);start(&s,&d);CHECK(!wr_reconcile(&s,only_twenty,NULL));
    CHECK(rpc(&s,20,W32R_WINDOW_GET,dead,"","",0,NULL,0,0)==1400);
    CHECK(!rpc(&s,20,W32R_WINDOW_GET,live,"","",0,NULL,0,0));
    CHECK(rpc(&s,10,W32R_ATOM_NAME,atom,"","",0,NULL,0,1)==6);
    CHECK(!rpc(&s,20,W32R_ATOM_FIND,0,"","Global survives",0,NULL,0,0)&&r.key==global);
    wr_destroy(&s);clear(&d);
}
static int nobody(void *o,uint32_t pid){(void)o;(void)pid;return 0;}
static int unknown(void *o,uint32_t pid){(void)o;(void)pid;return -1;}
static void batch_reconciliation(void)
{
    for(unsigned fresh=0;fresh<2;fresh++){
        struct disk d={0};struct wr_server s;start(&s,&d);
        CHECK(!rpc(&s,10,W32R_ATOM_ADD,0,"","Global batch",0,NULL,0,0));uint32_t global=r.key;
        for(unsigned i=0;i<50;i++){
            CHECK(!rpc(&s,100+i,W32R_CLIENT,0,"","",0,NULL,0,0));
            CHECK(!rpc(&s,100+i,W32R_WINDOW_ALLOC,0,"","Old window",0,NULL,0,1));
        }
        /* PROGMAN conversations and HWNDs share this reserved ID allocator. */
        uint32_t conversation=wr_new_id(&s);CHECK(conversation);
        uint32_t reserved=s.reserved_id,old_id=s.next_id;
        wr_destroy(&s);start(&s,&d);CHECK(s.next_id==reserved);
        d.event=0;CHECK(wr_reconcile(&s,unknown,NULL)==1016 && !d.event);
        /* One save = inactive-slot write, sync, marker patch, sync. */
        CHECK(!(fresh?wr_clients_reset(&s):wr_reconcile(&s,nobody,NULL)));
        CHECK(d.event==4 && !s.live_count && s.reserved_id==reserved && s.next_id==reserved);
        CHECK(!wr_reconcile(&s,nobody,NULL) && d.event==4);
        CHECK(rpc(&s,10,W32R_WINDOW_ENUM,0,"","",0,NULL,0,0)==259);
        CHECK(!rpc(&s,10,W32R_ATOM_FIND,0,"","Global batch",0,NULL,0,0) && r.key==global);
        wr_destroy(&s);start(&s,&d);
        CHECK(!rpc(&s,10,W32R_WINDOW_ALLOC,0,"","New window",0,NULL,0,1) && r.key>=reserved && r.key>old_id && r.key>conversation);
        CHECK(!rpc(&s,10,W32R_ATOM_ADD,0,"","New local",0,NULL,0,1) && r.key>global+50);
        wr_destroy(&s);clear(&d);
    }
    /* Failed batch commit must not publish a partially scrubbed directory. */
    for(unsigned phase=1;phase<=4;phase++){
        struct disk d={0};struct wr_server s;start(&s,&d);
        CHECK(!rpc(&s,10,W32R_WINDOW_ALLOC,0,"","Keep on failure",0,NULL,0,1));uint32_t hwnd=r.key;
        d.event=0;d.fail=(int)phase;d.cut=0;
        CHECK(wr_clients_reset(&s)==1016);
        CHECK(!rpc(&s,10,W32R_WINDOW_GET,hwnd,"","",0,NULL,0,0));
        d.fail=0;CHECK(!wr_clients_reset(&s));
        wr_destroy(&s);start(&s,&d);
        CHECK(rpc(&s,10,W32R_WINDOW_GET,hwnd,"","",0,NULL,0,0)==1400);
        wr_destroy(&s);clear(&d);
    }
    puts("PASS stale batch: 50 owners/100 objects, one write + patch + two syncs, globals/counters preserved, four commit failures");
}
static void concurrent(void)
{
    struct disk d={0};struct wr_server s;start(&s,&d);int fd[2][2];pid_t pids[2];
    for(unsigned c=0;c<2;c++){
        CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fd[c]));pids[c]=fork();CHECK(pids[c]>=0);
        if(!pids[c]){
            close(fd[c][0]);for(unsigned i=0;i<100;i++){
                uint8_t b[512];char path[80];snprintf(path,sizeof(path),"Software\\Concurrent\\Client%u\\Key%u",c,i);
                struct w32r_request a={1,W32R_CREATE,W32R_HKLM,0,0,0xf003f,0,(uint32_t)strlen(path)+1,1,0};uint32_t n=40+a.path_bytes+1;
                memcpy(b,&a,40);memcpy(b+40,path,a.path_bytes);b[40+a.path_bytes]=0;transfer(fd[c][1],&n,4,1);transfer(fd[c][1],b,n,1);struct w32r_reply out;transfer(fd[c][1],&out,40,0);CHECK(!out.error&&out.key);
                a=(struct w32r_request){1,W32R_SET,out.key,0,4,0,0,1,6,4};n=51;
                memcpy(b,&a,40);b[40]=0;memcpy(b+41,"value",6);uint32_t value=c*1000+i;memcpy(b+47,&value,4);
                transfer(fd[c][1],&n,4,1);transfer(fd[c][1],b,n,1);transfer(fd[c][1],&out,40,0);CHECK(!out.error);
            }close(fd[c][1]);_exit(0);
        }close(fd[c][1]);
    }
    for(unsigned i=0;i<100;i++)for(unsigned c=0;c<2;c++)for(unsigned phase=0;phase<2;phase++){
        uint32_t n;uint8_t input[512],out[W32R_MAX];size_t used;transfer(fd[c][0],&n,4,0);CHECK(n<=512);transfer(fd[c][0],input,n,0);
        CHECK(!wr_dispatch(&s,100+c,input,n,out,sizeof(out),&used));transfer(fd[c][0],out,used,1);
    }
    for(unsigned c=0;c<2;c++){int status;CHECK(waitpid(pids[c],&status,0)==pids[c]);CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));close(fd[c][0]);}
    CHECK(!wr_flush(&s));wr_destroy(&s);start(&s,&d);
    for(unsigned c=0;c<2;c++)for(unsigned i=0;i<100;i++){char path[80];snprintf(path,sizeof(path),"Software\\Concurrent\\Client%u\\Key%u",c,i);CHECK(!rpc(&s,10,W32R_OPEN,W32R_HKLM,path,"",0,NULL,0,0));uint32_t handle=r.key;
        CHECK(!get(&s,handle,"value")&&r.type==4&&r.data_bytes==4&&w32_u32(result+40)==c*1000+i);}
    wr_destroy(&s);clear(&d);
}
static void malformed(void)
{
    struct disk d={0};struct wr_server s;start(&s,&d);uint8_t out[W32R_MAX];size_t n;
    for(unsigned i=0;i<42;i++){uint8_t input[42]={0};CHECK(!wr_dispatch(&s,10,input,i,out,sizeof(out),&n));CHECK(((struct w32r_reply *)out)->error==87);}
    CHECK(rpc(&s,10,0,W32R_HKLM,"","",0,NULL,0,0)==120);
    CHECK(rpc(&s,10,W32R_WINDOW_ALLOC,0,"","",0,NULL,0,0)==87);
    CHECK(!rpc(&s,10,W32R_PROFILE_ACQUIRE,0,"","",0,NULL,0,0)&&s.profile_owner==10);
    CHECK(rpc(&s,20,W32R_PROFILE_ACQUIRE,0,"","",0,NULL,0,0)==170&&s.profile_owner==10);
    CHECK(rpc(&s,20,W32R_PROFILE_RELEASE,0,"","",0,NULL,0,0)==5&&s.profile_owner==10);
    CHECK(!wr_exit(&s,10)&&!s.profile_owner);
    CHECK(!rpc(&s,20,W32R_PROFILE_ACQUIRE,0,"","",0,NULL,0,0)&&s.profile_owner==20);
    CHECK(rpc(&s,20,W32R_PROFILE_RELEASE,0,"","bad",0,NULL,0,0)==87&&s.profile_owner==20);
    CHECK(!rpc(&s,20,W32R_PROFILE_RELEASE,0,"","",0,NULL,0,0)&&!s.profile_owner);
    struct wr_session session={.pid=10};struct w32r_packet p={.version=1,.id=1,.kind=W32R_WRITE,.total=40},q;
    CHECK(!wr_packet(&s,&session,&p,&q)&&q.error==87);p.kind=W32R_BEGIN;p.total=W32R_MAX+1;CHECK(!wr_packet(&s,&session,&p,&q)&&q.error==87);
    p.total=500;p.offset=0;CHECK(!wr_packet(&s,&session,&p,&q)&&!q.error);p.kind=W32R_WRITE;p.offset=231;CHECK(!wr_packet(&s,&session,&p,&q)&&q.error==87);wr_session_close(&session);
    wr_destroy(&s);clear(&d);
}
extern void version_tests(const char *);
extern void lz_tests(const char *);
static void compatibility(void)
{
    struct disk d={0};struct wr_server s;start(&s,&d);
    const char *path="Software\\CiukiOS\\Win32\\AppCompat";
    CHECK(rpc(&s,10,W32R_CREATE,W32R_HKLM,path,"",0,NULL,0,0)==5);
    s.compat_writer_pid=99;
    CHECK(!rpc(&s,99,W32R_CREATE,W32R_HKLM,path,"",0,NULL,0,0));uint32_t h=r.key;
    const char database[]="[fixture.exe]\nfile-size=12\nshims=heap-zero@1\n";
    CHECK(!rpc(&s,99,W32R_SET,h,"","Database",3,database,sizeof(database)-1,0));
    CHECK(!rpc(&s,10,W32R_OPEN,W32R_HKLM,path,"",0,NULL,0,0));uint32_t untrusted=r.key;
    CHECK(rpc(&s,10,W32R_SET,untrusted,"","Database",3,"private",7,0)==5);
    CHECK(rpc(&s,10,W32R_DELETE_VALUE,untrusted,"","Database",0,NULL,0,0)==5);
    CHECK(rpc(&s,10,W32R_DELETE_KEY,W32R_HKLM,"Software\\CiukiOS","",0,NULL,0,0)==5);
    CHECK(!rpc(&s,10,W32R_COMPAT,0,"","",0,NULL,0,0)&&r.type==3&&r.data_bytes==sizeof(database)-1&&!memcmp(result+40,database,sizeof(database)-1));
    CHECK(!wr_flush(&s));wr_destroy(&s);start(&s,&d);
    CHECK(!rpc(&s,10,W32R_COMPAT,0,"","",0,NULL,0,0)&&r.data_bytes==sizeof(database)-1);
    wr_destroy(&s);clear(&d);puts("PASS AppCompat desktop-only writer, protected ancestor deletion, read-only startup snapshot and persistence");
}
/* f5-20A ADVAPI32 entry points for the reused Wine DLLs, over the same
 * production server. Kernel32/ntdll imports of the facade are modelled. */
static unsigned forgotten,stub_diagnostics,refusals;
void w32_registry_forget(uint32_t key){(void)key;forgotten++;}
uint32_t w32_shim_stub(volatile uint32_t *once,const char *dll,const char *name,uint32_t convention)
{(void)dll;(void)name;if(!*once){*once=1;stub_diagnostics++;}last_error=convention==5?5:120;return convention==3?120:convention==4?UINT32_MAX:convention==1?0x80004001u:0;}
void w32_report_refusal(uint32_t kind,const char *dll,uint32_t error){CHECK(kind==2&&!strcmp(dll,"advapi32.dll")&&error==5);refusals++;}
void *w32r_alloc(uint32_t n){return malloc(n?n:1);}
void w32r_free(void *p){free(p);}
static uint32_t expand_ansi(const char *s,char *out,uint32_t cap)
{char t[512];const char *w=strstr(s,"%WINDIR%");if(w)snprintf(t,sizeof(t),"%.*sC:\\WINDOWS%s",(int)(w-s),s,w+8);else snprintf(t,sizeof(t),"%s",s);
 uint32_t n=(uint32_t)strlen(t)+1;if(out&&cap>=n)memcpy(out,t,n);return n;}
uint32_t ExpandEnvironmentStringsA(const char *s,char *out,uint32_t cap){return expand_ansi(s,out,cap);}
uint32_t ExpandEnvironmentStringsW(const WCHAR *s,WCHAR *out,uint32_t cap)
{char a[512],b[512];unsigned i=0;for(;s[i];i++)a[i]=(char)s[i];a[i]=0;uint32_t n=expand_ansi(a,b,sizeof(b));if(out&&cap>=n)for(i=0;i<n;i++)out[i]=(uint8_t)b[i];return n;}
static char loaded_path[300];static unsigned frees;
/* RT_STRING block 2 (ids 16..31): id 21 = "Recycle", id 16 = "" . */
static uint8_t strings_block[64];
void *LoadLibraryExA(const char *p,void *f,uint32_t flags){CHECK(!f&&flags==2);snprintf(loaded_path,sizeof(loaded_path),"%s",p);return strstr(p,"missing")?NULL:(void *)0x1000;}
int FreeLibrary(void *h){CHECK(h==(void *)0x1000);frees++;return 1;}
void *FindResourceA(void *h,const char *name,const char *type){CHECK(h==(void *)0x1000&&(uintptr_t)type==6);return (uintptr_t)name==2?strings_block:NULL;}
void *LoadResource(void *h,void *r){(void)h;return r;}
uint32_t SizeofResource(void *h,void *r){(void)h;(void)r;return sizeof(strings_block);}
DECL(LONG,RegCreateKeyExW,(uint32_t,const WCHAR *,uint32_t,WCHAR *,uint32_t,uint32_t,void *,uint32_t *,uint32_t *));
DECL(LONG,RegGetValueW,(uint32_t,const WCHAR *,const WCHAR *,uint32_t,uint32_t *,void *,uint32_t *));
DECL(LONG,RegGetValueA,(uint32_t,const char *,const char *,uint32_t,uint32_t *,void *,uint32_t *));
DECL(LONG,RegDeleteTreeW,(uint32_t,const WCHAR *));DECL(LONG,RegDeleteTreeA,(uint32_t,const char *));
DECL(LONG,RegDeleteKeyExW,(uint32_t,const WCHAR *,uint32_t,uint32_t));
DECL(LONG,RegLoadMUIStringW,(uint32_t,const WCHAR *,WCHAR *,uint32_t,uint32_t *,uint32_t,const WCHAR *));
DECL(LONG,RegLoadMUIStringA,(uint32_t,const char *,char *,uint32_t,uint32_t *,uint32_t,const char *));
DECL(LONG,RegEnumKeyExW,(uint32_t,uint32_t,WCHAR *,uint32_t *,void *,WCHAR *,uint32_t *,void *));
DECL(LONG,RegCreateKeyW,(uint32_t,const WCHAR *,uint32_t *));DECL(LONG,RegEnumKeyW,(uint32_t,uint32_t,WCHAR *,uint32_t));
DECL(LONG,RegSetValueW,(uint32_t,const WCHAR *,uint32_t,const WCHAR *,uint32_t));DECL(LONG,RegQueryValueW,(uint32_t,const WCHAR *,WCHAR *,int32_t *));
DECL(LONG,RegQueryInfoKeyW,(uint32_t,WCHAR *,uint32_t *,void *,uint32_t *,uint32_t *,uint32_t *,uint32_t *,uint32_t *,uint32_t *,uint32_t *,void *));
DECL(LONG,RegEnumValueW,(uint32_t,uint32_t,WCHAR *,uint32_t *,void *,uint32_t *,uint8_t *,uint32_t *));DECL(LONG,RegDeleteValueW,(uint32_t,const WCHAR *));
DECL(int,GetUserNameW,(WCHAR *,uint32_t *));
DECL(int,InitializeAcl,(void *,uint32_t,uint32_t));DECL(uint32_t,GetLengthSid,(const void *));
DECL(int,AddAccessAllowedAce,(void *,uint32_t,uint32_t,const void *));DECL(int,AddAccessAllowedAceEx,(void *,uint32_t,uint32_t,uint32_t,const void *));
DECL(int,AddAccessDeniedAce,(void *,uint32_t,uint32_t,const void *));DECL(int,AddAccessDeniedAceEx,(void *,uint32_t,uint32_t,uint32_t,const void *));
DECL(int,InitializeSecurityDescriptor,(void *,uint32_t));DECL(int,SetSecurityDescriptorDacl,(void *,int,void *,int));
DECL(int,SetSecurityDescriptorOwner,(void *,void *,int));DECL(int,GetCurrentHwProfileA,(void *));DECL(uint8_t,SystemFunction036,(void *,uint32_t));
DECL(void *,OpenSCManagerW,(const WCHAR *,const WCHAR *,uint32_t));DECL(void *,OpenServiceW,(void *,const WCHAR *,uint32_t));
DECL(void *,CreateServiceW,(void *,const WCHAR *,const WCHAR *,uint32_t,uint32_t,uint32_t,uint32_t,const WCHAR *,const WCHAR *,uint32_t *,const WCHAR *,const WCHAR *,const WCHAR *));
DECL(int,ChangeServiceConfigW,(void *,uint32_t,uint32_t,uint32_t,const WCHAR *,const WCHAR *,uint32_t *,const WCHAR *,const WCHAR *,const WCHAR *,const WCHAR *));
DECL(int,ChangeServiceConfig2W,(void *,uint32_t,void *));DECL(int,QueryServiceConfigW,(void *,void *,uint32_t,uint32_t *));
DECL(int,QueryServiceStatusEx,(void *,uint32_t,void *,uint32_t,uint32_t *));DECL(int,StartServiceW,(void *,uint32_t,const WCHAR **));
DECL(int,ConvertSidToStringSidW,(const void *,WCHAR **));
DECL(int,CreateProcessAsUserW,(void *,const WCHAR *,WCHAR *,void *,void *,int,uint32_t,void *,const WCHAR *,void *,void *));
DECL(int,GetFileSecurityW,(const WCHAR *,uint32_t,void *,uint32_t,uint32_t *));DECL(int,GetFileSecurityA,(const char *,uint32_t,void *,uint32_t,uint32_t *));
DECL(int,SetFileSecurityW,(const WCHAR *,uint32_t,void *));DECL(int,LookupPrivilegeValueW,(const WCHAR *,const WCHAR *,void *));
DECL(uint32_t,CommandLineFromMsiDescriptor,(const WCHAR *,WCHAR *,uint32_t *));
static const WCHAR *L16(const char *a){static WCHAR b[8][128];static unsigned n;WCHAR *w=b[n++%8];unsigned i=0;for(;a[i];i++)w[i]=(uint8_t)a[i];w[i]=0;return w;}
static int weq(const WCHAR *w,const char *a){for(;*a;a++,w++)if(*w!=(uint8_t)*a)return 0;return !*w;}
static void upper_registry(void)
{
    struct disk d={0};struct wr_server s;start(&s,&d);uint32_t k=0,disp=0;
    CHECK(!RegCreateKeyExW(W32R_HKLM,L16("Software\\Upper"),0,NULL,0,0x02000100u,NULL,&k,&disp)&&disp==1);
    CHECK(!RegSetValueExA(k,"Name",0,1,(const uint8_t *)"value",6));
    CHECK(!RegSetValueExA(k,"Path",0,2,(const uint8_t *)"%WINDIR%\\x",11));
    uint32_t five=5;CHECK(!RegSetValueExA(k,"Num",0,4,(const uint8_t *)&five,4));
    CHECK(!RegSetValueExA(k,"Bin",0,3,(const uint8_t *)"abcd",4));CHECK(!RegSetValueExA(k,"Odd",0,3,(const uint8_t *)"abc",3));
    WCHAR raw[3]={'r','a','w'};CHECK(!RegSetValueExW(k,L16("Raw"),0,1,(const uint8_t *)raw,6));
    WCHAR buffer[64];uint32_t type=0,size=sizeof(buffer);
    CHECK(!RegGetValueW(k,NULL,L16("Name"),2,&type,buffer,&size)&&type==1&&size==12&&weq(buffer,"value"));
    size=0;CHECK(!RegGetValueW(k,NULL,L16("Name"),2,&type,NULL,&size)&&size==12);
    size=4;CHECK(RegGetValueW(k,NULL,L16("Name"),2,&type,buffer,&size)==234&&size==12);
    size=sizeof(buffer);memset(buffer,0x5a,sizeof(buffer));
    CHECK(RegGetValueW(k,NULL,L16("Name"),0x20000010u,&type,buffer,&size)==1630&&buffer[0]==0&&buffer[63]==0);
    size=sizeof(buffer);CHECK(!RegGetValueW(k,NULL,L16("Path"),2,&type,buffer,&size)&&type==1&&weq(buffer,"C:\\WINDOWS\\x")&&size==26);
    size=sizeof(buffer);CHECK(!RegGetValueW(k,NULL,L16("Path"),0x10000004u,&type,buffer,&size)&&type==2&&weq(buffer,"%WINDIR%\\x"));
    CHECK(RegGetValueW(k,NULL,L16("Path"),4,&type,buffer,&size)==87);
    size=sizeof(buffer);CHECK(!RegGetValueW(k,NULL,L16("Bin"),0x18,&type,buffer,&size)&&type==3&&size==4);
    size=sizeof(buffer);CHECK(RegGetValueW(k,NULL,L16("Odd"),0x18,&type,buffer,&size)==1629);
    size=sizeof(buffer);uint32_t number=0;size=4;CHECK(!RegGetValueW(k,NULL,L16("Num"),0xffff,&type,&number,&size)&&type==4&&number==5);
    size=sizeof(buffer);CHECK(!RegGetValueW(W32R_HKLM,L16("Software\\Upper"),L16("Name"),0xffff,&type,buffer,&size)&&weq(buffer,"value"));
    size=sizeof(buffer);CHECK(RegGetValueW(k,NULL,L16("Missing"),0xffff,&type,buffer,&size)==2);
    CHECK(RegGetValueW(W32R_HKLM,L16("Software\\None"),L16("x"),0xffff,&type,NULL,&size)==2);
    CHECK(RegGetValueW(k,NULL,L16("Name"),0,&type,NULL,&size)==87&&RegGetValueW(k,NULL,L16("Name"),2,&type,buffer,NULL)==87);
    size=sizeof(buffer);CHECK(!RegGetValueW(k,NULL,L16("Raw"),2,&type,buffer,&size)&&size==8&&weq(buffer,"raw"));
    char ansi[32];size=sizeof(ansi);CHECK(!RegGetValueA(k,NULL,"Name",2,&type,ansi,&size)&&size==6&&!strcmp(ansi,"value"));
    size=sizeof(ansi);CHECK(!RegGetValueA(W32R_HKLM,"Software\\Upper","Path",2,&type,ansi,&size)&&!strcmp(ansi,"C:\\WINDOWS\\x")&&size==13);
    /* Trees. */
    uint32_t t=0;CHECK(!RegCreateKeyExA(k,"Tree\\A\\B",0,NULL,0,0xf003f,NULL,&t,NULL));CHECK(!RegSetValueExA(t,"v",0,1,(const uint8_t *)"x",2));CHECK(!RegCloseKey(t));
    CHECK(!RegCreateKeyExA(k,"Tree\\C",0,NULL,0,0xf003f,NULL,&t,NULL));CHECK(!RegCloseKey(t));
    CHECK(!RegDeleteTreeW(k,L16("Tree"))&&RegOpenKeyExA(k,"Tree",0,1,&t)==2);
    CHECK(!RegCreateKeyExA(k,"Keep\\Sub",0,NULL,0,0xf003f,NULL,&t,NULL));CHECK(!RegCloseKey(t));
    CHECK(!RegOpenKeyExA(k,"Keep",0,0xf003f,&t));CHECK(!RegSetValueExA(t,"a",0,1,(const uint8_t *)"1",2));CHECK(!RegSetValueExA(t,"b",0,1,(const uint8_t *)"2",2));
    CHECK(!RegDeleteTreeW(t,NULL));uint32_t subkeys=9,values=9;
    CHECK(!RegQueryInfoKeyA(t,NULL,NULL,NULL,&subkeys,NULL,NULL,&values,NULL,NULL,NULL,NULL)&&!subkeys&&!values);CHECK(!RegCloseKey(t));
    CHECK(!RegOpenKeyExA(k,"Keep",0,1,&t));CHECK(!RegCloseKey(t));
    CHECK(!RegDeleteTreeA(k,"Keep")&&RegOpenKeyExA(k,"Keep",0,1,&t)==2);
    CHECK(RegDeleteTreeA(k,"Absent")==2);
    CHECK(!RegCreateKeyExA(k,"Ex",0,NULL,0,0xf003f,NULL,&t,NULL));CHECK(!RegCloseKey(t));
    CHECK(RegDeleteKeyExW(k,L16("Ex"),0x100,1)==87&&!RegDeleteKeyExW(k,L16("Ex"),0x100,0)&&RegDeleteKeyExW(k,L16("Ex"),0,0)==2);
    /* MUI strings. */
    memset(strings_block,0,sizeof(strings_block));
    unsigned at=0;for(unsigned id=16;id<=21;id++){if(id==21){strings_block[at]=7;const char *r="Recycle";for(unsigned c=0;c<7;c++)strings_block[at+2+c*2]=(uint8_t)r[c];at+=16;}else at+=2;}
    CHECK(!RegSetValueExA(k,"Mui",0,1,(const uint8_t *)"@shell32.dll,-21",17));
    CHECK(!RegSetValueExA(k,"MuiEnv",0,1,(const uint8_t *)"@%WINDIR%\\x.dll,-21",20));
    CHECK(!RegSetValueExA(k,"Plain",0,1,(const uint8_t *)"Plain text",11));
    CHECK(!RegSetValueExA(k,"Empty",0,1,(const uint8_t *)"@shell32.dll,-16",17));
    CHECK(!RegSetValueExA(k,"Bad",0,1,(const uint8_t *)"@shell32.dll,21",16));
    CHECK(!RegSetValueExA(k,"Gone",0,1,(const uint8_t *)"@missing.dll,-21",17));
    uint32_t copied=0;frees=0;
    CHECK(!RegLoadMUIStringW(k,L16("Mui"),buffer,sizeof(buffer),&copied,0,L16("C:\\WINDOWS\\SYSTEM"))&&weq(buffer,"Recycle")&&copied==16);
    CHECK(!strcmp(loaded_path,"C:\\WINDOWS\\SYSTEM\\shell32.dll")&&frees==1);
    CHECK(!RegLoadMUIStringW(k,L16("MuiEnv"),buffer,sizeof(buffer),NULL,0,NULL)&&!strcmp(loaded_path,"C:\\WINDOWS\\x.dll"));
    CHECK(!RegLoadMUIStringW(k,L16("Plain"),buffer,sizeof(buffer),&copied,0,NULL)&&weq(buffer,"Plain text")&&copied==22);
    CHECK(RegLoadMUIStringW(k,L16("Plain"),buffer,8,&copied,0,NULL)==234&&copied==22);
    CHECK(!RegLoadMUIStringW(k,L16("Plain"),buffer,8,NULL,1,NULL)&&weq(buffer,"Pla"));
    CHECK(RegLoadMUIStringW(k,L16("Plain"),buffer,8,&copied,1,NULL)==87&&RegLoadMUIStringW(k,L16("Plain"),buffer,7,NULL,0,NULL)==87);
    CHECK(RegLoadMUIStringW(k,L16("Plain"),buffer,8,NULL,2,NULL)==87);
    CHECK(RegLoadMUIStringW(k,L16("Empty"),buffer,sizeof(buffer),NULL,0,NULL)==1814);
    CHECK(RegLoadMUIStringW(k,L16("Bad"),buffer,sizeof(buffer),NULL,0,NULL)==13);
    CHECK(RegLoadMUIStringW(k,L16("Gone"),buffer,sizeof(buffer),NULL,0,NULL)==2);
    CHECK(RegLoadMUIStringW(k,L16("Num"),buffer,sizeof(buffer),NULL,0,NULL)==13);
    CHECK(RegLoadMUIStringW(k,L16("Nothing"),buffer,sizeof(buffer),NULL,0,NULL)==2);
    CHECK(RegLoadMUIStringA(k,"Mui",ansi,sizeof(ansi),NULL,0,NULL)==120);
    /* The f5-07A Unicode registry entry points the reused DLLs bind. */
    uint32_t w=0;CHECK(!RegCreateKeyW(k,L16("Wide\\One"),&w));CHECK(!RegCloseKey(w));
    CHECK(!RegOpenKeyW(k,L16("Wide"),&w));WCHAR name[32];
    CHECK(!RegEnumKeyW(w,0,name,32)&&weq(name,"One")&&RegEnumKeyW(w,1,name,32)==259);
    uint32_t count=32;CHECK(!RegEnumKeyExW(w,0,name,&count,NULL,NULL,NULL,NULL)&&count==3);
    CHECK(!RegSetValueW(w,L16("One"),1,L16("text\xe9"),0));int32_t legacy=sizeof(buffer);
    CHECK(!RegQueryValueW(w,L16("One"),buffer,&legacy)&&weq(buffer,"text\xe9")&&legacy==12);
    CHECK(RegSetValueW(w,NULL,4,L16("x"),0)==87);
    CHECK(!RegSetValueExW(w,L16("v"),0,1,(const uint8_t *)L16("1"),4));
    uint32_t subs=0,vals=0;CHECK(!RegQueryInfoKeyW(w,NULL,NULL,NULL,&subs,NULL,NULL,&vals,NULL,NULL,NULL,NULL)&&subs==1&&vals==1);
    count=32;uint32_t vtype=0;CHECK(!RegEnumValueW(w,0,name,&count,NULL,&vtype,NULL,NULL)&&weq(name,"v")&&vtype==1);
    CHECK(!RegDeleteValueW(w,L16("v"))&&RegDeleteValueW(w,L16("v"))==2);CHECK(!RegCloseKey(w));
    WCHAR user[16];uint32_t user_size=16;CHECK(GetUserNameW(user,&user_size)&&user_size==6);user_size=1;
    CHECK(!GetUserNameW(user,&user_size)&&user_size==6&&last_error==122);
    forgotten=0;CHECK(!RegCloseKey(k)&&forgotten==1);CHECK(RegCloseKey(k)==6&&forgotten==1);
    wr_destroy(&s);clear(&d);
    puts("PASS f5-20A registry access normalization RegGetValue trees RegDeleteKeyEx MUI strings close notification");
}
static void upper_security(void)
{
    uint8_t sid[16]={1,2,0,0,0,0,0,5,32,0,0,0,32,2,0,0},bad_sid[8]={2,0};
    CHECK(GetLengthSid(sid)==16&&!GetLengthSid(NULL));
    uint8_t acl[64];last_error=0;
    CHECK(!InitializeAcl(acl,4,2)&&last_error==122);CHECK(!InitializeAcl(acl,64,3)&&last_error==87);CHECK(!InitializeAcl(acl,0x10000,2)&&last_error==87);
    CHECK(InitializeAcl(acl,63,2)&&acl[0]==2&&w32_u16(acl+2)==60&&!w32_u16(acl+4));
    CHECK(AddAccessAllowedAce(acl,2,0x1f01ff,sid)&&w32_u16(acl+4)==1&&acl[8]==0&&w32_u16(acl+10)==24&&w32_u32(acl+12)==0x1f01ff&&!memcmp(acl+16,sid,16));
    CHECK(AddAccessDeniedAceEx(acl,4,3,1,sid)&&w32_u16(acl+4)==2&&acl[32]==1&&acl[33]==3&&acl[0]==4);
    CHECK(!AddAccessAllowedAce(acl,2,1,sid)&&last_error==1344);
    CHECK(InitializeAcl(acl,64,2)&&!AddAccessAllowedAceEx(acl,2,0x20,1,sid)&&last_error==87);
    CHECK(!AddAccessDeniedAce(acl,2,1,bad_sid)&&last_error==1337);CHECK(!AddAccessDeniedAce(acl,9,1,sid)&&last_error==1336);
    uint8_t broken[8]={7};CHECK(!AddAccessAllowedAce(broken,2,1,sid)&&last_error==1336);
    CHECK(AddAccessDeniedAce(acl,2,1,sid)&&acl[8]==1);
    uint8_t sd[64];memset(sd,0xa5,sizeof(sd));
    CHECK(!InitializeSecurityDescriptor(sd,2)&&last_error==1305&&sd[0]==0xa5);
    CHECK(InitializeSecurityDescriptor(sd,1)&&sd[0]==1&&!w32_u16(sd+2));
    CHECK(SetSecurityDescriptorDacl(sd,1,acl,1)&&w32_u16(sd+2)==0xc);
    CHECK(SetSecurityDescriptorDacl(sd,0,acl,1)&&w32_u16(sd+2)==0);
    CHECK(SetSecurityDescriptorOwner(sd,sid,1)&&w32_u16(sd+2)==1);
    sd[3]=0x80;CHECK(!SetSecurityDescriptorDacl(sd,1,acl,0)&&last_error==1338);
    sd[0]=2;CHECK(!SetSecurityDescriptorOwner(sd,sid,0)&&last_error==1305);CHECK(!SetSecurityDescriptorDacl(NULL,1,acl,0)&&last_error==87);
    uint8_t profile[124];memset(profile,0xa5,sizeof(profile));
    CHECK(GetCurrentHwProfileA(profile)&&w32_u32(profile)==6&&profile[4]=='{'&&strlen((char *)profile+4)==38&&!strcmp((char *)profile+43,"Original Configuration"));
    CHECK(!GetCurrentHwProfileA(NULL)&&last_error==87);
    uint8_t a[37]={0},b[37]={0};CHECK(SystemFunction036(a,37)&&SystemFunction036(b,37)&&memcmp(a,b,37));
    unsigned nonzero=0;for(unsigned i=0;i<37;i++)nonzero+=a[i]!=0;CHECK(nonzero>20);
    CHECK(SystemFunction036(NULL,0)&&!SystemFunction036(NULL,1));
    stub_diagnostics=0;refusals=0;WCHAR *text=(WCHAR *)1;uint32_t length=5,needed=7;
    CHECK(!OpenSCManagerW(NULL,NULL,1)&&last_error==120);CHECK(!OpenServiceW((void *)1,L16("x"),1)&&last_error==120);
    CHECK(!CreateServiceW((void *)1,L16("x"),NULL,1,0x10,3,1,L16("x.exe"),NULL,NULL,NULL,NULL,NULL)&&last_error==120&&!refusals);
    CHECK(!CreateServiceW((void *)1,L16("x"),NULL,1,1,3,1,L16("x.sys"),NULL,NULL,NULL,NULL,NULL)&&last_error==5&&refusals==1);
    CHECK(!ChangeServiceConfigW((void *)1,0,0,0,NULL,NULL,NULL,NULL,NULL,NULL,NULL)&&last_error==120);
    CHECK(!ChangeServiceConfig2W((void *)1,1,NULL)&&!QueryServiceConfigW((void *)1,NULL,0,&needed)&&needed==7);
    CHECK(!QueryServiceStatusEx((void *)1,0,NULL,0,&needed)&&!StartServiceW((void *)1,0,NULL)&&last_error==120);
    CHECK(!ConvertSidToStringSidW(sid,&text)&&text==(WCHAR *)1&&last_error==120);
    CHECK(!CreateProcessAsUserW(NULL,NULL,NULL,NULL,NULL,0,0,NULL,NULL,NULL,NULL)&&last_error==120);
    uint8_t descriptor[20];CHECK(!GetFileSecurityW(L16("C:\\x"),4,descriptor,20,&needed)&&last_error==120&&needed==7);
    /* Amendment 1 item 2: the ANSI form fails identically (outputs untouched). */
    CHECK(!GetFileSecurityA("C:\\x",4,descriptor,20,&needed)&&last_error==120&&needed==7);
    CHECK(!SetFileSecurityW(L16("C:\\x"),4,descriptor)&&!LookupPrivilegeValueW(NULL,L16("SeBackupPrivilege"),descriptor)&&last_error==120);
    CHECK(CommandLineFromMsiDescriptor(L16("x"),NULL,&length)==120&&length==5);
    CHECK(stub_diagnostics==15);
    puts("PASS f5-20A security structures hardware profile random bytes and W21 service NT-security MSI failures");
}
/* f5-20B (directive f5-20 amendment 1, item 3): NtDeleteKey deletes the
 * key of an open handle (empty name) when the handle has DELETE access; a
 * root constant, or a handle on a key a predefined root names, is refused. */
static uint32_t rpc_access(struct wr_server *s,unsigned op,uint32_t key,const char *path,uint32_t access)
{
    uint8_t input[W32R_MAX];struct w32r_request a={1,op,key,0,0,access,0,(uint32_t)strlen(path)+1,1,0};
    memcpy(input,&a,40);memcpy(input+40,path,a.path_bytes);input[40+a.path_bytes]=0;
    size_t used;CHECK(!wr_dispatch(s,10,input,40+a.path_bytes+1,result,sizeof(result),&used));memcpy(&r,result,40);return r.error;
}
static void handle_deletion(void)
{
    struct disk d={0};struct wr_server s;start(&s,&d);
    create(&s,"Software\\Classes");CHECK(!rpc(&s,10,W32R_CREATE,W32R_HKU,".DEFAULT","",0,NULL,0,0));
    uint32_t parent=create(&s,"Software\\Doomed");create(&s,"Software\\Doomed\\Child\\Leaf");
    uint32_t leaf=r.key;CHECK(!rpc(&s,10,W32R_SET,leaf,"","v",1,"x",2,0));
    /* KEY_READ and KEY_WRITE lack DELETE: refused, nothing deleted. */
    CHECK(!rpc_access(&s,W32R_OPEN,W32R_HKLM,"Software\\Doomed",0x20019));uint32_t reader=r.key;
    CHECK(rpc_access(&s,W32R_DELETE_KEY,reader,"",0)==5);
    CHECK(!rpc_access(&s,W32R_OPEN,W32R_HKLM,"Software\\Doomed",0x20006));uint32_t writer=r.key;
    CHECK(rpc_access(&s,W32R_DELETE_KEY,writer,"",0)==5);
    CHECK(!rpc(&s,10,W32R_OPEN,W32R_HKLM,"Software\\Doomed\\Child","",0,NULL,0,0));
    /* Roots: the constants and handles on the keys they name. */
    CHECK(rpc_access(&s,W32R_DELETE_KEY,W32R_HKLM,"",0)==5);
    CHECK(rpc_access(&s,W32R_DELETE_KEY,W32R_HKU,"",0)==5);
    CHECK(rpc_access(&s,W32R_DELETE_KEY,W32R_HKCU,"",0)==5);
    CHECK(!rpc_access(&s,W32R_CREATE,W32R_HKU,"",0xf003f));uint32_t users=r.key;
    CHECK(users!=W32R_HKU&&rpc_access(&s,W32R_DELETE_KEY,users,"",0)==5);
    CHECK(!rpc_access(&s,W32R_CREATE,W32R_HKCU,"",0xf003f));
    CHECK(rpc_access(&s,W32R_DELETE_KEY,r.key,"",0)==5);
    CHECK(!rpc_access(&s,W32R_CREATE,W32R_HKCR,"",0xf003f));
    CHECK(rpc_access(&s,W32R_DELETE_KEY,r.key,"",0)==5);
    CHECK(!rpc(&s,10,W32R_OPEN,W32R_HKU,".DEFAULT","",0,NULL,0,0));
    /* DELETE access (KEY_ALL_ACCESS): the key and its subtree go; the
     * handle stays open and reports ERROR_KEY_DELETED; the parent lives. */
    CHECK(!rpc_access(&s,W32R_OPEN,W32R_HKLM,"Software\\Doomed",0xf003f));uint32_t doomed=r.key;
    CHECK(!rpc_access(&s,W32R_DELETE_KEY,doomed,"",0));
    CHECK(rpc(&s,10,W32R_OPEN,W32R_HKLM,"Software\\Doomed","",0,NULL,0,0)==2);
    CHECK(rpc(&s,10,W32R_OPEN,W32R_HKLM,"Software\\Doomed\\Child\\Leaf","",0,NULL,0,0)==2);
    CHECK(get(&s,doomed,"v")==1018&&get(&s,leaf,"v")==1018&&get(&s,parent,"v")==1018);
    CHECK(rpc_access(&s,W32R_DELETE_KEY,doomed,"",0)==1018);
    CHECK(!rpc(&s,10,W32R_OPEN,W32R_HKLM,"Software","",0,NULL,0,0));
    /* A DELETE handle on the protected compatibility tree stays refused. */
    CHECK(!rpc_access(&s,W32R_CREATE,W32R_HKLM,"Software\\CiukiOS\\Win32",0xf003f));
    CHECK(rpc_access(&s,W32R_DELETE_KEY,r.key,"",0)==5);
    CHECK(!wr_flush(&s));wr_destroy(&s);start(&s,&d);
    CHECK(rpc(&s,10,W32R_OPEN,W32R_HKLM,"Software\\Doomed","",0,NULL,0,0)==2);
    wr_destroy(&s);clear(&d);
    puts("PASS f5-20B NtDeleteKey by handle: DELETE access, subtree, ERROR_KEY_DELETED, roots and compatibility tree refused");
}
int main(int argc,char **argv)
{
    CHECK(argc>=2);
    if(!strcmp(argv[1],"compat"))compatibility();else if(!strcmp(argv[1],"basics"))basics();else if(!strcmp(argv[1],"limits"))limits();else if(!strcmp(argv[1],"faults"))faults();
    else if(!strcmp(argv[1],"lifecycle")){lifecycle();reconciliation();batch_reconciliation();default_deletions();}else if(!strcmp(argv[1],"concurrent"))concurrent();else if(!strcmp(argv[1],"malformed"))malformed();
    else if(!strcmp(argv[1],"version")){CHECK(argc==3);version_tests(argv[2]);}else if(!strcmp(argv[1],"lz")){CHECK(argc==3);lz_tests(argv[2]);}else if(!strcmp(argv[1],"upper")){upper_registry();upper_security();}else if(!strcmp(argv[1],"handle-delete"))handle_deletion();else CHECK(0);
    printf("w32srv %s checks=%u PASS\n",argv[1],checks);return 0;
}
