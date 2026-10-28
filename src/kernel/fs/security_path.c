/* Shared VFS/native resolved identity adapter for k03's actual operations.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/security.h>
#include "security_policy.h"
/* vfs_resolve_subject_locked lives in proc/posixpath.c (sec-k03): every
 * harness and kernel object that links the native resolver also gets the
 * legacy-table adapter, so a subject table never silently lacks it. */
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
