/* SY-1 System Information tree and SY-3 Resource Meter. SPDX-License-Identifier: MIT */
#include "systools.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int add(struct si_tree *t, int parent, unsigned kind, const char *label, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static int add(struct si_tree *t, int parent, unsigned kind, const char *label, const char *fmt, ...)
{
    if (t->count >= SI_NODES) { t->overflow = 1; return -1; }
    struct si_node *n = &t->n[t->count];
    memset(n, 0, sizeof(*n));
    n->parent = parent; n->kind = kind; n->depth = parent < 0 ? 0 : t->n[parent].depth + 1;
    snprintf(n->label, sizeof(n->label), "%s", label);
    if (fmt) { va_list ap; va_start(ap, fmt); vsnprintf(n->value, sizeof(n->value), fmt, ap); va_end(ap); }
    return (int)t->count++;
}
static const char *const class_name[CIUKI_DEVICE_CLASSES] = {
    "System devices", "Display", "Keyboard", "Mouse", "Disk drives", "CD-ROM", "Sound", "Network", "Ports" };
static const char *state_text(unsigned s)
{
    return s == CIUKI_DEVICE_READY ? "Working properly" : s == CIUKI_DEVICE_ABSENT ? "Not present" :
           s == CIUKI_DEVICE_QUARANTINED ? "Stopped (problem)" : "Using firmware settings";
}
static int overlap(const struct ciuki_device_resource *a, const struct ciuki_device_resource *b)
{
    if (a->type != b->type) return 0;
    if (a->type == CIUKI_RESOURCE_IRQ || a->type == CIUKI_RESOURCE_DMA) return a->start == b->start;
    uint64_t ae = a->end > a->start ? a->end : a->start + 1, be = b->end > b->start ? b->end : b->start + 1;
    return a->start < be && b->start < ae;
}
static const char *res_label(unsigned type)
{ return type == CIUKI_RESOURCE_IO ? "I/O" : type == CIUKI_RESOURCE_IRQ ? "IRQs" : type == CIUKI_RESOURCE_DMA ? "DMA" : "Memory"; }

