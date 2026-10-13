/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <ciuki/devices.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int number(struct rt *t, const char *k, const char *n, uint32_t v)
{
    int32_t value = (int32_t)v; return reg_put(t, k, n, REG_INT32, &value, sizeof(value));
}
static int string(struct rt *t, const char *k, const char *n, const char *v)
{
    return reg_put(t, k, n, REG_STRING, v, strlen(v) + 1);
}
static int device(struct rt *t, const char *key, struct ciuki_device_record *d, int resources)
{
    if (number(t, key, "vendor", d->vendor) || number(t, key, "device", d->device) || number(t, key, "class", d->pci_class) ||
        number(t, key, "subclass", d->subclass) || number(t, key, "prog-if", d->prog_if) || number(t, key, "state", d->state) ||
        string(t, key, "driver", d->driver) || string(t, key, "name", d->name) || string(t, key, "reason", d->reason) ||
        number(t, key, "resources", d->resource_count)) return -1;
    for (unsigned i = 0; resources && i < d->resource_count; i++) {
        char path[160], address[24]; struct ciuki_device_resource *r = &d->resources[i];
        snprintf(path, sizeof(path), "%s\\Resources\\%u", key, i);
        if (number(t, path, "type", r->type) || number(t, path, "flags", r->flags)) return -1;
        snprintf(address, sizeof(address), "0x%016llx", (unsigned long long)r->start); if (string(t, path, "start", address)) return -1;
        snprintf(address, sizeof(address), "0x%016llx", (unsigned long long)r->end); if (string(t, path, "end", address)) return -1;
    }
    return 0;
}
struct rt *reg_hardware(uint64_t previous)
{
    struct ciuki_devices_info *s = calloc(1, sizeof(*s)); if (!s) return NULL;
    s->version = CIUKI_DEVICES_VERSION; s->size = sizeof(*s);
    if (ciuki_devices(s)) { int e = errno; free(s); errno = e; return NULL; }
    if (s->count > CIUKI_DEVICES_MAX || s->version != CIUKI_DEVICES_VERSION || s->size != sizeof(*s)) { free(s); reg_fail(EIO); return NULL; }
    if (s->generation == previous) { free(s); errno = 0; return NULL; }
    struct rt *t = reg_tree_new("Hardware"); if (!t) { free(s); return NULL; }
    t->generation = s->generation;
    static const char *const paths[] = {"Hardware\\Display", "Hardware\\Audio", "Hardware\\Network"};
    static const unsigned classes[] = {CIUKI_DEVICE_DISPLAY, CIUKI_DEVICE_SOUND, CIUKI_DEVICE_NETWORK};
    unsigned counts[3] = {0};
    if (!reg_ensure(t, "Hardware\\PCI") || number(t, "Hardware", "truncated", !!(s->flags & CIUKI_DEVICES_TRUNCATED))) goto failed;
    for (unsigned i = 0; i < s->count; i++) {
        struct ciuki_device_record *d = &s->devices[i]; char key[96];
        if (!memchr(d->name, 0, sizeof(d->name)) || !memchr(d->driver, 0, sizeof(d->driver)) ||
            !memchr(d->reason, 0, sizeof(d->reason)) || d->resource_count > CIUKI_DEVICE_RESOURCES) { reg_fail(EIO); goto failed; }
        if (d->bus_type == CIUKI_BUS_PCI) {
            if (d->bus > 255 || d->slot > 31 || d->function > 7) { reg_fail(EIO); goto failed; }
            snprintf(key, sizeof(key), "Hardware\\PCI\\%02x:%02x.%x", (unsigned)d->bus, (unsigned)d->slot, (unsigned)d->function);
            if (device(t, key, d, 1)) goto failed;
        }
        for (unsigned j = 0; j < 3; j++) if (d->class_id == classes[j]) {
            snprintf(key, sizeof(key), "%s\\%u", paths[j], counts[j]++);
            /* PCI resources already have one authoritative branch. Avoid
             * duplicating up to 64 * 8 resources in the class summaries. */
            if (device(t, key, d, d->bus_type != CIUKI_BUS_PCI)) goto failed;
            if (d->bus_type == CIUKI_BUS_PCI) {
                char source[64]; snprintf(source, sizeof(source), "Hardware\\PCI\\%02x:%02x.%x", (unsigned)d->bus, (unsigned)d->slot, (unsigned)d->function);
                if (string(t, key, "pci", source)) goto failed;
            }
        }
    }
    for (unsigned j = 0; j < 3; j++) if (number(t, paths[j], "count", counts[j])) goto failed;
    free(s); return t;
failed: { int e = errno; reg_tree_free(t); free(s); errno = e; return NULL; }
}
