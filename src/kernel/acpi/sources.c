/* ACPI 6.5 chapters 9/10; original implementation, no donor code.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "runtime.h"
#include "acclib.h"

static struct source { ACPI_HANDLE handle; uint32_t caps, id; } sources[FW_MAX];
static unsigned count;
void fw_classes_reset(void)
{
    count = 0; fw_thermal_reset(); fw_ec_reset();
    memset(&fw_power, 0, sizeof(fw_power));
    memset(fw_power.batteries,0xff,sizeof(fw_power.batteries));
    memset(fw_power.zones,0xff,sizeof(fw_power.zones));
    fw_power.version = CIUKI_POWER_VERSION; fw_power.size = sizeof(fw_power);
    fw_power.ac_online = fw_power.lid_open = CIUKI_POWER_UNKNOWN;
    fw_power.ownership = CIUKI_POWER_PENDING;
}
bool fw_has(ACPI_HANDLE h, char *name)
{ ACPI_HANDLE out; return !AcpiGetHandle(h, name, &out); }
ACPI_STATUS fw_eval(ACPI_HANDLE h, char *name, ACPI_OBJECT_LIST *args, ACPI_BUFFER *out)
{
    fw_budget_begin(100000, 250);
    ACPI_STATUS s = AcpiEvaluateObject(h, name, args, out);
    if (fw_error()) s = fw_error();
    fw_budget_end(); return s;
}
ACPI_STATUS fw_integer(ACPI_HANDLE h, char *name, uint32_t *v)
{
    ACPI_BUFFER b = {ACPI_ALLOCATE_BUFFER, NULL};
    ACPI_STATUS s = fw_eval(h, name, NULL, &b);
    ACPI_OBJECT *o = b.Pointer;
    if (!s && (!o || o->Type != ACPI_TYPE_INTEGER || o->Integer.Value > UINT32_MAX)) s = AE_TYPE;
    if (!s) *v = o->Integer.Value;
    AcpiOsFree(b.Pointer); return s;
}
static bool dword(const ACPI_OBJECT *o, uint32_t *v)
{
    if (o->Type != ACPI_TYPE_INTEGER || o->Integer.Value > UINT32_MAX) return false;
    *v = o->Integer.Value; return true;
}
static bool measurement(uint32_t v) { return v <= INT32_MAX || v == CIUKI_POWER_UNKNOWN; }
static void battery(struct source *src, struct ciuki_power_battery *b)
{
    memset(b, 0xff, sizeof(*b)); b->id = src->id;
    uint32_t sta = 0;
    ACPI_STATUS s = fw_integer(src->handle, "_STA", &sta);
    if (s) return; /* absent _STA is not evidence that a battery is installed */
    b->present = !!(sta & 0x10);
    if (!b->present || !(sta & 1)) return;
    ACPI_BUFFER out = {ACPI_ALLOCATE_BUFFER, NULL};
    bool bix = fw_has(src->handle, "_BIX");
    s = fw_eval(src->handle, bix ? "_BIX" : "_BIF", NULL, &out);
    ACPI_OBJECT *o = out.Pointer;
    unsigned base = bix ? 1 : 0;
    uint32_t rev = 0, unit, design, full;
    bool valid = !s && o && o->Type == ACPI_TYPE_PACKAGE && o->Package.Count >= (bix ? 20u : 13u);
    if (valid && bix) valid = dword(&o->Package.Elements[0], &rev) && rev <= 1 &&
        (rev == 0 || o->Package.Count >= 21);
    if (valid) valid = dword(&o->Package.Elements[base], &unit) && unit <= 1 &&
        dword(&o->Package.Elements[base + 1], &design) && measurement(design) &&
        dword(&o->Package.Elements[base + 2], &full) && measurement(full);
    if (valid) { b->unit = unit; b->design = design; b->full = full; }
    AcpiOsFree(out.Pointer);
    if (!valid) return; /* invalid _BIX never silently falls back to stale _BIF */
    out = (ACPI_BUFFER){ACPI_ALLOCATE_BUFFER, NULL};
    s = fw_eval(src->handle, "_BST", NULL, &out); o = out.Pointer;
    uint32_t state, rate, remaining, voltage;
    valid = !s && o && o->Type == ACPI_TYPE_PACKAGE && o->Package.Count == 4 &&
        dword(&o->Package.Elements[0], &state) && !(state & ~15u) && (state & 3) != 3 &&
        dword(&o->Package.Elements[1], &rate) && measurement(rate) &&
        dword(&o->Package.Elements[2], &remaining) && measurement(remaining) &&
        dword(&o->Package.Elements[3], &voltage) && measurement(voltage);
    if (valid) { b->state = state; b->rate = rate; b->remaining = remaining; b->voltage = voltage; }
    if (valid && (state & 4) && fw_owned) fw_power_request();
    AcpiOsFree(out.Pointer);
}
ACPI_STATUS fw_sources_add(ACPI_HANDLE h, uint32_t caps, uint32_t id)
{
    if (!(caps & (CIUKI_FW_CAP_BATTERY | CIUKI_FW_CAP_LID | CIUKI_FW_CAP_AC | CIUKI_FW_CAP_BUTTON))) return AE_OK;
    if (count == FW_MAX) return AE_LIMIT;
    if (caps & CIUKI_FW_CAP_BATTERY) {
        if (fw_power.battery_count == CIUKI_POWER_BATTERIES) return AE_LIMIT;
        fw_power.batteries[fw_power.battery_count].id = id;
        fw_power.battery_count++;
    }
    sources[count++] = (struct source){h, caps, id}; return AE_OK;
}
void fw_sources_refresh(void)
{
    unsigned n = 0, acs = 0, lids = 0;
    uint32_t ac = CIUKI_POWER_UNKNOWN, lid = CIUKI_POWER_UNKNOWN;
    bool ac_bad = false, lid_bad = false;
    for (unsigned i = 0; i < count; i++) {
        struct source *s = &sources[i]; uint32_t v;
        if ((s->caps & CIUKI_FW_CAP_BATTERY) && n < CIUKI_POWER_BATTERIES) battery(s, &fw_power.batteries[n++]);
        if (s->caps & CIUKI_FW_CAP_AC) {
            if (fw_integer(s->handle, "_PSR", &v) || v > 1) ac_bad = true;
            else if (acs++ && ac != v) ac_bad = true;
            else ac = v;
        }
        if (s->caps & CIUKI_FW_CAP_LID) {
            if (fw_integer(s->handle, "_LID", &v)) lid_bad = true;
            else if (lids++ && lid != !!v) lid_bad = true;
            else lid = !!v;
        }
    }
    fw_power.ac_online = ac_bad ? CIUKI_POWER_UNKNOWN : ac;
    fw_power.lid_open = lid_bad ? CIUKI_POWER_UNKNOWN : lid;
}
