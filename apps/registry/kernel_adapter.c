/* Same accepted-descriptor lease mechanism as sec-int-01 ACCOUNT, without
 * sharing its role registration, fd table or request state. SPDX-License-Identifier: MIT */
#include "kernel_adapter.h"
#include "../accounts/kernel_sys.h"
#include <errno.h>
#include <string.h>
static int call(unsigned op,struct ciuki_security_args *a,const void *in,void *out,uint32_t bytes)
{a->version=1;a->size=64;a->operation=op;return acct_sys_security(a,in,in?64:0,out,bytes);}
static int role(void *p)
{
    (void)p;struct ciuki_credential c;struct ciuki_security_args a={0};
    int e=call(CIUKI_SECURITY_QUERY_SELF,&a,NULL,&c,64);if(e)return e;
    if(c.uid||c.session_id)return -EPERM;
    struct ciuki_security_link in={.version=1,.size=64,.service=4},out={0};memset(&a,0,sizeof(a));
    e=call(CIUKI_SECURITY_SERVICE_CONNECT,&a,&in,&out,64);if(e)return e;
    int own=out.peer_pid==acct_sys_pid()&&out.peer_role==CIUKI_SECURITY_ROLE_REGISTRY&&out.server_role==CIUKI_SECURITY_ROLE_REGISTRY;
    acct_sys_close(out.endpoint_fd);return own?0:-EPERM;
}
static struct rb_kernel_link *link(void *p,uintptr_t lease)
{
    struct rb_kernel *k=p;unsigned slot=(unsigned)(lease&15u);uint32_t serial=(uint32_t)(lease>>4);
    if((uint64_t)lease>>32||!slot||slot>RB_LINK_MAX||!serial)return NULL;
    struct rb_kernel_link *l=k->links+slot-1;
    return l->refs&&l->serial==serial?l:NULL;
}
static int peer(struct rb_kernel_link *l,struct ciuki_credential *out)
{
    if(!l||l->retiring)return -ESRCH;
    struct ciuki_security_args a={.endpoint_fd=l->wire.endpoint_fd};int e=call(CIUKI_SECURITY_QUERY_PEER,&a,NULL,out,64);if(e)return e;
    if(a.result!=l->wire.endpoint_generation||memcmp(out,&l->credential,64))return -ESRCH;
    return 0;
}
static int retain(void *p,uintptr_t lease)
{struct rb_kernel_link *l=link(p,lease);struct ciuki_credential c;int e=peer(l,&c);if(e)return e;if(l->refs==UINT32_MAX)return -EAGAIN;l->refs++;return 0;}
static void release(void *p,uintptr_t lease)
{struct rb_kernel_link *l=link(p,lease);if(!l)return;if(!--l->refs){acct_sys_close(l->wire.endpoint_fd);memset(l,0,sizeof(*l));}}
static int inspect(void *p,uintptr_t lease,struct rb_identity *out)
{
    struct rb_kernel *k=p;struct rb_kernel_link *l=link(p,lease);struct ciuki_credential c;int e=peer(l,&c);if(e)return e;
    struct rb_identity current={0};int enabled=0;
    if(!k->account)return -EIO;e=k->account(k->context,c.uid,&current,&enabled);if(e)return e;
    if(!enabled||current.uid!=c.uid)return -EACCES;
    *out=current;out->session=c.session_id;out->group_count=c.group_count;memcpy(out->groups,c.groups,sizeof(c.groups));
    out->role=l->wire.peer_role;out->process_generation=l->wire.peer_process_generation;
    return peer(l,&c); /* account query may yield; lease must still name this peer */
}
int rb_kernel_init(struct rb_kernel *k,struct rb_authority *a)
{
    if(!k||!a)return -EINVAL;memset(k,0,sizeof(*k));
    *a=(struct rb_authority){k,role,retain,release,inspect};return 0;
}
int rb_kernel_accept(struct rb_kernel *k,uintptr_t *lease,int *fd,uint32_t *generation)
{
    unsigned i=0;while(i<RB_LINK_MAX&&k->links[i].refs)i++;
    if(i==RB_LINK_MAX||k->serial==0x0fffffffu)return -EAGAIN;
    struct ciuki_security_link in={.version=1,.size=64,.service=4},out={0};struct ciuki_security_args a={0};
    int e=call(CIUKI_SECURITY_SERVICE_ACCEPT,&a,&in,&out,64);if(e)return e;
    struct ciuki_credential c;memset(&a,0,sizeof(a));a.endpoint_fd=out.endpoint_fd;
    e=call(CIUKI_SECURITY_QUERY_PEER,&a,NULL,&c,64);
    if(e||a.result!=out.endpoint_generation||out.server_role!=CIUKI_SECURITY_ROLE_REGISTRY){acct_sys_close(out.endpoint_fd);return e?e:-EACCES;}
    k->links[i]=(struct rb_kernel_link){.wire=out,.credential=c,.serial=++k->serial,.refs=1};
    *lease=((uintptr_t)k->serial<<4)|(i+1);*fd=out.endpoint_fd;*generation=out.endpoint_generation;return 0;
}
void rb_kernel_retire(struct rb_kernel *k,uintptr_t lease){struct rb_kernel_link *l=link(k,lease);if(l){l->retiring=1;release(k,lease);}}
int rb_kernel_live(struct rb_kernel *k,uintptr_t lease){struct ciuki_credential c;return peer(link(k,lease),&c);}
