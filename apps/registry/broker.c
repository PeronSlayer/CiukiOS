/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "broker.h"
#include "../../sdk/libreg/internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint32_t rb_get32(const void *v){const uint8_t *p=v;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
uint64_t rb_get64(const void *v){const uint8_t *p=v;return rb_get32(p)|(uint64_t)rb_get32(p+4)<<32;}
void rb_put32(void *v,uint32_t x){uint8_t *p=v;for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(8*i));}
void rb_put64(void *v,uint64_t x){rb_put32(v,(uint32_t)x);rb_put32((uint8_t *)v+4,(uint32_t)(x>>32));}
static int admin(const struct rb_identity *s){if(!s->uid)return 1;for(unsigned i=0;i<s->group_count;i++)if(s->groups[i]==544)return 1;return 0;}
static const char *root_name(unsigned root){return root==RB_SYSTEM?"System":root==RB_PROGRAMS?"Programs":root==RB_USERS?"Users":NULL;}
static void discard(struct rb_link *l){if(l->stage){memset(l->stage,0,l->total);free(l->stage);}l->stage=NULL;l->root=l->total=l->received=0;l->expected=0;}
static int current(struct rb_broker *,struct rb_link *,struct rb_identity *);
/* sec-sysvc2 (checker.c; weak: broker fixtures without Registry Checker). */
extern int rb_checker_exchange(struct rb_checker *,const struct rb_identity *,unsigned,const uint8_t *,size_t,
                               uint8_t *,size_t,size_t *) __attribute__((weak));
