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
int main(int argc,char **argv)
{
    CHECK(argc>=2);
    if(!strcmp(argv[1],"compat"))compatibility();else if(!strcmp(argv[1],"basics"))basics();else if(!strcmp(argv[1],"limits"))limits();else if(!strcmp(argv[1],"faults"))faults();
    else if(!strcmp(argv[1],"lifecycle")){lifecycle();reconciliation();default_deletions();}else if(!strcmp(argv[1],"concurrent"))concurrent();else if(!strcmp(argv[1],"malformed"))malformed();
    else if(!strcmp(argv[1],"version")){CHECK(argc==3);version_tests(argv[2]);}else if(!strcmp(argv[1],"lz")){CHECK(argc==3);lz_tests(argv[2]);}else CHECK(0);
    printf("w32srv %s checks=%u PASS\n",argv[1],checks);return 0;
}
