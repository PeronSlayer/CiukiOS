/* Production primary-port provisioning, actual NIC interface/tap, identity
 * persistence and netif selection replaced only at their external seams.
 * SPDX-License-Identifier: GPL-2.0-only */
#define PORT_TEST_MAIN unused_port_suite_main
#include "port_test.c"
#include <ciuki/storage.h>
#include "../../../src/kernel/net/bridge_runtime.c"
static struct storage store;
static int chosen = -ENODEV, table_error;
static unsigned tables, destroyed, random_calls, publications, mode_calls;
static int persisted = -ENOENT, publication_error;
static uint8_t saved_identity[16];
static bool change_generation, reenter;
int net_stack_primary(void) { return chosen; }
int net_stack_random_id(uint8_t id[16])
{ random_calls++; memset(id, 0x73, 16); return 0; }
struct storage *storage_get(void) { return &store; }
int vfs_table_init(struct vfs *vfs, struct vfs_table *t, unsigned drive)
{
    CHECK(vfs == &store.vfs && t == &identity_table && drive == 2);
    if (table_error) return table_error;
    tables++; return 0;
}
void vfs_table_destroy(struct vfs_table *t)
{ CHECK(t == &identity_table); destroyed++; }
int net_bridge_identity_read_file(void *arg, uint8_t id[16])
{
    CHECK(arg == &identity_table);
    if (reenter) {
        struct net_bridge_port *out = (void *)1;
        CHECK(net_bridge_primary_port(&out) == -EBUSY && !out);
    }
    if (!persisted) memcpy(id, saved_identity, 16);
    return persisted;
}
int net_bridge_identity_write_file(void *arg, const uint8_t id[16])
{
    CHECK(arg == &identity_table); publications++;
    if (publication_error) return publication_error;
    CHECK(persisted == -ENOENT); memcpy(saved_identity, id, 16); persisted = 0;
    if (change_generation) devices[chosen].generation++;
    return 0;
}
static int primary_mode(struct netif *d, bool on)
{ CHECK(d == &devices[1]); CHECK(persisted == 0); mode_calls++; (void)on; return 0; }
static const struct netif_driver primary_driver = {.promiscuous = primary_mode};
static const struct netif_driver unsupported_driver = {0};
int main(void)
{
    struct net_bridge_port *out = (void *)1;
    CHECK(net_bridge_primary_port(NULL) == -EINVAL);
    CHECK(net_bridge_primary_port(&out) == -ENODEV && !out && !tables);
    device_count = 2;
    for (unsigned i = 0; i < 2; i++) {
        devices[i].driver = &primary_driver; devices[i].generation = i + 55;
        devices[i].ready = devices[i].running = true; devices[i].link = NET_LINK_UP;
        devices[i].address[0] = 2; devices[i].address[5] = i + 1;
    }
    chosen = 1; /* NIC zero runs too; it is NOT the stack's selected primary. */
    CHECK(net_bridge_primary_port(&out) == -EIO && !out);
    store.ready = true; devices[1].driver = &unsupported_driver;
    CHECK(net_bridge_primary_port(&out) == -ENOSYS && !tables);
    devices[1].driver = &primary_driver; table_error = -ENOMEM;
    CHECK(net_bridge_primary_port(&out) == -ENOMEM && !tables);
    table_error = 0; persisted = -EINVAL;
    CHECK(net_bridge_primary_port(&out) == -EINVAL && !out && !random_calls && !primary.registered);
    persisted = -ENOENT; publication_error = -EIO;
    CHECK(net_bridge_primary_port(&out) == -EIO && !out && random_calls == 1 && !primary.registered);
    publication_error = 0; change_generation = true;
    CHECK(net_bridge_primary_port(&out) == -ENODEV && !out && !primary.registered);
    change_generation = false; reenter = true;
    CHECK(!net_bridge_primary_port(&out) && out == &primary && primary.device == &devices[1]);
    CHECK(primary.generation == devices[1].generation && primary.bridge.interface == 1);
    CHECK(primary.bridge.physical_count == 2 && random_calls == 2 && publications == 2);
    CHECK(tables == destroyed && !mode_calls && !devices[0].promiscuous);
    struct bridge_key a = {1, 10}, b = {2, 20}; uint8_t ma[6], mb[6];
    CHECK(!net_bridge_enable(&out->bridge, a, 0, ma));
    CHECK(!net_bridge_enable(&out->bridge, b, 0, mb));
    CHECK(mode_calls == 1 && devices[1].promiscuous && !devices[0].promiscuous);
    CHECK(memcmp(ma, mb, 6) && memcmp(ma, devices[0].address, 6) && memcmp(mb, devices[1].address, 6));
    unsigned previous_tables = tables;
    CHECK(!net_bridge_primary_port(&out) && tables == previous_tables);
    devices[1].generation++; devices[1].promiscuous = false; /* driver replacement */
    CHECK(net_bridge_primary_port(&out) == -EBUSY && !out);
    net_bridge_port_carrier(&primary); CHECK(!primary.bridge.link);
    net_bridge_disable(&primary.bridge, a); net_bridge_disable(&primary.bridge, b);
    CHECK(!primary.bridge.promiscuous && mode_calls == 1); /* no stale hardware write */
    CHECK(!net_bridge_primary_port(&out) && out->generation == devices[1].generation);
    CHECK(random_calls == 2 && publications == 2 && tables == destroyed);
    CHECK(!net_bridge_enable(&out->bridge, a, 0, ma));
    net_bridge_disable(&out->bridge, a);
    CHECK(mode_calls == 3 && !devices[1].promiscuous);
    CHECK(!net_bridge_port_close(out) && !packets.used);
    devices[1].running = false;
    CHECK(net_bridge_primary_port(&out) == -ENODEV && !out); /* no NIC zero fallback */
    printf("primary port: %u checks passed\n", checks); return 0;
}
