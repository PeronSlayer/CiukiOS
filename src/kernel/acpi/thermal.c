/* ACPI 6.5 chapter 11 thermal admission and evaluation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "runtime.h"
#include "acclib.h"
static struct zone { ACPI_HANDLE handle; uint32_t previous; } zones[CIUKI_POWER_ZONES];
static struct cooling { ACPI_HANDLE handle; bool resource, wanted, on; } cooling[FW_MAX];
static unsigned targets;
static ACPI_HANDLE fans[FW_MAX];
static unsigned fan_count, inspected;
static uint64_t demand[CIUKI_POWER_ZONES][10];
static bool temperature(uint32_t v) { return v > 0 && v <= 10000; }
void fw_thermal_reset(void) { memset(zones, 0, sizeof(zones)); targets = fan_count = 0; memset(cooling, 0, sizeof(cooling)); }
static ACPI_STATUS target(ACPI_HANDLE h, uint64_t *mask)
{
    ACPI_OBJECT_TYPE type;
    ACPI_STATUS s = AcpiGetType(h, &type); if (s) return s;
    bool resource = type == ACPI_TYPE_POWER;
    if (!resource && type != ACPI_TYPE_DEVICE) return AE_TYPE;
    if (!fw_has(h, resource ? "_ON" : "_PS0") || !fw_has(h, resource ? "_OFF" : "_PS3")) return AE_SUPPORT;
    for (unsigned i = 0; i < targets; i++) if (cooling[i].handle == h) { *mask |= (uint64_t)1 << i; return AE_OK; }
    if (targets == FW_MAX) return AE_LIMIT;
    cooling[targets] = (struct cooling){h, resource, false, false};
    *mask |= (uint64_t)1 << targets++; return AE_OK;
}
static ACPI_STATUS fan(ACPI_HANDLE h, uint64_t *mask)
{
    /* A legacy fan may only supply _PR0. Flatten shared resources into one
     * target set so another zone can never turn off a resource still needed. */
    ACPI_OBJECT_TYPE type; ACPI_STATUS s = AcpiGetType(h, &type); if (s) return s;
    if (type == ACPI_TYPE_POWER) return target(h, mask);
    if (type != ACPI_TYPE_DEVICE || fw_has(h, "_FIF") || fw_has(h, "_FSL")) return AE_SUPPORT;
    bool found = false;
    for (unsigned i = 0; i < fan_count; i++) if (fans[i] == h) found = true;
    if (!found) { if (fan_count == FW_MAX) return AE_LIMIT; fans[fan_count++] = h; }
    bool pr = fw_has(h, "_PR0"), ps = fw_has(h, "_PS0");
    if (!pr && !ps) return AE_SUPPORT;
    if (fw_has(h, "_PR3")) {
        ACPI_BUFFER b = {ACPI_ALLOCATE_BUFFER, NULL}; s = fw_eval(h, "_PR3", NULL, &b);
        ACPI_OBJECT *o = b.Pointer;
        if (!s && (!o || o->Type != ACPI_TYPE_PACKAGE || o->Package.Count)) s = AE_SUPPORT;
        AcpiOsFree(b.Pointer); if (s) return s;
    }
    if (pr) {
        ACPI_BUFFER b = {ACPI_ALLOCATE_BUFFER, NULL}; s = fw_eval(h, "_PR0", NULL, &b);
        ACPI_OBJECT *o = b.Pointer;
        if (!s && (!o || o->Type != ACPI_TYPE_PACKAGE || !o->Package.Count || o->Package.Count > 8)) s = AE_BAD_DATA;
        if (!s) for (unsigned i = 0; i < o->Package.Count && !s; i++) {
            ACPI_OBJECT *e = &o->Package.Elements[i]; ACPI_OBJECT_TYPE t;
            if (e->Type != ACPI_TYPE_LOCAL_REFERENCE || AcpiGetType(e->Reference.Handle, &t) || t != ACPI_TYPE_POWER) s = AE_TYPE;
            else s = target(e->Reference.Handle, mask);
        }
        AcpiOsFree(b.Pointer); if (s) return s;
    }
    return ps ? target(h, mask) : AE_OK;
}
ACPI_STATUS fw_thermal_add(ACPI_HANDLE h, uint32_t id)
{
    if (fw_power.zone_count == CIUKI_POWER_ZONES) return AE_LIMIT;
    unsigned i = fw_power.zone_count++;
    zones[i] = (struct zone){h, CIUKI_POWER_UNKNOWN};
    memset(&fw_power.zones[i], 0xff, sizeof(fw_power.zones[i]));
    fw_power.zones[i].id = id; fw_power.zones[i].cooling = 0;
    fw_power.zones[i].passive_delta = INT32_MIN;
    return AE_OK;
}
static ACPI_STATUS shared_power(ACPI_HANDLE h, UINT32 level, void *ctx, void **ret)
{
    (void)level; (void)ctx; (void)ret;
    if (++inspected > FW_MAX) return AE_LIMIT;
    if (!fw_has(h,"_PR0")) return AE_OK;
    for (unsigned i = 0; i < fan_count; i++) if (fans[i] == h) return AE_OK;
    ACPI_BUFFER b = {ACPI_ALLOCATE_BUFFER, NULL}; ACPI_STATUS s = fw_eval(h,"_PR0",NULL,&b);
    ACPI_OBJECT *o = b.Pointer;
    if (!s && (!o || o->Type != ACPI_TYPE_PACKAGE || o->Package.Count > FW_MAX)) s = AE_BAD_DATA;
    if (!s) for (unsigned i = 0; i < o->Package.Count && !s; i++) {
        ACPI_OBJECT *e = &o->Package.Elements[i];
        if (e->Type != ACPI_TYPE_LOCAL_REFERENCE) { s = AE_TYPE; break; }
        for (unsigned k = 0; k < targets; k++)
            if (cooling[k].resource && cooling[k].handle == e->Reference.Handle) s = AE_SUPPORT;
    }
    AcpiOsFree(b.Pointer); return s;
}
ACPI_STATUS fw_thermal_refresh(bool apply)
{
    for (unsigned i = 0; i < targets; i++) cooling[i].wanted = false;
    for (unsigned i = 0; i < fw_power.zone_count && i < CIUKI_POWER_ZONES; i++) {
        struct ciuki_power_zone *z = &fw_power.zones[i]; ACPI_HANDLE h = zones[i].handle;
        uint32_t id = z->id;
        memset(z,0xff,sizeof(*z)); z->id = id; z->cooling = 0; z->passive_delta = INT32_MIN;
        uint32_t t; ACPI_STATUS s = fw_integer(h, "_TMP", &t);
        z->temperature = CIUKI_POWER_UNKNOWN;
        if (s || !temperature(t)) return s ? s : AE_BAD_DATA;
        z->temperature = t;
        if (fw_has(h, "_RTV") || fw_has(h, "_ART") || fw_has(h, "_TRT")) return AE_SUPPORT;
        s = fw_integer(h, "_CRT", &z->critical);
        if (s || !temperature(z->critical)) return s ? s : AE_BAD_DATA;
        if (fw_has(h, "_HOT")) {
            s = fw_integer(h, "_HOT", &z->hot);
            if (s || !temperature(z->hot)) return s ? s : AE_BAD_DATA;
            return AE_SUPPORT; /* S4 is unavailable; never assume S5 implements _HOT. */
        }
        if (fw_has(h, "_PSV")) {
            uint32_t tc1, tc2, tsp;
            s = fw_integer(h, "_PSV", &z->passive);
            if (!s) s = fw_integer(h, "_TC1", &tc1);
            if (!s) s = fw_integer(h, "_TC2", &tc2);
            if (!s) s = fw_integer(h, "_TSP", &tsp);
            if (s || !temperature(z->passive) || !tsp || tc1 > 10000 || tc2 > 10000 || !fw_has(h, "_PSL")) return s ? s : AE_BAD_DATA;
            ACPI_BUFFER b = {ACPI_ALLOCATE_BUFFER, NULL}; s = fw_eval(h, "_PSL", NULL, &b);
            ACPI_OBJECT *o = b.Pointer;
            if (!s && (!o || o->Type != ACPI_TYPE_PACKAGE || !o->Package.Count || o->Package.Count > FW_MAX)) s = AE_BAD_DATA;
            if (!s && zones[i].previous != CIUKI_POWER_UNKNOWN) {
                int64_t delta = (int64_t)tc1 * ((int64_t)t - zones[i].previous) + (int64_t)tc2 * ((int64_t)t - z->passive);
                z->passive_delta = delta > INT32_MAX ? INT32_MAX : delta < -INT32_MAX ? -INT32_MAX : (int32_t)delta;
            }
            AcpiOsFree(b.Pointer); zones[i].previous = t;
            return s ? s : AE_SUPPORT; /* No qualified processor throttling provider. */
        }
        uint32_t mask = 0;
        for (unsigned k = 0; k < 10; k++) {
            char trip[] = "_AC0", list[] = "_AL0"; trip[3] += k; list[3] += k;
            z->active[k] = CIUKI_POWER_UNKNOWN; demand[i][k] = 0;
            if (!fw_has(h, trip)) { if (fw_has(h, list)) return AE_BAD_DATA; continue; }
            s = fw_integer(h, trip, &z->active[k]);
            if (s || !temperature(z->active[k]) || z->active[k] >= z->critical) return s ? s : AE_BAD_DATA;
            ACPI_BUFFER b = {ACPI_ALLOCATE_BUFFER, NULL}; s = fw_eval(h, list, NULL, &b);
            ACPI_OBJECT *o = b.Pointer;
            if (!s && (!o || o->Type != ACPI_TYPE_PACKAGE || !o->Package.Count || o->Package.Count > 8)) s = AE_BAD_DATA;
            if (!s) for (unsigned j = 0; j < o->Package.Count && !s; j++) {
                ACPI_OBJECT *e = &o->Package.Elements[j];
                s = e->Type == ACPI_TYPE_LOCAL_REFERENCE ? fan(e->Reference.Handle, &demand[i][k]) : AE_TYPE;
            }
            AcpiOsFree(b.Pointer); if (s) return s;
            if (t >= z->active[k]) {
                mask |= 1u << k;
                for (unsigned j = 0; j < targets; j++) if (demand[i][k] & ((uint64_t)1 << j)) cooling[j].wanted = true;
            }
        }
        z->cooling = mask;
        if (t >= z->critical) { if (apply) fw_power_request(); else return AE_LIMIT; }
        zones[i].previous = t;
    }
    /* Refuse to switch a resource also needed by a device outside the
     * thermal fan set; the native VMM does not own that device's D-state. */
    if (targets) {
        inspected = 0;
        ACPI_STATUS s = AcpiWalkNamespace(ACPI_TYPE_DEVICE,ACPI_ROOT_OBJECT,64,shared_power,NULL,NULL,NULL);
        if (s) return s;
    }
    if (!apply) return AE_OK;
    /* Resources before devices on activation; reverse on deactivation. */
    for (unsigned pass = 0; pass < 4; pass++) for (unsigned i = 0; i < targets; i++) {
        struct cooling *c = &cooling[i];
        if (c->wanted == c->on || (pass < 2) != c->wanted ||
            c->resource != (pass == 0 || pass == 3)) continue;
        char *method = c->resource ? (c->wanted ? "_ON" : "_OFF") : (c->wanted ? "_PS0" : "_PS3");
        ACPI_STATUS s = fw_eval(c->handle, method, NULL, NULL); if (s) return s;
        c->on = c->wanted;
    }
    return AE_OK;
}
