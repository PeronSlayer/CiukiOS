/* Bind DOS LAN to the actual f6-10 default physical interface, on demand.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/net_bridge_port.h>
#include <ciuki/net_stack.h>
#include <ciuki/storage.h>
#include <ciuki/cpu.h>

static struct net_bridge_port primary;
static struct bridge_pool packets; /* references only, no frame payloads */
static struct vfs_table identity_table;
static bool provisioning;

static int primary_receive_mode(struct netif *device, bool on)
{ return netif_promiscuous(device, primary.generation, on); }
static int runtime_random(void *unused, uint8_t id[16])
{ (void)unused; return net_stack_random_id(id); }

int net_bridge_primary_port(struct net_bridge_port **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    uint32_t flags = irq_save(); bool busy = provisioning;
    if (!busy) provisioning = true;
    irq_restore(flags);
    if (busy) return -EBUSY;
    int index = net_stack_primary();
    struct netif *device = index >= 0 ? net_get((unsigned)index) : NULL;
    int error = -ENODEV;
    if (!device || !device->ready || !device->running || device->stopping || device->quarantined) goto done;
    if (primary.registered) {
        if (primary.device == device && primary.generation == device->generation) {
            *out = &primary; error = 0; goto done;
        }
        /* An active VM keeps its lease and drains against the old generation.
         * A later launch may reopen only after every VM/reference retired. */
        if (primary.bridge.count || primary.bridge.promiscuous) { error = -EBUSY; goto done; }
        error = net_bridge_port_close(&primary);
        if (error) goto done;
    }
    /* A failed close/receive-mode revocation cannot be overwritten. */
    if (primary.bridge.promiscuous || primary.bridge.count) { error = -EBUSY; goto done; }
    if (!device->driver || !device->driver->promiscuous) { error = -ENOSYS; goto done; }
    struct storage *s = storage_get();
    if (!s || !s->ready || s->stopped) { error = -EIO; goto done; }
    error = vfs_table_init(&s->vfs, &identity_table, 2);
    if (error) goto done;
    gen_t generation = device->generation;
    uint8_t id[16], physical[NETIF_MAX][6]; unsigned count = 0;
    struct bridge_identity_ops ops = {&identity_table, net_bridge_identity_read_file,
        net_bridge_identity_write_file, runtime_random};
    error = net_bridge_identity_load(&ops, id);
    vfs_table_destroy(&identity_table);
    if (error) goto done;
    if (device->generation != generation || net_stack_primary() != index) { error = -ENODEV; goto done; }
    for (unsigned i = 0; i < net_count() && i < NETIF_MAX; i++) {
        struct netif *d = net_get(i);
        if (d) memcpy(physical[count++], d->address, 6);
    }
    error = net_bridge_port_open(&primary, &packets, device, (unsigned)index, id,
                                  physical, count, primary_receive_mode);
    if (!error) *out = &primary;
done:
    flags = irq_save(); provisioning = false; irq_restore(flags);
    return error;
}
