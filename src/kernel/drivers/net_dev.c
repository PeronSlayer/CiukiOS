/* Public read-only network observations; no register or configuration access.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/net.h>
#include <ciuki/setup_mode.h>
#pragma weak setup_mode
#pragma weak net_count
#pragma weak net_get
#include "../ndis/native.h"
#pragma weak ndis_qualified_package
#pragma weak ndis_package_busy

static int driver_pack_query(uint32_t address,uint32_t bytes)
{
    if(bytes!=sizeof(struct ciuki_driver_pack_query)) return -EINVAL;
    struct ciuki_driver_pack_query q;
    int err=copy_from_user(&q,address,sizeof(q)); if(err) return err;
    if(q.version!=CIUKI_DRIVER_PACK_VERSION) return -EOPNOTSUPP;
    if(q.size!=sizeof(q) || q.qualified || q.busy || q.reserved[0] || q.reserved[1] ||
       q.bus>255 || q.slot>31 || q.function>7 || q.vendor>65535 || q.device>65535 ||
       q.sub_vendor>65535 || q.sub_device>65535 || q.revision>255) return -EINVAL;
    for(unsigned i=0;i<pci_count();i++) {
        const struct pci_func *p=pci_get(i);
        if(!p || p->class_code!=2 || p->subclass!=0 || p->bus!=q.bus || p->dev!=q.slot || p->fn!=q.function ||
           p->vendor!=q.vendor || p->device!=q.device || p->sub_vendor!=q.sub_vendor || p->sub_device!=q.sub_device || p->revision!=q.revision) continue;
        q.qualified=ndis_qualified_package && ndis_qualified_package(q.binary_hash,p,q.package_seal);
        q.busy=ndis_package_busy && ndis_package_busy(p);
        break;
    }
    return copy_to_user(address,&q,sizeof(q));
}

int net_devctl(struct process *p, int32_t fd, uint32_t op, uint32_t address, uint32_t bytes)
{
    if (!file_fd_object(p, fd)) return -EBADF;
    struct file_description *d = file_fd(p, fd);
    if (!d || d->node->kind != PX_NET) return -ENOTTY;
    if (op==CIUKI_SETUP_NET_QUERY) {
        if (!p->setup_capability || !setup_mode || !setup_mode()) return -EPERM;
        if (bytes!=sizeof(struct ciuki_setup_net)) return -EINVAL;
        struct ciuki_setup_net q;
        int e=copy_from_user(&q,address,sizeof(q)); if(e)return e;
        if(q.version!=CIUKI_SETUP_FAT_VERSION || q.size!=sizeof(q) || q.reserved) return -EINVAL;
        q=(struct ciuki_setup_net){.version=CIUKI_SETUP_FAT_VERSION,.size=sizeof(q)};
        q.count=net_count ? net_count() : 0;
        if(q.count>2 || (q.count && !net_get))return -EOVERFLOW;
        for(unsigned i=0;i<q.count;i++) {
            struct netif *n=net_get(i); if(!n)return -EIO;
            /* PCI identity and station MAC are immutable after discovery. */
            q.identity[i].vendor=n->pci.vendor; q.identity[i].device=n->pci.device;
            memcpy(q.identity[i].mac,n->address,6);
        }
        return copy_to_user(address,&q,sizeof(q));
    }
    if (op == NET_QUERY_DRIVER_PACK) return driver_pack_query(address,bytes);
    if (op != NET_GET_STATE) return -ENOTTY;
    if (bytes != sizeof(struct ciuki_net_state)) return -EINVAL;
    struct ciuki_net_state state;
    int err = copy_from_user(&state, address, sizeof(state));
    if (err) return err;
    if (state.version != CIUKI_NET_VERSION) return -EOPNOTSUPP;
    if (state.size != sizeof(state) || state.reserved0[0] || state.reserved0[1] || state.reserved1) return -EINVAL;
    net_state_snapshot(&state);
    return copy_to_user(address, &state, sizeof(state));
}
