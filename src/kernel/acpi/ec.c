/* One EC engine for AML and vendor quirks. ACPI 6.5 chapter 12.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "runtime.h"
#include "acclib.h"
static ACPI_HANDLE ec_handle;
static uint32_t ports[2], ec_gpe;
static unsigned port_count;
static bool ec_bound, ec_busy, ec_pending, ec_failed, ec_query_pending;
static bool ec_preflight;
static unsigned transaction_io;
static ACPI_STATUS read_port(uint32_t p, uint8_t *v)
{ UINT64 n = 0; if (ec_busy) transaction_io++; ACPI_STATUS s = fw_os->access(0, p, 8, &n, NULL); *v = n; return s; }
static ACPI_STATUS write_port(uint32_t p, uint8_t v)
{ if (ec_busy) transaction_io++; return fw_os->write ? fw_os->write(0, p, 8, v, NULL) : AE_ACCESS; }
static ACPI_STATUS wait_status(unsigned mask, unsigned wanted, uint64_t end, unsigned *polls)
{
    while ((*polls)++ < 500 && fw_os->clock() < end) {
        uint8_t status; ACPI_STATUS s = read_port(ports[1], &status);
        if (s || status == 0xff) return s ? s : AE_NOT_EXIST;
        if ((status & mask) == wanted) return AE_OK;
        fw_os->pause(1); /* independent progress: never depends on queued GPE work */
    }
    return AE_TIME;
}
static ACPI_STATUS transaction(uint8_t command, uint8_t address, uint8_t *v, unsigned ms)
{
    if (!ec_bound || ec_failed || (!fw_owned && !ec_preflight)) return AE_ACCESS;
    uint32_t flags = fw_os->lock();
    if (ec_busy) { fw_os->unlock(flags); return AE_ALREADY_ACQUIRED; }
    ec_busy = true; transaction_io = 0; fw_os->unlock(flags);
    if (ms > 500) ms = 500;
    unsigned polls = 0; uint64_t end = fw_os->clock() + (uint64_t)ms * 10000;
    ACPI_STATUS s = wait_status(3, 0, end, &polls);
    if (!s) s = write_port(ports[1], command);
    if (!s) s = wait_status(2, 0, end, &polls);
    if (!s && command != 0x84) s = write_port(ports[0], address);
    if (!s && command != 0x84) s = wait_status(2, 0, end, &polls);
    if (!s && command == 0x81) {
        s = write_port(ports[0], *v);
        if (!s) s = wait_status(2, 0, end, &polls);
    } else if (!s) {
        ec_pending = true; s = wait_status(1, 1, end, &polls);
        if (!s) { s = read_port(ports[0], v); if (!s) ec_pending = false; }
    }
    /* A timed-out transaction cannot be reassigned to a new caller. Retain
     * the ports; no draining an unsolicited byte or guessing EC reset state. */
    if (s) ec_failed = true;
    flags = fw_os->lock(); ec_busy = false; fw_os->unlock(flags); return s;
}
bool fw_ec_ready(void) { return ec_bound && fw_owned && !ec_failed; }
bool fw_ec_idle(void) { return !ec_failed && !ec_busy && !ec_pending; }
bool fw_ec_faulted(void) { return ec_failed; }
/* Kernel-only quirk broker. No AML or shared worker budget in this call. */
int acpi_ec_quirk_transfer_counted(bool write, uint8_t address, uint8_t *value, unsigned ms, unsigned *cost)
{
    if (cost) *cost = 0;
    if (ports[0] != 0x62 || ports[1] != 0x66) return -19;
    if (!ms || !value) return -110;
    ACPI_STATUS s = transaction(write ? 0x81 : 0x80, address, value, ms);
    if (cost && s != AE_ALREADY_ACQUIRED && s != AE_ACCESS) *cost = transaction_io;
    return !s ? 0 : s == AE_ALREADY_ACQUIRED ? -16 : s == AE_TIME ? -110 : -19;
}
int acpi_ec_quirk_transfer(bool write, uint8_t address, uint8_t *value)
{ return acpi_ec_quirk_transfer_counted(write,address,value,500,NULL); }
static ACPI_STATUS ec_region(UINT32 function, ACPI_PHYSICAL_ADDRESS address, UINT32 bits,
                              UINT64 *value, void *handler, void *region)
{
    (void)handler; (void)region;
    if (fw_error()) return fw_error();
    if (!value || bits != 8 || address > 255 || (function != ACPI_READ && function != ACPI_WRITE)) return AE_BAD_PARAMETER;
    uint8_t v = *value;
    ACPI_STATUS s = transaction(function == ACPI_WRITE ? 0x81 : 0x80, address, &v, 250);
    if (!s && function == ACPI_READ) *value = v;
    if (s) fw_fail(s);
    return s;
}
static ACPI_STATUS ec_resources(ACPI_RESOURCE *r, void *ctx)
{
    (void)ctx;
    uint32_t address = 0, n = 0;
    if (r->Type == ACPI_RESOURCE_TYPE_IO) { address = r->Data.Io.Minimum; n = r->Data.Io.AddressLength;
        if (r->Data.Io.Minimum != r->Data.Io.Maximum) return AE_BAD_DATA; }
    else if (r->Type == ACPI_RESOURCE_TYPE_FIXED_IO) { address = r->Data.FixedIo.Address; n = r->Data.FixedIo.AddressLength; }
    else if (r->Type == ACPI_RESOURCE_TYPE_END_TAG) return AE_OK;
    else return AE_SUPPORT;
    if (port_count == 2 || n != 1 || !address || address > 65535) return AE_BAD_DATA;
    ports[port_count++] = address; return AE_OK;
}
ACPI_STATUS fw_ec_add(ACPI_HANDLE h)
{
    if (ec_handle) return AE_SUPPORT; /* one explicitly identified EC, no ambiguous alias */
    uint32_t glk = 0; ACPI_STATUS s = fw_integer(h, "_GLK", &glk);
    if ((s && s != AE_NOT_FOUND) || glk) return s && s != AE_NOT_FOUND ? s : AE_SUPPORT;
    s = fw_integer(h, "_GPE", &ec_gpe); if (s || ec_gpe > 255) return s ? s : AE_SUPPORT;
    fw_budget_begin(100000, 250); s = AcpiWalkResources(h, "_CRS", ec_resources, NULL);
    if (fw_error()) s = fw_error(); fw_budget_end();
    if (s || port_count != 2 || ports[0] == ports[1]) return s ? s : AE_BAD_DATA;
    if (!fw_os->claim) return AE_ACCESS;
    s = fw_os->claim(ports[0], 1); if (!s) s = fw_os->claim(ports[1], 1);
    if (s) return s;
    ec_handle = h; ec_bound = ec_preflight = true;
    fw_budget_begin(100000, 250);
    s = AcpiInstallAddressSpaceHandler(h, ACPI_ADR_SPACE_EC, ec_region, NULL, NULL);
    if (fw_error()) s = fw_error(); fw_budget_end();
    return s;
}
static UINT32 ec_event(ACPI_HANDLE h, UINT32 number, void *ctx)
{ (void)h; (void)number; (void)ctx; ec_query_pending = true; return ACPI_INTERRUPT_HANDLED; }
ACPI_STATUS fw_ec_enable(void)
{
    ec_preflight = false;
    if (!ec_bound) return AE_OK;
    ACPI_STATUS s = AcpiInstallGpeHandler(NULL, ec_gpe, ACPI_GPE_LEVEL_TRIGGERED, ec_event, NULL);
    if (!s) s = AcpiEnableGpe(NULL, ec_gpe);
    return s;
}
void fw_ec_poll(void)
{
    if (!fw_ec_ready() || ec_busy) return;
    uint8_t status; ACPI_STATUS s = read_port(ports[1], &status);
    if (s || status == 0xff) { ec_failed = true; fw_power.error = s ? s : AE_NOT_EXIST; return; }
    if (!ec_query_pending && !(status & 0x20)) return;
    ec_query_pending = false;
    /* One query per worker turn; an event storm cannot monopolize AML. */
    if (status & 0x20) {
        uint8_t query = 0; s = transaction(0x84, 0, &query, 250);
        if (!s && query) {
            const char hex[] = "0123456789ABCDEF";
            char method[] = "_Q00"; method[2] = hex[query >> 4]; method[3] = hex[query & 15];
            s = fw_eval(ec_handle, method, NULL, NULL);
        }
        if (s) fw_power.error = s;
    }
    (void)AcpiFinishGpe(NULL, ec_gpe);
}
void fw_ec_reset(void)
{
    ec_handle = NULL; port_count = 0; ports[0] = ports[1] = ec_gpe = 0;
    ec_bound = ec_busy = ec_pending = ec_failed = ec_query_pending = ec_preflight = false;
}