static int io_live(struct rb_broker *b)
{
    if(b->active_link<0||b->active_link>=(int)RB_LINK_MAX||b->faulted)return -ESRCH;
    struct rb_identity c;return current(b,b->links+b->active_link,&c);
}
static int scoped_read(void *p,const char *name,uint8_t **out,size_t *n)
{struct rb_broker *b=p;if(io_live(b))return -1;return b->raw_io.read(b->raw_io.opaque,name,out,n);}
static int scoped_write(void *p,const char *name,const void *v,size_t n)
{struct rb_broker *b=p;if(io_live(b))return -1;return b->raw_io.write(b->raw_io.opaque,name,v,n);}
static int scoped_patch(void *p,const char *name,size_t at,const void *v,size_t n)
{struct rb_broker *b=p;if(io_live(b))return -1;return b->raw_io.patch(b->raw_io.opaque,name,at,v,n);}
static int scoped_sync(void *p,const char *name)
{struct rb_broker *b=p;if(io_live(b))return -1;return b->raw_io.sync(b->raw_io.opaque,name);}
static void scoped_log(void *p,const char *line)
{struct rb_broker *b=p;b->raw_io.log(b->raw_io.opaque,line);}
int rb_init(struct rb_broker *b,const struct rb_authority *a,const struct rb_storage *s,struct wr_server *w)
{
    if(!b||!a||!a->registry_role||!a->inspect||!a->retain||!a->release||
       !s||!s->read||!s->write)return -EINVAL;
    int e=a->registry_role(a->context);if(e)return e;
    memset(b,0,sizeof(*b));b->authority=*a;b->storage=*s;b->win32=w;b->active_link=-1;
    if(w){b->raw_io=w->io;w->io=(struct wr_io){b,scoped_read,scoped_write,scoped_patch,scoped_sync,scoped_log};}
    return 0;
}
void rb_disconnect(struct rb_broker *b,unsigned slot)
{
    if(slot>=RB_LINK_MAX)return;struct rb_link *l=b->links+slot;
    if(l->retained){if(b->win32&&!b->faulted)wr_exit(b->win32,l->id);b->authority.release(b->authority.context,l->retained);}
    discard(l);free(l->reply);memset(l,0,sizeof(*l));
}
void rb_destroy(struct rb_broker *b){for(unsigned i=0;i<RB_LINK_MAX;i++)rb_disconnect(b,i);if(b->win32)b->win32->io=b->raw_io;memset(b,0,sizeof(*b));}
int rb_connect(struct rb_broker *b,uintptr_t retained,unsigned *slot)
{
    if(!retained||!slot||b->faulted)return -EACCES;
    unsigned i=0;while(i<RB_LINK_MAX&&b->links[i].retained)i++;
    if(i==RB_LINK_MAX||b->high_id==0x7fffffffu)return -EAGAIN;
    struct rb_identity c={0};int e=b->authority.inspect(b->authority.context,retained,&c);
    if(e)return e;if(c.group_count>8||!c.process_generation)return -EACCES;
    e=b->authority.retain(b->authority.context,retained);if(e)return e;
    b->links[i]=(struct rb_link){.retained=retained,.id=++b->high_id,.identity=c};*slot=i;return 0;
}
int rb_bind_hive(struct rb_broker *b,const struct rb_identity *c)
{
    if(!b->win32||!c||!c->uid||!c->session||!memchr(c->profile,0,sizeof(c->profile))||
       strncmp(c->profile,"/Windows/Profiles/",18))return -EINVAL;
    for(unsigned i=0;i<RB_LINK_MAX;i++)if(b->links[i].retained&&b->links[i].win32&&
        (b->links[i].identity.uid!=c->uid||b->links[i].identity.session!=c->session))return -EBUSY;
    struct wr_server *s=b->win32;if(!s->secured)return -EACCES;
    if(b->storage.profile){int e=b->storage.profile(b->storage.context,c);if(e)return e;}
    char label[16];snprintf(label,sizeof(label),"u%08x",c->uid);
    uint32_t e=wr_select_profile(s,label,c->profile);if(e)return -EIO;
    if(!memchr(c->name,0,sizeof(c->name)))return -EINVAL;
    memcpy(s->user_name,c->name,sizeof(s->user_name));
    char sid[96];int n=snprintf(sid,sizeof(sid),"S-1-5-21-%u-%u-%u-%u",c->machine_sid[0],c->machine_sid[1],c->machine_sid[2],c->uid);
    if(n<0||(size_t)n>=sizeof(sid))return -EINVAL;
    struct wr_key *own=wr_child(s,s->roots[1],sid);
    if(!own){
        /* Only the broker's protected template seeds a newly provisioned hive.
         * Old USER.CRG paths are migration inputs, never selected here. */
        own=wr_child(s,s->roots[1],".DEFAULT");if(!own)return -EIO;
        char *name=strdup(sid);if(!name)return -ENOMEM;
        /* Recompute serialized-text accounting after the name change. */
        size_t old=strlen(own->name),depth=0;
        for(struct wr_key *k=s->keys;k;k=k->next){
            struct wr_key *p=k;while(p&&p!=own)p=p->parent;
            if(p)depth++;
        }
        free(own->name);own->name=name;s->hive[1].text+=depth*(strlen(sid)-old);
        s->hive[1].dirty=1;
    }
    strcpy(s->user_branch,sid);s->subject.uid=c->uid;s->subject.session=c->session;
    return wr_flush(s)?-EIO:0;
}
static int current(struct rb_broker *b,struct rb_link *l,struct rb_identity *c)
{
    int e=b->authority.inspect(b->authority.context,l->retained,c);if(e)return e;
    if(c->group_count>8||c->uid!=l->identity.uid||c->session!=l->identity.session||
       c->process_generation!=l->identity.process_generation)return -ESRCH;
    return 0;
}
static int tree_read(struct rb_broker *b,unsigned root,const struct rb_identity *c,char **text,size_t *n,struct rt **tree)
{
    int e=root==RB_USERS?(b->storage.users?b->storage.users(b->storage.context,c,text,n):-EOPNOTSUPP):
        b->storage.read(b->storage.context,root,text,n);
    if(e)return e;
    if(*n>RB_ROOT_MAX||!(*tree=reg_parse(root_name(root),*text,*n))){free(*text);*text=NULL;return -EIO;}
    return 0;
}
static int native(struct rb_broker *b,struct rb_link *l,const struct rb_identity *c,unsigned op,
                  const uint8_t *p,size_t n,uint8_t *out,size_t cap,size_t *used)
{
    if(n<32||rb_get32(p)!=1||!root_name(rb_get32(p+4))||rb_get64(p+24)||cap<32)return -EINVAL;
    unsigned root=rb_get32(p+4);uint32_t offset=rb_get32(p+8),length=rb_get32(p+12);uint64_t expected=rb_get64(p+16);
    if(n>RB_BODY_MAX||length>RB_ROOT_MAX)return -EINVAL;
    int mutation=op>=RB_NATIVE_BEGIN&&op<=RB_NATIVE_COMMIT;
    if(mutation&&(root==RB_USERS||!admin(c)))return -EACCES;
    if(op==RB_NATIVE_ABORT){if(n!=32||offset||length)return -EINVAL;discard(l);*used=0;return 0;}
    if(op==RB_NATIVE_BEGIN){
        if(n!=32||offset||!length||expected==UINT64_MAX)return -EINVAL;
        if(l->stage)return -EBUSY;
        l->stage=malloc(length);if(!l->stage)return -ENOMEM;
        l->total=length;l->root=root;l->expected=expected;*used=0;return 0;
    }
    if(op==RB_NATIVE_PART){
        if(!l->stage||root!=l->root||expected!=l->expected||offset!=l->received||
           n<=32||n-32>RB_PAGE||n-32>l->total-l->received||length!=l->total)return -EINVAL;
        memcpy(l->stage+l->received,p+32,n-32);l->received+=(uint32_t)(n-32);*used=0;return 0;
    }
    if(n!=32)return -EINVAL;
    if(op!=RB_NATIVE_READ&&op!=RB_NATIVE_PAGE&&op!=RB_NATIVE_COMMIT)return -EOPNOTSUPP;
    char *text=NULL;size_t size=0;struct rt *tree=NULL;
    int e=tree_read(b,root,c,&text,&size,&tree);if(e)return e;
    uint64_t generation=tree->generation;
    if(op==RB_NATIVE_COMMIT){
        if(!l->stage||root!=l->root||expected!=l->expected||offset||length!=l->total||l->received!=l->total)e=-EINVAL;
        else if(generation!=expected)e=-EAGAIN;
        else if(generation>=UINT64_MAX-1)e=-EOVERFLOW;
        else{
            struct rt *candidate=reg_parse(root_name(root),(const char *)l->stage,l->total);
            if(!candidate)e=-EINVAL;
            else if(candidate->generation!=generation+1)e=-EINVAL;
            else{
                /* Every key/value of the complete staged root was parsed before
                 * publication. Machine System/Programs have one fixed policy;
                 * Users never uses this writer. No caller pathname is opened. */
                struct rb_identity fresh={0};e=current(b,l,&fresh);
                if(!e&&!admin(&fresh))e=-EACCES;
                if(!e)e=b->storage.write(b->storage.context,root,(char *)l->stage,l->total,generation);
            }
            reg_tree_free(candidate);
        }
        discard(l);*used=0;
    }else if(op==RB_NATIVE_PAGE&&(generation!=expected||offset>size))e=-EAGAIN;
    else{
        memset(out,0,32);rb_put32(out,1);rb_put32(out+4,root);rb_put32(out+8,offset);
        rb_put32(out+12,(uint32_t)size);rb_put64(out+16,generation);*used=32;
        if(op==RB_NATIVE_PAGE){size_t take=size-offset;if(take>RB_PAGE)take=RB_PAGE;
            if(cap-32<take)e=-EINVAL;else{memcpy(out+32,text+offset,take);*used+=take;}}
    }
    reg_tree_free(tree);free(text);return e;
}
static int win32_dispatch(struct rb_broker *b,struct rb_link *l,const struct rb_identity *c,
                          const void *p,size_t n,void *out,size_t cap,size_t *used)
{
    if(!b->win32||!b->win32->secured)return -EACCES;
    if(c->session!=b->win32->subject.session||c->uid!=b->win32->subject.uid){
        int e=rb_bind_hive(b,c);if(e)return e;
    }
    l->win32=1;
    if(n<sizeof(struct w32r_request)||n>W32R_MAX)return -EINVAL;
    struct w32r_request request;memcpy(&request,p,sizeof(request));
    if(request.op>W32R_FLUSH&&request.op!=W32R_COMPAT)return -EOPNOTSUPP;
    b->win32->subject.group_count=c->group_count;memcpy(b->win32->subject.groups,c->groups,sizeof(c->groups));
    int e=wr_dispatch(b->win32,l->id,p,n,out,cap,used)?-EINVAL:0;
    struct rb_identity fresh={0};if(!e)e=current(b,l,&fresh);
    if(e){b->faulted=1;return e;}
    if(!e&&wr_flush(b->win32)){b->faulted=1;e=-EIO;}
    return e;
}
static int win32_stream(struct rb_broker *b,struct rb_link *l,const struct rb_identity *c,unsigned op,
                        const uint8_t *p,size_t n,uint8_t *out,size_t cap,size_t *used)
{
    if(op==RB_WIN32)return n>RB_BODY_MAX?-EINVAL:win32_dispatch(b,l,c,p,n,out,cap,used);
    if(n<32||n>RB_BODY_MAX||rb_get32(p)!=1||rb_get32(p+4)||rb_get64(p+16)||rb_get64(p+24))return -EINVAL;
    uint32_t at=rb_get32(p+8),total=rb_get32(p+12);
    if(op==RB_WIN32_BEGIN){
        if(n!=32||at||total<sizeof(struct w32r_request)+2||total>W32R_MAX)return -EINVAL;
        if(l->stage||l->reply)return -EBUSY;
        l->stage=malloc(total);if(!l->stage)return -ENOMEM;l->total=total;*used=0;return 0;
    }
    if(op==RB_WIN32_PART){
        if(!l->stage||l->root||at!=l->received||total!=l->total||n<=32||n-32>l->total-l->received)return -EINVAL;
        memcpy(l->stage+at,p+32,n-32);l->received+=(uint32_t)(n-32);*used=0;return 0;
    }
    if(op==RB_WIN32_COMMIT){
        if(n!=32||!l->stage||l->root||at||total!=l->total||l->received!=l->total||cap<32)return -EINVAL;
        l->reply=malloc(W32R_MAX);if(!l->reply)return -ENOMEM;
        size_t bytes=0;int e=win32_dispatch(b,l,c,l->stage,l->total,l->reply,W32R_MAX,&bytes);discard(l);
        if(e){free(l->reply);l->reply=NULL;return e;}
        l->reply_bytes=(uint32_t)bytes;memset(out,0,32);rb_put32(out,1);rb_put32(out+12,(uint32_t)bytes);*used=32;return 0;
    }
    if(op==RB_WIN32_READ){
        if(n!=32||!l->reply||total!=l->reply_bytes||at>=total||cap<32)return -EINVAL;
        size_t take=total-at;if(take>RB_PAGE)take=RB_PAGE;if(take>cap-32)return -EINVAL;
        memset(out,0,32);rb_put32(out,1);rb_put32(out+8,at);rb_put32(out+12,total);
        memcpy(out+32,l->reply+at,take);*used=32+take;
        if(at+take==total){free(l->reply);l->reply=NULL;l->reply_bytes=0;}return 0;
    }
    return -EOPNOTSUPP;
}
int rb_exchange(struct rb_broker *b,unsigned slot,unsigned op,const void *p,size_t n,void *out,size_t cap,size_t *used)
{
    if(!b||slot>=RB_LINK_MAX||!used||(!p&&n)||(!out&&cap)||!b->links[slot].retained)return -EINVAL;
    *used=0;if(b->faulted)return -EIO;if(b->busy)return -EAGAIN;
    b->busy=1;b->active_link=(int)slot;struct rb_link *l=b->links+slot;struct rb_identity c={0};int e=current(b,l,&c);
    if(!e&&op==RB_MACHINE_ID){
        if(n||cap<32)e=-EINVAL;
        else{memset(out,0,32);rb_put32(out,1);for(unsigned i=0;i<3;i++)rb_put32((uint8_t *)out+4+i*4,c.machine_sid[i]);
            rb_put64((uint8_t *)out+16,c.volume);rb_put64((uint8_t *)out+24,c.account_generation);*used=32;}
    }else if(!e&&op>=RB_CHECKER_LIST&&op<=RB_CHECKER_STATUS){
        /* sec-sysvc2: Registry Checker, as SYSTEM after the caller's check. */
        e=b->checker&&rb_checker_exchange?rb_checker_exchange(b->checker,&c,op,p,n,out,cap,used):-EOPNOTSUPP;
    }else if(!e)e=op>=RB_WIN32?win32_stream(b,l,&c,op,p,n,out,cap,used):native(b,l,&c,op,p,n,out,cap,used);
    if(e){*used=0;if(e==-ESRCH||e==-EACCES)discard(l);}
    b->active_link=-1;b->busy=0;return e;
}
