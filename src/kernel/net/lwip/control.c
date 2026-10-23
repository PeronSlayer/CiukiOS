/* Serialized settings transactions outside the TCP/IP core.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/sync.h>
#include <ciuki/net_control.h>
#include "lwip/sys.h"
extern int net_control_driver(unsigned, struct ciuki_net_control *, bool) __attribute__((weak));
extern int net_settings_save(unsigned, const struct ciuki_net_config *) __attribute__((weak));
extern void net_settings_get(unsigned, struct ciuki_net_config *) __attribute__((weak));
static sys_mutex_t control_mutex;
static bool control_ready;
static const char *config_ip(uint32_t address, char out[16])
{
    const uint8_t *b = (const void *)&address;
    ksnprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]); return out;
}
int net_control_init(void)
{
    control_ready = sys_mutex_new(&control_mutex) == ERR_OK;
    /* fx-5 follow-up: held across net_settings_get (settings lock) and
     * net_control_driver (netif mutex), objects in other files: an outer
     * level, never their link addresses (sync.h). */
    if (control_ready) ciuki_lwip_mutex_class(&control_mutex, "net.control", KMUTEX_LEVEL_OUTER);
    return control_ready ? 0 : -ENOMEM;
}
int net_control(unsigned operation, struct ciuki_net_control *record)
{
    if (operation > CIUKI_NETCTL_RELEASE || record->index >= 2) return -EINVAL;
    if (operation == CIUKI_NETCTL_SET && !net_config_valid(&record->config)) return -EINVAL;
    if (!control_ready) {
        if (operation != CIUKI_NETCTL_GET) return -ENETDOWN;
        net_config_default(&record->config);
        if (net_settings_get) net_settings_get(record->index, &record->config);
        return net_control_driver ? net_control_driver(record->index, record, false) : -ENODEV;
    }
    sys_mutex_lock(&control_mutex);
    struct ciuki_net_control old = {.index = record->index};
    int e = net_stack_config(record->index, CIUKI_NETCTL_GET, &old);
    if (e == -ENODEV && operation == CIUKI_NETCTL_GET && net_control_driver) {
        net_config_default(&record->config);
        if (net_settings_get) net_settings_get(record->index, &record->config);
        e = net_control_driver(record->index, record, false);
        sys_mutex_unlock(&control_mutex); return e;
    }
    if (!e && net_control_driver) e = net_control_driver(record->index, &old, false);
    if (!e && operation != CIUKI_NETCTL_GET && record->generation != old.generation) e = -EBUSY;
    bool driver_applied = false;
    if (!e && operation == CIUKI_NETCTL_SET) {
        if (net_control_driver) { e = net_control_driver(record->index, record, true); driver_applied = !e; }
        else if (record->config.speed) e = -EOPNOTSUPP;
    }
    if (!e) e = net_stack_config(record->index, operation, record);
    if (e && driver_applied) {
        int rollback = net_control_driver(record->index, &old, true);
        if (rollback) klog("[net] config iface=en%u error=%d rollback=driver_failed", record->index, rollback);
    }
    if (!e && operation == CIUKI_NETCTL_SET) {
        e = net_settings_save ? net_settings_save(record->index, &record->config) : -ENODEV;
        if (e) {
            int driver = net_control_driver ? net_control_driver(record->index, &old, true) : 0;
            int stack = net_stack_config(record->index, CIUKI_NETCTL_SET, &old);
            if (driver || stack) klog("[net] config iface=en%u error=%d rollback=failed driver=%d stack=%d", record->index, e, driver, stack);
        }
    }
    if (!e && net_control_driver) e = net_control_driver(record->index, record, false);
    if (!e && operation != CIUKI_NETCTL_GET) {
        char ip[16], mask[16], gateway[16];
        klog("[net] config iface=en%u mode=%s enabled=%u action=%u ip=%s mask=%s gw=%s speed=%u duplex=%u", record->index,
             record->config.dhcp ? "dhcp" : "static", record->config.enabled, operation,
             config_ip(record->address, ip), config_ip(record->mask, mask), config_ip(record->gateway, gateway),
             record->config.speed, record->config.duplex);
    }
    sys_mutex_unlock(&control_mutex); return e;
}
