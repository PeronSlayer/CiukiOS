/* Cached namespace/PnP facts; no activation or I/O in the page ABI.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "runtime.h"
#include "accommon.h"
#include "acinterp.h"

static struct ciuki_firmware_record records[FW_MAX];
static ACPI_HANDLE handles[FW_MAX];
static uint32_t count, flags = CIUKI_FIRMWARE_PENDING, status, generation = 1, high_water;
static bool complete;
static void text(char *dst, size_t size, const char *src, struct ciuki_firmware_record *r)
{
    size_t n = strlen(src);
    if (n >= size) { n = size - 1; r->flags |= CIUKI_FIRMWARE_TRUNCATED; flags |= CIUKI_FIRMWARE_TRUNCATED; }
    memcpy(dst, src, n); dst[n] = 0;
}
static struct ciuki_firmware_record *append(void)
{
    if (count == FW_MAX) { flags |= CIUKI_FIRMWARE_TRUNCATED; return NULL; }
    struct ciuki_firmware_record *r = &records[count++];
    memset(r, 0, sizeof(*r)); r->id = count;
    r->state = CIUKI_FIRMWARE_DISCOVERED; return r;
}
static void unavailable(struct ciuki_firmware_record *r, ACPI_STATUS s)
{
    if (!s || s == AE_NOT_FOUND) return;
    flags |= CIUKI_FIRMWARE_INCOMPLETE;
    r->flags |= CIUKI_FIRMWARE_INCOMPLETE;
    r->state = CIUKI_FIRMWARE_UNSUPPORTED;
    text(r->reason, sizeof(r->reason), s == AE_ACCESS ? "Firmware method requires an owned region or EC service." :
         s == AE_TIME ? "Firmware evaluation budget expired." : s == AE_NO_MEMORY ?
         "Firmware inventory allocation failed." : "Firmware object or resource is invalid or unsupported.", r);
}
static ACPI_STATUS evaluate(ACPI_HANDLE handle, char *name, ACPI_BUFFER *out)
{
    fw_budget_stage(name, handle);
    fw_budget_begin(100000, 250);
    ACPI_STATUS s = AcpiEvaluateObject(handle, name, NULL, out);
    if (fw_error()) s = fw_error();
    fw_budget_end(); return s;
}
static void identifier(ACPI_OBJECT *o, char *out, size_t size, struct ciuki_firmware_record *r)
{
    if (o->Type == ACPI_TYPE_STRING && o->String.Pointer && o->String.Length) {
        /* ACPICA strings terminate, but reject embedded NUL/control bytes. */
        size_t length = o->String.Length;
        if (length >= size) { length = size - 1; r->flags |= CIUKI_FIRMWARE_TRUNCATED; flags |= CIUKI_FIRMWARE_TRUNCATED; }
        for (unsigned i = 0; i < length; i++) if ((unsigned char)o->String.Pointer[i] < 32 ||
            (unsigned char)o->String.Pointer[i] > 126) { unavailable(r, AE_BAD_DATA); return; }
        memcpy(out, o->String.Pointer, length); out[length] = 0;
    } else if (o->Type == ACPI_TYPE_INTEGER && o->Integer.Value <= UINT32_MAX) {
        char id[8]; AcpiExEisaIdToString(id, (UINT32)o->Integer.Value); text(out, size, id, r);
    } else unavailable(r, AE_TYPE);
}
static bool has(ACPI_HANDLE h, char *name)
{ ACPI_HANDLE child; return ACPI_SUCCESS(AcpiGetHandle(h, name, &child)); }
static bool matches(const struct ciuki_firmware_record *r, const char *id)
{
    if (!strcmp(r->hid,id)) return true;
    for (unsigned i = 0; i < r->cid_count; i++) if (!strcmp(r->cid[i],id)) return true;
    return false;
}
static void ids(ACPI_HANDLE h, struct ciuki_firmware_record *r)
{
    ACPI_BUFFER out = {ACPI_ALLOCATE_BUFFER, NULL};
    ACPI_STATUS s = evaluate(h, "_HID", &out);
    if (!s && out.Pointer) identifier(out.Pointer, r->hid, sizeof(r->hid), r);
    unavailable(r, s); AcpiOsFree(out.Pointer);
    out = (ACPI_BUFFER){ACPI_ALLOCATE_BUFFER, NULL}; s = evaluate(h, "_CID", &out);
    if (!s && out.Pointer) {
        ACPI_OBJECT *o = out.Pointer;
        unsigned n = o->Type == ACPI_TYPE_PACKAGE ? o->Package.Count : 1;
        if (n > 4) { r->flags |= CIUKI_FIRMWARE_TRUNCATED; flags |= CIUKI_FIRMWARE_TRUNCATED; n = 4; }
        for (unsigned i = 0; i < n; i++) {
            ACPI_OBJECT *id = o->Type == ACPI_TYPE_PACKAGE ? &o->Package.Elements[i] : o;
            identifier(id, r->cid[r->cid_count], sizeof(r->cid[0]), r);
            if (r->cid[r->cid_count][0]) r->cid_count++;
        }
    }
    unavailable(r, s); AcpiOsFree(out.Pointer);
}
static void extent(struct ciuki_firmware_record *r, unsigned type, uint64_t a, uint64_t n)
{
    if (!n || a > UINT64_MAX - n || (type == 0 && (a > 65535 || n > 65536 - a)) ||
        (type == 1 && (a > UINT32_MAX || n != 1)) || (type == 2 && (a > 7 || n != 1))) {
        unavailable(r, AE_BAD_ADDRESS); return;
    }
    if (r->resource_count == 8) { r->flags |= CIUKI_FIRMWARE_TRUNCATED; flags |= CIUKI_FIRMWARE_TRUNCATED; return; }
    r->resources[r->resource_count++] = (struct ciuki_firmware_resource){type, 0, a, a + n};
    if (type == 1 && r->state == CIUKI_FIRMWARE_DISCOVERED) r->state = CIUKI_FIRMWARE_ROUTING_UNQUALIFIED;
    if (a + n > UINT32_MAX && type == 3) unavailable(r, AE_BAD_ADDRESS); /* retain 64-bit fact */
}
static ACPI_STATUS resource(ACPI_RESOURCE *res, void *ctx)
{
    if (AcpiCiukiBudgetStep()) return fw_error();
    struct ciuki_firmware_record *r = ctx;
    switch (res->Type) {
    case ACPI_RESOURCE_TYPE_IO:
        if (res->Data.Io.Minimum!=res->Data.Io.Maximum) unavailable(r,AE_BAD_DATA);
        else extent(r, 0, res->Data.Io.Minimum, res->Data.Io.AddressLength);
        break;
    case ACPI_RESOURCE_TYPE_FIXED_IO: extent(r, 0, res->Data.FixedIo.Address, res->Data.FixedIo.AddressLength); break;
    case ACPI_RESOURCE_TYPE_IRQ:
        for (unsigned i = 0; i < res->Data.Irq.InterruptCount; i++) {
            unsigned before=r->resource_count; extent(r, 1, res->Data.Irq.Interrupts[i], 1);
            if (r->resource_count>before) r->resources[before].flags=
                (res->Data.Irq.Triggering==ACPI_EDGE_SENSITIVE ? 1u : 0u) |
                (res->Data.Irq.Shareable==ACPI_EXCLUSIVE ? 2u : 0u) |
                (res->Data.Irq.Polarity==ACPI_ACTIVE_HIGH ? 4u : 0u);
        }
        break;
    case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
        for (unsigned i = 0; i < res->Data.ExtendedIrq.InterruptCount; i++) {
            unsigned before=r->resource_count; extent(r, 1, res->Data.ExtendedIrq.Interrupts[i], 1);
            if (r->resource_count>before) r->resources[before].flags=
                (res->Data.ExtendedIrq.Triggering==ACPI_EDGE_SENSITIVE ? 1u : 0u) |
                (res->Data.ExtendedIrq.Shareable==ACPI_EXCLUSIVE ? 2u : 0u) |
                (res->Data.ExtendedIrq.Polarity==ACPI_ACTIVE_HIGH ? 4u : 0u);
        }
        break;
    case ACPI_RESOURCE_TYPE_DMA:
        for (unsigned i = 0; i < res->Data.Dma.ChannelCount; i++) extent(r, 2, res->Data.Dma.Channels[i], 1);
        break;
    case ACPI_RESOURCE_TYPE_MEMORY24: extent(r, 3, (uint64_t)res->Data.Memory24.Minimum << 8, (uint64_t)res->Data.Memory24.AddressLength << 8); break;
    case ACPI_RESOURCE_TYPE_MEMORY32: extent(r, 3, res->Data.Memory32.Minimum, res->Data.Memory32.AddressLength); break;
    case ACPI_RESOURCE_TYPE_FIXED_MEMORY32: extent(r, 3, res->Data.FixedMemory32.Address, res->Data.FixedMemory32.AddressLength); break;
    case ACPI_RESOURCE_TYPE_ADDRESS16:
    case ACPI_RESOURCE_TYPE_ADDRESS32:
    case ACPI_RESOURCE_TYPE_ADDRESS64: {
        ACPI_RESOURCE_ADDRESS64 a;
        ACPI_STATUS s = AcpiResourceToAddress64(res, &a);
        if (s || a.ResourceType > ACPI_IO_RANGE || a.Address.TranslationOffset ||
            !a.MinAddressFixed || !a.MaxAddressFixed || a.Address.Minimum > a.Address.Maximum) unavailable(r, AE_SUPPORT);
        else if (a.Address.AddressLength) {
            if (a.Address.AddressLength - 1 > a.Address.Maximum - a.Address.Minimum) unavailable(r, AE_BAD_ADDRESS);
            else extent(r, a.ResourceType == ACPI_IO_RANGE ? 0 : 3, a.Address.Minimum, a.Address.AddressLength);
        }
        break;
    }
    case ACPI_RESOURCE_TYPE_END_TAG: break;
    default: unavailable(r, AE_SUPPORT); break;
    }
    return AE_OK;
}
static ACPI_STATUS device(ACPI_HANDLE handle, UINT32 level, void *ctx, void **ret)
{
    (void)ctx; (void)ret;
    if (level == 64) {
        ACPI_HANDLE child;
        if (!AcpiGetNextObject(ACPI_TYPE_ANY, handle, NULL, &child)) flags |= CIUKI_FIRMWARE_TRUNCATED;
    }
    fw_budget_stage("walk", NULL); /* fx-3: a walk failure names the walk, not the last method */
    ACPI_OBJECT_TYPE type; if (AcpiGetType(handle, &type)) return AE_OK;
    if (type != ACPI_TYPE_DEVICE && type != ACPI_TYPE_PROCESSOR && type != ACPI_TYPE_THERMAL && type != ACPI_TYPE_POWER) return AE_OK;
    fw_budget_stage("walk", handle);
    struct ciuki_firmware_record *r = append(); if (!r) return AE_CTRL_TERMINATE;
    handles[count - 1] = handle; r->origin = CIUKI_FIRMWARE_ACPI;
    ACPI_BUFFER path = {ACPI_ALLOCATE_BUFFER, NULL};
    if (!AcpiGetName(handle, ACPI_FULL_PATHNAME, &path) && path.Pointer) text(r->path, sizeof(r->path), path.Pointer, r);
    else unavailable(r, AE_NO_MEMORY);
    AcpiOsFree(path.Pointer);
    ACPI_HANDLE parent = handle;
    while (!AcpiGetParent(parent, &parent)) {
        for (unsigned i = 0; i + 1 < count; i++) if (handles[i] == parent) { r->parent = records[i].id; break; }
        if (r->parent) break;
    }
    text(r->reason, sizeof(r->reason), "Firmware description only; device and power ownership unchanged.", r);
    ids(handle, r);
    bool serial=matches(r,"PNP0500") || matches(r,"PNP0501");
    if (serial) {
        ACPI_BUFFER sta={ACPI_ALLOCATE_BUFFER,NULL}; ACPI_STATUS error=evaluate(handle,"_STA",&sta);
        if (!error && (!sta.Pointer || ((ACPI_OBJECT *)sta.Pointer)->Type!=ACPI_TYPE_INTEGER)) error=AE_TYPE;
        bool enabled=error==AE_NOT_FOUND || (!error && (((ACPI_OBJECT *)sta.Pointer)->Integer.Value&3)==3);
        unavailable(r,error); AcpiOsFree(sta.Pointer);
        if (!enabled) {
            r->state=CIUKI_FIRMWARE_UNSUPPORTED;
            text(r->reason,sizeof(r->reason),"Serial device absent, disabled or status unavailable; no activation.",r);
            return AE_OK;
        }
    }
    if (matches(r,"PNP0C0C") || matches(r,"PNP0C0E")) r->capabilities |= CIUKI_FW_CAP_BUTTON;
    if (matches(r,"PNP0C09")) r->capabilities |= CIUKI_FW_CAP_EC;
    if (has(handle, "_LID")) r->capabilities |= CIUKI_FW_CAP_LID;
    if (has(handle, "_PSR")) r->capabilities |= CIUKI_FW_CAP_AC;
    if (has(handle, "_BST") || has(handle, "_BIF") || has(handle, "_BIX")) r->capabilities |= CIUKI_FW_CAP_BATTERY;
    if (type == ACPI_TYPE_THERMAL || has(handle, "_TMP")) r->capabilities |= CIUKI_FW_CAP_THERMAL;
    if (type == ACPI_TYPE_POWER || has(handle, "_PS0")) r->capabilities |= CIUKI_FW_CAP_POWER;
    if (has(handle, "_PRW")) r->capabilities |= CIUKI_FW_CAP_WAKE;
    fw_budget_stage("_CRS", handle);
    fw_budget_begin(100000, 250);
    ACPI_STATUS s = AcpiWalkResources(handle, "_CRS", resource, r);
    if (fw_error()) s = fw_error();
    fw_budget_end(); unavailable(r, s);
    return AE_OK;
}
void fw_pnp_discover(void)
{
    const uint8_t *rom = fw_os->table_map(0xf0000, 65536);
    if (!rom) return;
    for (uint32_t offset = 0; offset <= 65536 - 6; offset += 16) {
        const uint8_t *p = rom + offset;
        bool sig = !memcmp((void *)p, "$PnP", 4); unsigned n = p[5];
        if (!sig) continue;
        if (n < 0x21 || offset + n > 65536) { flags |= CIUKI_FIRMWARE_INCOMPLETE; continue; }
        uint8_t sum = 0; for (unsigned i = 0; i < n; i++) sum += p[i];
        if (sum) { flags |= CIUKI_FIRMWARE_INCOMPLETE; continue; }
        struct ciuki_firmware_record *r = append(); if (!r) break;
        r->origin = CIUKI_FIRMWARE_PNP_BIOS; r->state = CIUKI_FIRMWARE_UNSUPPORTED;
        text(r->path, sizeof(r->path), "PnP BIOS", r); text(r->hid, sizeof(r->hid), "$PnP", r);
        text(r->reason, sizeof(r->reason), "PnP BIOS present; firmware node calls and ISA configuration are disabled.", r);
        break;
    }
    fw_os->table_unmap(rom, 65536);
}
static ACPI_STATUS inventory_run(const struct fw_ops *ops, uint32_t rsdp, bool power)
{
    complete = false; count = 0; flags = 0; status = 0; memset(handles, 0, sizeof(handles));
    fw_bind(ops); fw_runtime = power;
    fw_scan_begin();
    fw_classes_reset();
    fw_budget_stage("tables", NULL);
    fw_pnp_discover();
    status = fw_tables_discover(rsdp);
    bool initialized = false;
    if (!status) {
        fw_budget_stage("load", NULL);
        fw_budget_begin(100000, FW_LOAD_RUN_MS);
        initialized = true; status = AcpiInitializeSubsystem();
        AcpiGbl_EnableInterpreterSlack = FALSE;
        AcpiGbl_EnableTableValidation = TRUE;
        if (!status) status = AcpiInitializeTables(NULL, FW_MAX, FALSE);
        if (!status) status = AcpiLoadTables();
        if (fw_error()) status = fw_error();
        fw_budget_end();
        if (!status) {
            struct ciuki_firmware_record *root = append();
            if (root) {
                handles[count - 1] = AcpiGbl_RootNode; root->origin = CIUKI_FIRMWARE_ACPI;
                text(root->path, sizeof(root->path), "\\", root);
                text(root->reason, sizeof(root->reason), "ACPI description only; SCI state and current power values are unknown.", root);
                if (!(AcpiGbl_FADT.Flags & ACPI_FADT_POWER_BUTTON) || !(AcpiGbl_FADT.Flags & ACPI_FADT_SLEEP_BUTTON)) root->capabilities |= CIUKI_FW_CAP_BUTTON;
                if (has(ACPI_ROOT_OBJECT, "_S5")) root->capabilities |= CIUKI_FW_CAP_POWER;
            }
            fw_budget_stage("walk", NULL);
            status = AcpiWalkNamespace(ACPI_TYPE_ANY, ACPI_ROOT_OBJECT, 64, device, NULL, NULL, NULL);
        }
        if (!status && power) {
            if (flags & CIUKI_FIRMWARE_TRUNCATED) status = AE_LIMIT;
            fw_budget_stage("collect", NULL);
            /* Bind EC first, before dependent thermal/battery AML. */
            for (unsigned pass = 0; pass < 2 && !status; pass++)
                for (unsigned i = 0; i < count && !status; i++) {
                    uint32_t caps = records[i].capabilities;
                    if (!handles[i] || handles[i] == AcpiGbl_RootNode || !!(caps & CIUKI_FW_CAP_EC) != !pass) continue;
                    status = fw_power_collect(handles[i], caps, records[i].id,matches(&records[i],"PNP0C0C"));
                }
            if (!status) { fw_budget_stage("takeover", NULL); status = fw_power_takeover(); }
            if (status) { fw_power.error = status; fw_power_stop(); }
        }
        uint32_t live; fw_alloc_stats(&live, &high_water);
        if (initialized && !fw_owned) (void)AcpiTerminate();
    }
    if (!power && !status && fw_run_error()) status = fw_run_error();
    if (power && status && !fw_owned) {
        fw_power.error = status;
        if (fw_power.ownership == CIUKI_POWER_PENDING) fw_power.ownership = CIUKI_POWER_FIRMWARE;
        fw_power_publish();
    }
    if (status) flags |= CIUKI_FIRMWARE_INCOMPLETE;
    fw_budget_end();
    fw_scan_end();
    if (!fw_owned) { fw_os_cleanup(); fw_runtime = false; }
    if (!power) { fw_power.error = status; fw_power.ownership = CIUKI_POWER_FIRMWARE; fw_power_publish(); }
    complete = true;
    fw_ec_settle(); /* fx-3: ownership decided; a kernel EC quirk may attach */
    return status;
}
ACPI_STATUS fw_inventory_run(const struct fw_ops *ops, uint32_t rsdp) { return inventory_run(ops, rsdp, false); }
ACPI_STATUS fw_inventory_power_run(const struct fw_ops *ops, uint32_t rsdp) { return inventory_run(ops, rsdp, true); }
void fw_inventory_unavailable(ACPI_STATUS error)
{
    count = high_water = fw_table_count = 0;
    flags = CIUKI_FIRMWARE_INCOMPLETE; status = error; complete = true;
    fw_classes_reset(); fw_power.error = error; fw_power.ownership = CIUKI_POWER_FIRMWARE; fw_power_publish();
    fw_ec_settle(); /* fx-3: ownership decided (firmware) */
}
int firmware_inventory_page(struct ciuki_firmware_page *p)
{
    if (p->version != CIUKI_FIRMWARE_VERSION || p->size != sizeof(*p) || p->reserved ||
        p->cursor > count || (!p->generation && p->cursor)) return -22;
    if (p->generation && p->generation != generation) return -11;
    uint32_t cursor = p->cursor;
    memset(p, 0, sizeof(*p)); p->version = CIUKI_FIRMWARE_VERSION; p->size = sizeof(*p);
    p->cursor = cursor; p->generation = generation; p->total = complete ? count : 0;
    p->flags = complete ? flags : CIUKI_FIRMWARE_PENDING; p->status = status;
    p->peak_bytes = high_water; p->table_count = fw_table_count;
    if (!complete) return 0;
    for (; cursor < count && p->count < CIUKI_FIRMWARE_PAGE_RECORDS; cursor++) p->records[p->count++] = records[cursor];
    p->next = cursor < count ? cursor : 0;
    return 0;
}
