/* Native shell client of the f5-07A copied registry RPC. SPDX-License-Identifier: MIT */
#include "win32_adapter.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static pthread_mutex_t registry_lock=PTHREAD_MUTEX_INITIALIZER;
static const char uninstall[]="Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
struct response { struct w32r_reply header; uint8_t bytes[W32R_MAX-sizeof(struct w32r_reply)]; };
static uint32_t call(struct response *r, unsigned op, uint32_t key, const char *path, const char *name, unsigned index)
{
    if (!path) path="";
    if (!name) name="";
    size_t p=strlen(path)+1,n=strlen(name)+1;
    if (p>W32R_PATH || n>W32R_NAME) return 87;
    size_t size=sizeof(struct w32r_request)+p+n;
    uint8_t *wire=malloc(size); if (!wire) return 8;
    struct w32r_request a={.version=W32R_VERSION,.op=op,.key=key,.index=index,
        .access=0x20019,.path_bytes=(uint32_t)p,.name_bytes=(uint32_t)n};
    memcpy(wire,&a,sizeof(a)); memcpy(wire+sizeof(a),path,p); memcpy(wire+sizeof(a)+p,name,n);
    uint32_t used=0, error=desk_w32_exchange(wire,(uint32_t)size,r,sizeof(*r),&used);
    free(wire);
    if (error) return error;
    if (used<sizeof(r->header) || r->header.version!=W32R_VERSION || r->header.name_bytes>=W32R_NAME ||
        r->header.data_bytes>W32R_DATA) return 13;
    size_t names=r->header.name_bytes ? r->header.name_bytes+1 : 0;
    if (used!=sizeof(r->header)+names+r->header.data_bytes ||
        (names && (r->bytes[names-1] || memchr(r->bytes,0,names-1)))) return 13;
    return r->header.error;
}
static int native_error(uint32_t e)
{
    switch (e) { case 0:return 0; case 2:case 3:return -ENOENT; case 5:return -EACCES;
    case 8:return -ENOMEM; case 87:case 13:return -EINVAL; case 234:return -EOVERFLOW;
    case 1062:return -EPIPE; default:return -EIO; }
}
/* Canonical UTF-16LE -> bounded UTF-8. Reject malformed strings and truncation,
 * including embedded NULs, rather than launching a changed command line. */
