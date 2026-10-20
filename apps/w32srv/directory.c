/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct atom { struct atom *next; uint32_t id,pid,refs; char *name; };
struct window { struct window *next; struct w32r_window value; char *title; };
struct clip_data { struct clip_data *next; uint32_t format,bytes; uint8_t data[]; };
struct directory { uint32_t clip_pid,clip_tid,clip_window,clip_owner,clip_owner_pid,clip_viewer; struct clip_data *clip; struct atom *atoms; struct window *windows; uint32_t next_atom; uint64_t sequence; unsigned slot; };
static uint32_t rd(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
void wr_directory_free(struct wr_server *s)
{
    struct directory *d=s->directory;if(!d)return;
    while(d->clip){struct clip_data *c=d->clip;d->clip=c->next;free(c);}
    while(d->atoms){struct atom *a=d->atoms;d->atoms=a->next;free(a->name);free(a);}
    while(d->windows){struct window *w=d->windows;d->windows=w->next;free(w->title);free(w);}free(d);s->directory=NULL;
}
static uint32_t save(struct wr_server *s)
{
    struct directory *d=s->directory;size_t n=56;unsigned ac=0,wc=0;
    if(d->sequence>=UINT64_MAX-1)return 1016;
    for(struct atom *a=d->atoms;a;a=a->next){n+=16+strlen(a->name)+1;ac++;}
    for(struct window *w=d->windows;w;w=w->next){n+=28+strlen(w->title)+1;wc++;}
    if(n>W32R_TEXT)return 8;
    uint8_t *b=calloc(1,n);if(!b)return 8;memcpy(b,"CRS1",4);put(b+4,1);
    uint64_t seq=d->sequence+1;put(b+12,(uint32_t)seq);put(b+16,(uint32_t)(seq>>32));put(b+20,(uint32_t)n-40);
    put(b+40,s->reserved_id);put(b+44,d->next_atom);put(b+48,ac);put(b+52,wc);uint8_t *p=b+56;
    for(struct atom *a=d->atoms;a;a=a->next){uint32_t len=(uint32_t)strlen(a->name)+1;put(p,a->id);put(p+4,a->pid);put(p+8,a->refs);put(p+12,len);memcpy(p+16,a->name,len);p+=16+len;}
    for(struct window *w=d->windows;w;w=w->next){uint32_t len=(uint32_t)strlen(w->title)+1;memcpy(p,&w->value,24);put(p+24,len);memcpy(p+28,w->title,len);p+=28+len;}
    put(b+24,wr_crc(b+40,n-40));put(b+28,wr_crc(b,28));
    unsigned slot=d->sequence?1-d->slot:0;char name[24];snprintf(name,sizeof(name),"SERVICE.CRS.%u",slot);
    uint8_t marker[4];put(marker,0x43524731);
    int ok=!s->io.write(s->io.opaque,name,b,n)&&!s->io.sync(s->io.opaque,name)&&!s->io.patch(s->io.opaque,name,32,marker,4)&&!s->io.sync(s->io.opaque,name);
    free(b);if(!ok){s->io.log(s->io.opaque,"[win32] registry directory-commit-failed");return 1016;}
    d->slot=slot;d->sequence=seq;return 0;
}
static int parse(struct wr_server *s,const uint8_t *b,size_t n)
{
    if(n<56||memcmp(b,"CRS1",4)||rd(b+4)!=1||rd(b+8)||rd(b+36)||rd(b+20)!=n-40||rd(b+32)!=0x43524731||
       rd(b+24)!=wr_crc(b+40,n-40)||rd(b+28)!=wr_crc(b,28))return 0;
    uint64_t seq=(uint64_t)rd(b+12)|((uint64_t)rd(b+16)<<32);if(!seq||seq==UINT64_MAX)return 0;
    uint32_t reserved=rd(b+40),next=rd(b+44),ac=rd(b+48),wc=rd(b+52);
    if(!reserved||reserved>0x7ffff000u||next<0xc000||next>0x10000||ac>16384||wc>4096)return 0;
    struct directory *d=calloc(1,sizeof(*d));if(!d)return 0;s->directory=d;d->sequence=seq;d->next_atom=next;
    const uint8_t *p=b+56,*end=b+n;
    for(uint32_t i=0;i<ac;i++){
        if((size_t)(end-p)<16)return 0;
        uint32_t id=rd(p),pid=rd(p+4),refs=rd(p+8),len=rd(p+12);
        if(id<0xc000||id>=next||!refs||!len||len>W32R_NAME||len>(size_t)(end-p)-16||p[16+len-1]||memchr(p+16,0,len-1)||!wr_name((const char *)p+16))return 0;
        for(struct atom *a=d->atoms;a;a=a->next)if(a->id==id||(a->pid==pid&&wr_equal(a->name,(const char *)p+16)))return 0;
        struct atom *a=calloc(1,sizeof(*a));if(!a)return 0;a->name=strdup((const char *)p+16);if(!a->name){free(a);return 0;}
        a->id=id;a->pid=pid;a->refs=refs;a->next=d->atoms;d->atoms=a;p+=16+len;
    }
    for(uint32_t i=0;i<wc;i++){
        if((size_t)(end-p)<28)return 0;uint32_t len=rd(p+24);
        if(!len||len>W32R_NAME||len>(size_t)(end-p)-28||p[28+len-1]||memchr(p+28,0,len-1)||!wr_name((const char *)p+28))return 0;
        struct window *w=calloc(1,sizeof(*w));if(!w)return 0;memcpy(&w->value,p,24);
        if(!w->value.hwnd||w->value.hwnd>=reserved||!w->value.pid||!w->value.tid){free(w);return 0;}
        for(struct window *q=d->windows;q;q=q->next)if(q->value.hwnd==w->value.hwnd){free(w);return 0;}
        w->title=strdup((const char *)p+28);if(!w->title){free(w);return 0;}w->next=d->windows;d->windows=w;p+=28+len;
    }
    if(p!=end)return 0;s->reserved_id=reserved;s->next_id=reserved;return 1;
}
int wr_directory_init(struct wr_server *s)
{
    struct wr_server c[2];memset(c,0,sizeof(c));int status[2],ok[2]={0,0};
    for(unsigned i=0;i<2;i++){
        char name[24];snprintf(name,sizeof(name),"SERVICE.CRS.%u",i);uint8_t *b=NULL;size_t n=0;
        status[i]=s->io.read(s->io.opaque,name,&b,&n);if(status[i]>0)ok[i]=parse(&c[i],b,n);free(b);
    }
    if(status[0]<0||status[1]<0){wr_directory_free(&c[0]);wr_directory_free(&c[1]);return 0;}
    int chosen=ok[1]&&(!ok[0]||((struct directory *)c[1].directory)->sequence>((struct directory *)c[0].directory)->sequence)?1:ok[0]?0:-1;
    if((status[0]&&!ok[0])||(status[1]&&!ok[1]))s->io.log(s->io.opaque,"[win32] registry directory-invalid-slot fallback");
    if(chosen<0){
        wr_directory_free(&c[0]);wr_directory_free(&c[1]);if(status[0]||status[1])return 0;
        struct directory *d=calloc(1,sizeof(*d));if(!d)return 0;s->directory=d;d->next_atom=0xc000;s->next_id=s->reserved_id=1;return 1;
    }
    s->directory=c[chosen].directory;c[chosen].directory=NULL;s->reserved_id=c[chosen].reserved_id;s->next_id=s->reserved_id;
    ((struct directory *)s->directory)->slot=(unsigned)chosen;wr_directory_free(&c[0]);wr_directory_free(&c[1]);return 1;
}
uint32_t wr_new_id(struct wr_server *s)
{
    if(s->next_id>=s->reserved_id){
        if(s->reserved_id>0x7fffebffu)return 0;uint32_t old=s->reserved_id;s->reserved_id+=1024;
        if(save(s)){s->reserved_id=old;return 0;}
    }
    return s->next_id++;
}
static uint32_t integer_atom(const char *s)
{
    if(*s!='#')return 0;uint32_t n=0;s++;if(!*s)return 0;
    for(;*s;s++){if(*s<'0'||*s>'9'||n>=0xc000)return 0;n=n*10+(unsigned)(*s-'0');}return n&&n<0xc000?n:0;
}
uint32_t wr_directory_call(struct wr_server *s,uint32_t pid,const struct w32r_request *r,const char *name,const void *data,struct w32r_reply *out,uint8_t *bytes)
{
    struct directory *d=s->directory;
    if(r->op==W32R_USER_CLEANUP){
        if(r->key||r->type||r->flags||r->access||r->data_bytes)return 87;
        if(!r->index)return wr_directory_exit(s,pid);
        if(d->clip_pid==pid&&d->clip_tid==r->index)d->clip_pid=d->clip_tid=d->clip_window=0;
        for(;;){struct window *w=d->windows;while(w&&(w->value.pid!=pid||w->value.tid!=r->index))w=w->next;if(!w)break;
            struct w32r_request free_request={.op=W32R_WINDOW_FREE,.key=w->value.hwnd};
            uint32_t e=wr_directory_call(s,pid,&free_request,"",NULL,out,bytes);if(e)return e;}
        return 0;
    }
    if(r->op>=W32R_CLIP_OPEN&&r->op<=W32R_CLIP_CHAIN){
        out->key=d->clip_owner;out->type=d->clip_viewer;
        if(r->op==W32R_CLIP_OPEN){
            if(d->clip_pid)return 5;
            if(!r->index)return 87;
            if(r->key){struct window *w=d->windows;while(w&&w->value.hwnd!=r->key)w=w->next;if(!w||w->value.pid!=pid)return 1400;}
            d->clip_pid=pid;d->clip_tid=r->index;d->clip_window=r->key;return 0;
        }
        if(r->op==W32R_CLIP_OWNER)return 0;
        if(r->op==W32R_CLIP_VIEWER){
            struct window *w=d->windows;while(w&&w->value.hwnd!=r->key)w=w->next;
            if(!w||w->value.pid!=pid)return 1400;out->key=d->clip_viewer;d->clip_viewer=r->key;return 0;
        }
        if(r->op==W32R_CLIP_CHAIN){if(d->clip_viewer==r->key)d->clip_viewer=r->flags;return 0;}
        if(r->op==W32R_CLIP_QUERY){for(struct clip_data *c=d->clip;c;c=c->next)if(c->format==r->type){out->disposition=1;break;}return 0;}
        if(d->clip_pid!=pid||d->clip_tid!=r->index)return 1418;
        if(r->op==W32R_CLIP_CLOSE){d->clip_pid=d->clip_tid=d->clip_window=0;return 0;}
        if(r->op==W32R_CLIP_EMPTY){
            out->disposition=d->clip_owner;while(d->clip){struct clip_data *c=d->clip;d->clip=c->next;free(c);}
            d->clip_owner=d->clip_window;d->clip_owner_pid=pid;out->key=d->clip_owner;return 0;
        }
        if(r->op==W32R_CLIP_GET){for(struct clip_data *c=d->clip;c;c=c->next)if(c->format==r->type){memcpy(bytes,c->data,c->bytes);out->data_bytes=c->bytes;return 0;}return 6;}
        if(r->op==W32R_CLIP_SET){
            if(d->clip_owner_pid!=pid||!d->clip_owner)return 5;
            if(r->type!=1&&r->type!=2&&r->type!=7&&r->type!=8&&r->type!=13)return 120;
            struct clip_data *copy=malloc(sizeof(*copy)+r->data_bytes);if(!copy)return 8;
            copy->format=r->type;copy->bytes=r->data_bytes;memcpy(copy->data,data,r->data_bytes);
            struct clip_data **at=&d->clip;while(*at&&(*at)->format!=r->type)at=&(*at)->next;
            if(*at){struct clip_data *old=*at;copy->next=old->next;free(old);}else copy->next=NULL;*at=copy;return 0;
        }
        return 120;
    }
    if(r->op>=W32R_ATOM_ADD&&r->op<=W32R_ATOM_NAME){
        if(r->index>1)return 87;uint32_t scope=r->index?pid:0;
        uint32_t integer=(r->op==W32R_ATOM_ADD||r->op==W32R_ATOM_FIND)?integer_atom(name):r->key;
        if(integer&&integer<0xc000){
            out->key=integer;
            if(r->op==W32R_ATOM_NAME){int n=snprintf((char *)bytes,W32R_NAME,"#%u",integer);out->data_bytes=(uint32_t)n+1;}
            return 0;
        }
        struct atom **at=&d->atoms;
        while(*at&&((*at)->pid!=scope||((r->op==W32R_ATOM_ADD||r->op==W32R_ATOM_FIND)?!wr_equal((*at)->name,name):(*at)->id!=r->key)))at=&(*at)->next;
        struct atom *a=*at;
        if(r->op==W32R_ATOM_ADD){
            if(!*name)return 87;
            if(a){if(a->refs==UINT32_MAX)return 8;a->refs++;uint32_t e=save(s);if(e)a->refs--;else out->key=a->id;return e;}
            if(d->next_atom>65535)return 8;a=calloc(1,sizeof(*a));if(!a)return 8;a->name=strdup(name);if(!a->name){free(a);return 8;}
            a->id=d->next_atom++;a->pid=scope;a->refs=1;a->next=d->atoms;d->atoms=a;
            uint32_t e=save(s);if(e){d->atoms=a->next;d->next_atom--;free(a->name);free(a);}else out->key=a->id;return e;
        }
        if(!a)return 6;
        if(r->op==W32R_ATOM_DELETE){
            a->refs--;if(!a->refs)*at=a->next;uint32_t e=save(s);
            if(e){a->refs++;if(a->refs==1)*at=a;}else if(!a->refs){free(a->name);free(a);}return e;
        }
        out->key=a->id;if(r->op==W32R_ATOM_NAME){out->data_bytes=(uint32_t)strlen(a->name)+1;memcpy(bytes,a->name,out->data_bytes);}return 0;
    }
    if(r->op==W32R_WINDOW_ALLOC){
        if(!r->index)return 87;
        unsigned count=0;for(struct window *w=d->windows;w;w=w->next)count++;if(count>=4096)return 8;
        struct window *w=calloc(1,sizeof(*w));if(!w)return 8;w->title=strdup(name);if(!w->title){free(w);return 8;}
        w->value=(struct w32r_window){wr_new_id(s),pid,r->index,0,r->type,r->flags};if(!w->value.hwnd){free(w->title);free(w);return 1016;}
        w->next=d->windows;d->windows=w;uint32_t e=save(s);if(e){d->windows=w->next;free(w->title);free(w);}else out->key=w->value.hwnd;return e;
    }
    struct window **at=&d->windows;
    if(r->op==W32R_WINDOW_ENUM){uint32_t i=0;while(*at){if((r->flags&1||!(*at)->value.parent)&&i++==r->index)break;at=&(*at)->next;}}
    else while(*at&&(*at)->value.hwnd!=r->key)at=&(*at)->next;
    struct window *w=*at;if(!w)return r->op==W32R_WINDOW_ENUM?259:1400;
    if(r->op==W32R_WINDOW_GET||r->op==W32R_WINDOW_ENUM){
        out->key=w->value.hwnd;out->data_bytes=24+(uint32_t)strlen(w->title)+1;memcpy(bytes,&w->value,24);memcpy(bytes+24,w->title,strlen(w->title)+1);return 0;
    }
    if(r->op==W32R_WINDOW_CLASS){
        for(struct atom *a=d->atoms;a;a=a->next)if(a->id==w->value.class_atom&&(!a->pid||a->pid==w->value.pid)){out->data_bytes=(uint32_t)strlen(a->name)+1;memcpy(bytes,a->name,out->data_bytes);return 0;}return 1411;
    }
    if(w->value.pid!=pid)return 5;
    if(r->op==W32R_WINDOW_FREE){if(d->clip_owner==w->value.hwnd)d->clip_owner=d->clip_owner_pid=0;if(d->clip_viewer==w->value.hwnd)d->clip_viewer=0;*at=w->next;uint32_t e=save(s);if(e)*at=w;else{free(w->title);free(w);}return e;}
    if(r->op==W32R_WINDOW_SET){
        if(r->data_bytes<25||r->data_bytes>24+W32R_NAME)return 87;
        struct w32r_window v;memcpy(&v,data,24);const char *title=(const char *)data+24;
        if(v.hwnd!=w->value.hwnd||v.pid!=pid||!v.tid||title[r->data_bytes-25]||memchr(title,0,r->data_bytes-25)||!wr_name(title)||v.parent==v.hwnd)return 87;
        if(v.parent){struct window *p=d->windows;while(p&&p->value.hwnd!=v.parent)p=p->next;if(!p)return 1400;
            for(unsigned i=0;p&&p->value.parent&&i<=4096;i++){if(p->value.parent==v.hwnd)return 87;uint32_t id=p->value.parent;p=d->windows;while(p&&p->value.hwnd!=id)p=p->next;}}
        char *copy=strdup(title);if(!copy)return 8;char *old=w->title;struct w32r_window before=w->value;w->title=copy;w->value=v;
        uint32_t e=save(s);if(e){w->title=old;w->value=before;free(copy);}else free(old);return e;
    }
    return 120;
}
uint32_t wr_directory_exit(struct wr_server *s,uint32_t pid)
{
    struct directory *d=s->directory;
    if(d->clip_pid==pid)d->clip_pid=d->clip_tid=d->clip_window=0;
    if(d->clip_owner_pid==pid)d->clip_owner=d->clip_owner_pid=0;
    for(struct window *w=d->windows;w;w=w->next)if(w->value.pid==pid&&w->value.hwnd==d->clip_viewer)d->clip_viewer=0;
    /* Remove one at a time through the same durable mutation path. */
    for(;;){struct atom *a=d->atoms;while(a&&a->pid!=pid)a=a->next;if(!a)break;
        uint32_t refs=a->refs;a->refs=1;struct w32r_request r={.op=W32R_ATOM_DELETE,.index=1,.key=a->id};struct w32r_reply out={0};uint8_t b[1];
        uint32_t e=wr_directory_call(s,pid,&r,"",NULL,&out,b);if(e){a->refs=refs;return e;}}
    for(;;){struct window *w=d->windows;while(w&&w->value.pid!=pid)w=w->next;if(!w)break;
        struct w32r_request r={.op=W32R_WINDOW_FREE,.key=w->value.hwnd};struct w32r_reply out={0};uint8_t b[1];
        uint32_t e=wr_directory_call(s,pid,&r,"",NULL,&out,b);if(e)return e;}
    return 0;
}
/* The first server of a new desktop lifetime must discard old process
 * identities before accepting clients: a cold boot can reuse native PIDs.
 * Supervised restarts within that desktop preserve/reconcile them instead. */
uint32_t wr_clients_reset(struct wr_server *s)
{
    struct directory *d=s->directory;
    for(;;){
        uint32_t pid=d->windows?d->windows->value.pid:0;
        if(!pid)for(struct atom *a=d->atoms;a;a=a->next)if(a->pid){pid=a->pid;break;}
        if(!pid){s->live_count=0;return 0;}
        uint32_t e=wr_exit(s,pid);if(e)return e;
    }
}
uint32_t wr_reconcile(struct wr_server *s,int (*alive)(void *,uint32_t),void *opaque)
{
    /* A client can exit while the server is down. Preserve surviving owners'
     * identities and remove only positively identified dead process state. */
    struct directory *d=s->directory;
    for(;;){
        uint32_t dead=0;
        for(struct atom *a=d->atoms;a&&!dead;a=a->next)if(a->pid){int state=alive(opaque,a->pid);if(state<0)return 1016;if(!state)dead=a->pid;}
        for(struct window *w=d->windows;w&&!dead;w=w->next){int state=alive(opaque,w->value.pid);if(state<0)return 1016;if(!state)dead=w->value.pid;}
        if(!dead){
            for(struct atom *a=d->atoms;a;a=a->next)if(a->pid && wr_equal(a->name,WR_CLIENT_MARKER)){
                uint32_t e=wr_client_join(s,a->pid);if(e)return e;
            }
            return 0;
        }
        uint32_t e=wr_exit(s,dead);if(e)return e;
    }
}