void si_build(struct si_tree *t, const struct si_input *in)
{
    memset(t, 0, sizeof(*t));
    const struct ciuki_devices_info *d = in->devices;
    unsigned count = d ? (d->count > CIUKI_DEVICES_MAX ? CIUKI_DEVICES_MAX : d->count) : 0;
    int sys = add(t, -1, SI_CATEGORY, "System Information", NULL);
    add(t, sys, SI_ITEM, "System", "%s", in->os_build[0] ? in->os_build : "CiukiOS");
    if (d) {
        add(t, sys, SI_ITEM, "Processor", "%s", d->cpu_brand[0] ? d->cpu_brand : d->cpu_vendor);
        add(t, sys, SI_ITEM, "CPU family/model/stepping", "%u/%u/%u", d->cpu_family, d->cpu_model, d->cpu_stepping);
    }
    add(t, sys, SI_ITEM, "Memory", "%llu KB RAM, %llu KB free", (unsigned long long)in->memory_total_kib,
        (unsigned long long)in->memory_free_kib);
    add(t, sys, SI_ITEM, "System resources", "%u%% free", in->gdi_free_pct < in->user_free_pct ? in->gdi_free_pct : in->user_free_pct);
    int hw = add(t, -1, SI_CATEGORY, "Hardware Resources", NULL);
    /* Conflicts/Sharing: overlapping I/O or memory, identical DMA; IRQ sharing is listed, not a conflict. */
    int conflicts = add(t, hw, SI_CATEGORY, "Conflicts/Sharing", NULL);
    unsigned n_conf = 0, n_share = 0;
    for (unsigned i = 0; i < count; i++)
        for (unsigned j = i + 1; j < count; j++)
            for (unsigned a = 0; a < d->devices[i].resource_count && a < CIUKI_DEVICE_RESOURCES; a++)
                for (unsigned b = 0; b < d->devices[j].resource_count && b < CIUKI_DEVICE_RESOURCES; b++)
                    if (overlap(&d->devices[i].resources[a], &d->devices[j].resources[b])) {
                        char what[SI_LABEL];
                        snprintf(what, sizeof(what), "%s %llx", res_label(d->devices[i].resources[a].type),
                                 (unsigned long long)d->devices[i].resources[a].start);
                        if (d->devices[i].resources[a].type == CIUKI_RESOURCE_IRQ) {
                            n_share++;
                            add(t, conflicts, SI_ITEM, what, "IRQ shared: %s / %s", d->devices[i].name, d->devices[j].name);
                        } else {
                            n_conf++;
                            add(t, conflicts, SI_ITEM, what, "Conflict: %s / %s", d->devices[i].name, d->devices[j].name);
                        }
                    }
    t->conflicts = n_conf;
    if (!n_conf && !n_share) snprintf(t->n[conflicts].value, SI_VALUE, "No conflicts");
    else snprintf(t->n[conflicts].value, SI_VALUE, "%u conflict(s), %u shared IRQ(s)", n_conf, n_share);
    for (unsigned type = CIUKI_RESOURCE_IO; type <= CIUKI_RESOURCE_MEMORY; type++) {
        static const unsigned order[] = { CIUKI_RESOURCE_DMA, CIUKI_RESOURCE_IO, CIUKI_RESOURCE_IRQ, CIUKI_RESOURCE_MEMORY };
        unsigned ty = order[type];
        int cat = add(t, hw, SI_CATEGORY, ty == CIUKI_RESOURCE_IO ? "I/O" : ty == CIUKI_RESOURCE_IRQ ? "IRQs" :
                      ty == CIUKI_RESOURCE_DMA ? "DMA" : "Memory", NULL);
        for (unsigned i = 0; i < count; i++)
            for (unsigned r = 0; r < d->devices[i].resource_count && r < CIUKI_DEVICE_RESOURCES; r++) {
                const struct ciuki_device_resource *x = &d->devices[i].resources[r];
                if (x->type != ty) continue;
                char range[SI_LABEL];
                if (ty == CIUKI_RESOURCE_IRQ || ty == CIUKI_RESOURCE_DMA) snprintf(range, sizeof(range), "%llu", (unsigned long long)x->start);
                else if (x->flags & CIUKI_RESOURCE_UNKNOWN_SIZE) snprintf(range, sizeof(range), "%08llX", (unsigned long long)x->start);
                else snprintf(range, sizeof(range), "%08llX-%08llX", (unsigned long long)x->start, (unsigned long long)(x->end - 1));
                add(t, cat, SI_ITEM, range, "%s", d->devices[i].name);
            }
    }
    int comp = add(t, -1, SI_CATEGORY, "Components", NULL);
    for (unsigned c = 0; c < CIUKI_DEVICE_CLASSES; c++) {
        int cat = -1;
        for (unsigned i = 0; i < count; i++) {
            if (d->devices[i].class_id != c) continue;
            if (cat < 0) cat = add(t, comp, SI_CATEGORY, class_name[c], NULL);
            add(t, cat, SI_ITEM, d->devices[i].name, "%s; driver %s", state_text(d->devices[i].state),
                d->devices[i].driver[0] ? d->devices[i].driver : "none");
        }
    }
    int prob = add(t, comp, SI_CATEGORY, "Problem Devices", NULL);
    unsigned problems = 0;
    for (unsigned i = 0; i < count; i++)
        if (d->devices[i].state == CIUKI_DEVICE_QUARANTINED) {
            problems++;
            add(t, prob, SI_ITEM, d->devices[i].name, "%s", d->devices[i].reason[0] ? d->devices[i].reason : state_text(d->devices[i].state));
        }
    snprintf(t->n[prob].value, SI_VALUE, problems ? "%u device(s)" : "No problems detected", problems);
    int soft = add(t, -1, SI_CATEGORY, "Software Environment", NULL);
    int drv = add(t, soft, SI_CATEGORY, "System Drivers", NULL);
    if (d)
        for (unsigned i = 0; i < d->driver_count && i < CIUKI_DRIVERS_MAX; i++)
            add(t, drv, SI_ITEM, d->drivers[i].name, "build %s; %u device(s)", d->drivers[i].build, d->drivers[i].bound_devices);
    add(t, soft, SI_ITEM, "Running Tasks", "%u", in->processes);
    int tools = add(t, -1, SI_CATEGORY, "Tools", NULL);
    add(t, tools, SI_ITEM, "Device Manager", "Control Panel > System");
    add(t, tools, SI_ITEM, "System Monitor", "Accessories > System Tools");
    add(t, tools, SI_ITEM, "Resource Meter", "Accessories > System Tools");
    add(t, tools, SI_ITEM, "Hardware Report", "Accessories > System Tools");
}
int si_find(const struct si_tree *t, const char *label)
{
    for (unsigned i = 0; i < t->count; i++) if (!strcmp(t->n[i].label, label)) return (int)i;
    return -1;
}
unsigned si_children(const struct si_tree *t, int parent)
{
    unsigned c = 0;
    for (unsigned i = 0; i < t->count; i++) if (t->n[i].parent == parent) c++;
    return c;
}
size_t si_export(const struct si_tree *t, char *out, size_t cap)
{
    size_t at = 0;
    for (unsigned i = 0; i < t->count; i++) {
        const struct si_node *n = &t->n[i];
        int w = snprintf(out + at, cap - at, "%*s%s%s%s\r\n", (int)n->depth * 2, "", n->label, n->value[0] ? "\t" : "", n->value);
        if (w < 0 || (size_t)w >= cap - at) { if (cap) out[0] = 0; return 0; }
        at += (size_t)w;
    }
    return at;
}