static uint32_t string(struct response *r, uint32_t key, const char *name, char *out, unsigned cap)
{
    out[0]=0;
    uint32_t e=call(r,W32R_QUERY,key,NULL,name,0);
    if (e) return e;
    unsigned count=r->header.data_bytes;
    if (r->header.type!=1 || r->header.name_bytes || count<2 || count%2 ||
        r->bytes[count-1] || r->bytes[count-2]) return 13;
    unsigned at=0;
    for (unsigned i=0;i+2<count;i+=2) {
        uint32_t c=r->bytes[i]|(uint32_t)r->bytes[i+1]<<8;
        if (!c) return 13;
        if (c>=0xd800 && c<=0xdbff) {
            if (i+4>=count) return 13;
            uint32_t low=r->bytes[i+2]|(uint32_t)r->bytes[i+3]<<8;
            if (low<0xdc00 || low>0xdfff) return 13;
            c=0x10000+((c-0xd800)<<10)+(low-0xdc00); i+=2;
        } else if (c>=0xdc00 && c<=0xdfff) return 13;
        unsigned bytes=c<128 ? 1 : c<2048 ? 2 : c<65536 ? 3 : 4;
        if (at+bytes>=cap) return 234;
        if (bytes==1) out[at++]=(char)c;
        else {
            out[at++]=(char)((bytes==2 ? 0xc0 : bytes==3 ? 0xe0 : 0xf0)|(c>>(6*(bytes-1))));
            for (unsigned j=bytes-1;j;j--) out[at++]=(char)(0x80|((c>>(6*(j-1)))&63));
        }
    }
    out[at]=0; return 0;
}
int ciuki_win32_uninstall_list(struct desk_uninstall_entry *out, unsigned cap)
{
    if (!out && cap) return -EINVAL;
    if (cap>DESK_UNINSTALL_MAX) cap=DESK_UNINSTALL_MAX;
    if (!cap) return 0;
    struct response *r=malloc(sizeof(*r)); if (!r) return -ENOMEM;
    pthread_mutex_lock(&registry_lock);
    uint32_t error=call(r,W32R_OPEN,W32R_HKLM,uninstall,NULL,0),root=0;
    unsigned count=0;
    if (!error) root=r->header.key;
    else if (error==2) error=0; /* missing uninstall root is an empty list */
    /* Limit both returned records and scanned keys. The hive can be untrusted. */
    for (unsigned index=0;root && !error && index<1024 && count<cap;index++) {
        error=call(r,W32R_ENUM_KEY,root,NULL,NULL,index);
        if (error==259) {error=0;break;}
        if (error) break;
        unsigned n=r->header.name_bytes;
        if (!n || n>=sizeof(out[count].key) || !desk_utf8_valid(r->bytes,n) || strchr((char *)r->bytes,'\\') || strchr((char *)r->bytes,'/')) continue;
        struct desk_uninstall_entry entry={0}; memcpy(entry.key,r->bytes,n+1);
        error=call(r,W32R_OPEN,root,entry.key,NULL,0);
        if (error==2) {error=0;continue;}
        if (error) break;
        uint32_t key=r->header.key;
        uint32_t e=string(r,key,"DisplayName",entry.name,sizeof(entry.name));
        if (!e) e=string(r,key,"UninstallString",entry.command,sizeof(entry.command));
        if (!e) {uint32_t q=string(r,key,"DisplayVersion",entry.version,sizeof(entry.version));if(q && q!=2)e=q;}
        if (!e) {uint32_t q=string(r,key,"Publisher",entry.publisher,sizeof(entry.publisher));if(q && q!=2)e=q;}
        uint32_t closed=call(r,W32R_CLOSE,key,NULL,NULL,0);
        if (closed) {error=closed;break;}
        if (e==1062) {error=e;break;}
        if (!e && entry.name[0] && entry.command[0]) out[count++]=entry;
    }
    if (root) {uint32_t e=call(r,W32R_CLOSE,root,NULL,NULL,0);if(!error)error=e;}
    pthread_mutex_unlock(&registry_lock);free(r);
    return error ? native_error(error) : (int)count;
}
int ciuki_win32_uninstall_run(const char *key)
{
    if (!key || !*key || strlen(key)>=260 || strchr(key,'\\') || strchr(key,'/')) return -EINVAL;
    char path[sizeof(uninstall)+260];snprintf(path,sizeof(path),"%s\\%s",uninstall,key);
    struct response *r=malloc(sizeof(*r));if(!r)return -ENOMEM;
    struct desk_launch_record *record=malloc(sizeof(*record));
    if (!record) {free(r);return -ENOMEM;}
    pthread_mutex_lock(&registry_lock);
    uint32_t error=call(r,W32R_OPEN,W32R_HKLM,path,NULL,0);
    char command[1284];
    if (!error) {
        uint32_t h=r->header.key;
        error=string(r,h,"UninstallString",command,sizeof(command));
        uint32_t e=call(r,W32R_CLOSE,h,NULL,NULL,0);if(!error)error=e;
    }
    pthread_mutex_unlock(&registry_lock);free(r);
    int result=error ? native_error(error) : !desk_uninstall_command(command,record) ? -EINVAL :
        ciuki_win32_shell_launch(record);
    free(record);return result;
}
int desk_w32_flush(void)
{
    struct response *r=malloc(sizeof(*r));if(!r)return -ENOMEM;
    pthread_mutex_lock(&registry_lock);
    uint32_t e=call(r,W32R_FLUSH,W32R_HKLM,NULL,NULL,0);
    pthread_mutex_unlock(&registry_lock);free(r);return native_error(e);
}
