/* Shared VFS/native resolved identity adapter for k03's actual operations.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/security.h>
#include "security_policy.h"
int vfs_resolve_subject_locked(struct vfs_table *t,const struct security_subject *subject,const char *path,
    bool win32,bool missing,struct px_path *out)
{
    if(!t || !subject || !path || !out) return -EACCES;
    struct px_namespace *s=files_namespace_locked(t->vfs); if(!s) return -EIO;
    unsigned drive=t->drive;
    if(path[0] && path[1]==':') { drive=(unsigned)(path_upper((uint8_t)path[0])-'A'); path+=2; }
    if(drive>=26 || !t->vfs->policy[drive]) return -EACCES;
    /* Resolve the actual cwd before relative components. Never pre-normalize
     * input strings: that would erase an unauthorized intermediate directory. */
    struct px_node *start=s->root;
    if(path[0]!='/' && path[0]!='\\') {
        struct px_path cwd;
        int e=px_resolve_volume_subject_locked(s,drive,s->root,t->cwd[drive],false,false,subject,&cwd);
        if(e) return e;
        uint32_t cluster=t->cwd_cluster[drive];
        if(!cluster && cwd.node->volume && cwd.node->volume->type==32) cluster=cwd.node->volume->root;
        if(cwd.node->volume && cwd.node->entry.first!=cluster) return -EIO;
        start=cwd.node;
    }
    return px_resolve_volume_subject_locked(s,drive,start,path,win32,missing,subject,out);
}
int policy_reference_get_locked(struct px_node *node,struct security_subject *subject,unsigned access,struct policy_reference *out)
{
    if(!out || !subject) return -EACCES;
    int e=px_policy_check_locked(node,subject,access); if(e) return e;
    struct policy_label label; e=px_policy_label_locked(node,&label); if(e) return e;
    e=security_subject_get(subject); if(e) return e;
    px_retain(node); *out=(struct policy_reference){label,node,subject,access,node->generation}; return 0;
}
int policy_reference_check_locked(struct policy_reference *r,unsigned access)
{
    if(!r || !r->node || !r->subject || !access || (access&~r->access)) return -EACCES;
    struct px_node *n=r->node;
    if(n->ino!=r->label.object || n->generation!=r->drive_generation || !n->linked ||
       n->drive>=26 || n->space->vfs->generation[n->drive]!=r->drive_generation) return -EIO;
    int e=px_policy_check_locked(n,r->subject,access);
    if(!e) e=px_policy_label_locked(n,&r->label);
    return e;
}
void policy_reference_put_locked(struct policy_reference *r)
{
    if(!r) return;
    px_release(r->node); security_subject_put(r->subject); memset(r,0,sizeof(*r));
}
