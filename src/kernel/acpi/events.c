/* ACPICA event lifecycle and ordered S5 worker. SPDX-License-Identifier: GPL-2.0-only */
#include "runtime.h"
#include "accommon.h"
#include "acevents.h"
#include "achware.h"
#include <ciuki/apm_ports.h>

struct ciuki_power_status fw_power;
bool fw_runtime, fw_writes, fw_owned, fw_interrupt;
static bool changed_mode, events_started, shutdown_latched, refresh_needed;
static bool original_acpi, mode_checked;
static bool provider_reserved; /* exclusive ACPI/APM power_source reservation */
static uint32_t requests;
static volatile unsigned off_request; /* 0 idle, 1 requested, 2 completed */
static int off_result;
static bool s5_available;

static uint64_t sources_at, request_at;
static struct ciuki_power_status published;
static ACPI_HANDLE power_buttons[FW_MAX], sleep_buttons[FW_MAX];
static unsigned sleep_button_count;
static uint64_t sleep_request_at;
static unsigned button_count;
static struct saved_enable { uint32_t address; uint8_t value; } enables[260];
static unsigned enable_count;
static ACPI_STATUS save_enables(const ACPI_GENERIC_ADDRESS *gas, unsigned length)
{
    uint64_t base; memcpy(&base,&gas->Address,sizeof(base));
    if (!base) return AE_OK;
    if (!length || length & 1) return AE_BAD_DATA;
    for (unsigned i = length / 2; i < length; i++) {
        if (enable_count == 260) return AE_LIMIT;
        UINT64 value = 0; ACPI_STATUS s = fw_os->access(0,base+i,8,&value,NULL);
        if (s) return s;
        enables[enable_count++] = (struct saved_enable){base+i,value};
    }
    return AE_OK;
}
void fw_power_publish(void)
{
    if (!fw_os) { published = fw_power; return; }
    uint32_t flags = fw_os->lock(); published = fw_power; fw_os->unlock(flags);
    if (fw_os->publish) fw_os->publish(&published);
}

void fw_power_request(void)
{
    /* Coalesce repeated fixed/AML notifications until desktop observes the
     * request. The read-only query is deliberately not an acknowledgement. */
    uint32_t flags = fw_os->lock();
    bool changed = false;
    if (!shutdown_latched || fw_os->clock() >= request_at) {
        shutdown_latched = true; fw_power.shutdown_requests = ++requests;
        request_at = fw_os->clock() + 10000000;
        published.shutdown_requests = requests; changed = true;
    }
    /* Before the first publication the next fw_power_publish carries it. */
    bool ready = changed && published.version;
    fw_os->unlock(flags);
    if (ready && fw_os->publish) fw_os->publish(&published);
}
static void sleep_request(void)
{
    uint32_t f = fw_os->lock();
    bool due = !sleep_request_at || fw_os->clock() >= sleep_request_at;
    if (due) sleep_request_at = fw_os->clock() + 10000000;
    fw_os->unlock(f);
    if (due && fw_os->sleep_request) fw_os->sleep_request();
}
static UINT32 button(void *ctx)
{ if (ctx) sleep_request(); else fw_power_request(); return ACPI_INTERRUPT_HANDLED; }
static void notify(ACPI_HANDLE h, UINT32 value, void *ctx)
{
    (void)ctx;
    if (value == 0x80) {
        for (unsigned i = 0; i < sleep_button_count; i++) if (sleep_buttons[i] == h) sleep_request();
        for (unsigned i = 0; i < button_count; i++) if (power_buttons[i] == h) fw_power_request();
    }
    /* fx-3 follow-up 2: a thermal zone's notification re-reads that zone
     * (ACPI 6.5 11.1.3.1); other device notifications refresh sources. */
    if ((value == 0x80 || value == 0x81 || value == 0x82) && !fw_thermal_notify(h)) refresh_needed = true;
}
static ACPI_STATUS gas_claim(const ACPI_GENERIC_ADDRESS *gas, unsigned bytes, bool required)
{
    uint64_t address; memcpy(&address, &gas->Address, sizeof(address));
    if (!address) return required ? AE_BAD_ADDRESS : AE_OK;
    if (gas->SpaceId != ACPI_ADR_SPACE_SYSTEM_IO || gas->BitOffset || address > 65535 ||
        !bytes || bytes > 256 || bytes > 65536 - address) return AE_SUPPORT;
    return fw_os->claim ? fw_os->claim(address, bytes) : AE_ACCESS;
}
static ACPI_STATUS mode(bool enable)
{
    uint32_t state;
    ACPI_STATUS s = AcpiReadBitRegister(ACPI_BITREG_SCI_ENABLE, &state);
    if (!s && enable && !mode_checked) { original_acpi = !!state; mode_checked = true; }
    if (s || (!!state == enable && (enable || !changed_mode))) return s;
    uint32_t command = AcpiGbl_FADT.SmiCommand;
    uint8_t value = enable ? AcpiGbl_FADT.AcpiEnable : AcpiGbl_FADT.AcpiDisable;
    if (!command || command > 65535 || !value) return AE_SUPPORT;
    /* Require a rollback command before changing ownership. */
    if (enable && !AcpiGbl_FADT.AcpiDisable) return AE_SUPPORT;
    s = AcpiOsWritePort(command, value, 8);
    if (enable && !s) changed_mode = true;
    uint64_t end = fw_os->clock() + 30000000;
    for (unsigned i = 0; !s && i < 3000 && fw_os->clock() < end; i++) {
        s = AcpiReadBitRegister(ACPI_BITREG_SCI_ENABLE, &state);
        if (!s && !!state == enable) return AE_OK;
        fw_os->pause(1);
    }
    return s ? s : AE_NO_HARDWARE_RESPONSE;
}
/* fx-18: the FADT's SystemIO fixed-hardware blocks, captured after the
 * tables load (before AcpiTerminate), for the APM BIOS port window. */
static struct apm_pm_blocks pm_blocks;
static bool pm_blocks_valid;
static uint16_t gas_io(const ACPI_GENERIC_ADDRESS *gas)
{
    uint64_t address; memcpy(&address, &gas->Address, sizeof(address));
    return gas->SpaceId == ACPI_ADR_SPACE_SYSTEM_IO && !gas->BitOffset && address && address <= 0xffff ? (uint16_t)address : 0;
}
void fw_pm_blocks_capture(void)
{
    struct apm_pm_blocks b = {
        .pm1a_evt = gas_io(&AcpiGbl_FADT.XPm1aEventBlock), .pm1b_evt = gas_io(&AcpiGbl_FADT.XPm1bEventBlock),
        .pm1a_cnt = gas_io(&AcpiGbl_FADT.XPm1aControlBlock), .pm1b_cnt = gas_io(&AcpiGbl_FADT.XPm1bControlBlock),
        .pm2_cnt = gas_io(&AcpiGbl_FADT.XPm2ControlBlock), .pm_tmr = gas_io(&AcpiGbl_FADT.XPmTimerBlock),
        .gpe0 = gas_io(&AcpiGbl_FADT.XGpe0Block), .gpe1 = gas_io(&AcpiGbl_FADT.XGpe1Block),
        .smi_cmd = AcpiGbl_FADT.SmiCommand <= 0xffff ? (uint16_t)AcpiGbl_FADT.SmiCommand : 0,
        .pm1_evt_len = AcpiGbl_FADT.Pm1EventLength, .pm1_cnt_len = AcpiGbl_FADT.Pm1ControlLength,
        .pm2_cnt_len = AcpiGbl_FADT.Pm2ControlLength, .pm_tmr_len = AcpiGbl_FADT.PmTimerLength,
        .gpe0_len = AcpiGbl_FADT.Gpe0BlockLength, .gpe1_len = AcpiGbl_FADT.Gpe1BlockLength};
    pm_blocks = b; pm_blocks_valid = b.pm1a_cnt != 0;
}
int acpi_pm_blocks(struct apm_pm_blocks *out)
{
    if (!out || !pm_blocks_valid) return -2; /* ENOENT */
    *out = pm_blocks; return 0;
}
/* fx-acpioff: \_S5 as the loaded tables evaluate it (AcpiGetSleepTypeData:
 * one encoded integer or two or more integer elements), captured right
 * after the load in any inventory mode and before ownership is decided, so
 * the S5 register fallback (core/power_acpi_s5.c) can use it when its own
 * walker finds no Name(\_S5, Package) in the DSDT or an SSDT. Budgeted
 * like every other inventory stage; a failure leaves no hint. */
static uint8_t s5_hint_a, s5_hint_b;
static bool s5_hint_valid;
void fw_s5_hint_capture(void)
{
    UINT8 a = 0xff, b = 0xff;
    fw_budget_begin(100000, 250);
    ACPI_STATUS s = AcpiGetSleepTypeData(ACPI_STATE_S5, &a, &b);
    if (fw_error()) s = fw_error();
    fw_budget_end();
    s5_hint_valid = !s && a <= 7 && b <= 7;
    if (s5_hint_valid) { s5_hint_a = a; s5_hint_b = b; }
}
int acpi_s5_hint(uint8_t *typa, uint8_t *typb)
{
    if (!typa || !typb || !s5_hint_valid) return -2; /* ENOENT */
    *typa = s5_hint_a; *typb = s5_hint_b; return 0;
}
ACPI_STATUS fw_power_collect(ACPI_HANDLE h, uint32_t caps, uint32_t id, bool power_button, bool sleep_button)
{
    ACPI_STATUS s = AE_OK;
    if (power_button) {
        if (button_count == FW_MAX) return AE_LIMIT;
        power_buttons[button_count++] = h;
    }
    if (sleep_button) {
        if (sleep_button_count == FW_MAX) return AE_LIMIT;
        sleep_buttons[sleep_button_count++] = h;
    }
    if (caps & CIUKI_FW_CAP_EC) s = fw_ec_add(h);
    if (!s && (caps & CIUKI_FW_CAP_THERMAL)) s = fw_thermal_add(h, id);
    if (!s) s = fw_sources_add(h, caps, id);
    return s;
}
ACPI_STATUS fw_power_takeover(void)
{
    ACPI_STATUS s = fw_thermal_refresh(false);
    fw_sources_refresh();
    if (s) return s;
    if (AcpiGbl_ReducedHardware || !fw_os->claim || !fw_os->sci || !fw_os->unsci) return AE_SUPPORT;
    if (AcpiGbl_FADT.Pm1ControlLength != 2 || AcpiGbl_FADT.Pm1EventLength != 4 ||
        AcpiGbl_FADT.Gpe0BlockLength & 1 || AcpiGbl_FADT.Gpe1BlockLength & 1) return AE_SUPPORT;
    /* Exactly one ACPI/APM provider: reserve before the PM/SCI claims and any
     * ownership write (EC leases from collection are released on refusal). */
    if (fw_os->reserve) {
        if (fw_os->reserve()) return AE_ALREADY_ACQUIRED;
        provider_reserved = true;
    }
    s = gas_claim(&AcpiGbl_FADT.XPm1aControlBlock, AcpiGbl_FADT.Pm1ControlLength, true);
    if (!s) s = gas_claim(&AcpiGbl_FADT.XPm1bControlBlock, AcpiGbl_FADT.Pm1ControlLength, false);
    if (!s) s = gas_claim(&AcpiGbl_FADT.XPm1aEventBlock, AcpiGbl_FADT.Pm1EventLength, true);
    if (!s) s = gas_claim(&AcpiGbl_FADT.XPm1bEventBlock, AcpiGbl_FADT.Pm1EventLength, false);
    if (!s) s = gas_claim(&AcpiGbl_FADT.XGpe0Block, AcpiGbl_FADT.Gpe0BlockLength, false);
    if (!s) s = gas_claim(&AcpiGbl_FADT.XGpe1Block, AcpiGbl_FADT.Gpe1BlockLength, false);
    if (!s && AcpiGbl_FADT.SmiCommand) s = fw_os->claim(AcpiGbl_FADT.SmiCommand, 1);
    if (s) return s;
    enable_count = 0;
    s = save_enables(&AcpiGbl_FADT.XPm1aEventBlock,AcpiGbl_FADT.Pm1EventLength);
    if (!s) s = save_enables(&AcpiGbl_FADT.XPm1bEventBlock,AcpiGbl_FADT.Pm1EventLength);
    if (!s) s = save_enables(&AcpiGbl_FADT.XGpe0Block,AcpiGbl_FADT.Gpe0BlockLength);
    if (!s) s = save_enables(&AcpiGbl_FADT.XGpe1Block,AcpiGbl_FADT.Gpe1BlockLength);
    if (s) return s;
    /* The OSL installs the qualified route before any mode-change write.
     * Until events exist, the handler returns NOT_HANDLED without touching AML. */
    s = fw_os->sci(AcpiGbl_FADT.SciInterrupt, NULL, NULL);
    if (s) return s;
    fw_writes = true;
    s = mode(true); if (s) return s;
    /* FACS writes/global-lock arbitration are intentionally unsupported.
     * Region LockRule and explicit _GL acquisition fail at a build seam. */
    AcpiGbl_UseGlobalLock = FALSE;
    fw_budget_begin(100000, 250);
    events_started = true;
    s = AcpiEnableSubsystem(ACPI_NO_ACPI_ENABLE | ACPI_NO_FACS_INIT | ACPI_NO_HANDLER_INIT);
    if (fw_error()) s = fw_error(); fw_budget_end();
    if (!s) s = AcpiEvInstallSciHandler();
    if (!s) s = AcpiInstallNotifyHandler(ACPI_ROOT_OBJECT, ACPI_ALL_NOTIFY, notify, NULL);
    if (!s && !(AcpiGbl_FADT.Flags & ACPI_FADT_POWER_BUTTON)) s = AcpiInstallFixedEventHandler(ACPI_EVENT_POWER_BUTTON, button, NULL);
    /* Both button forms use the policy owner; no sleep in SCI context. */
    if (!s && !(AcpiGbl_FADT.Flags & ACPI_FADT_SLEEP_BUTTON)) s = AcpiInstallFixedEventHandler(ACPI_EVENT_SLEEP_BUTTON, button, (void *)1);
    if (!s) {
        fw_budget_begin(100000, 250);
        s = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
        if (fw_error()) s = fw_error(); fw_budget_end();
    }
    fw_owned = !s;
    if (!s) s = fw_ec_enable();
    if (!s) s = fw_thermal_refresh(true);
    if (!s) fw_sources_refresh();
    if (!s) s = AcpiUpdateAllGpes();
    if (s) { fw_owned = false; return s; }
    UINT8 a, b;
    fw_budget_begin(100000, 250);
    s5_available = !AcpiGetSleepTypeData(ACPI_STATE_S5, &a, &b) && !fw_error() && a <= 7 && b <= 7;
    fw_budget_end();
    if (provider_reserved && fw_os->activate && fw_os->activate()) { fw_owned = false; return AE_ALREADY_ACQUIRED; }
    fw_power.ownership = CIUKI_POWER_OWNED; fw_power.error = 0;
    refresh_needed = true; fw_power.sequence++; fw_power_publish(); return AE_OK;
}
void fw_power_stop(void)
{
    /* An incoming SCI_EN=1 is not proof of firmware thermal ownership. A
     * failed OS backend cannot advertise a safe legacy hand-back in that case. */
    bool safe = fw_ec_idle() && !(mode_checked && original_acpi);
    fw_owned = false;
    fw_budget_begin(100000, 250);
    if (events_started && AcpiDisableAllGpes()) safe = false;
    if (events_started && AcpiDisableEvent(ACPI_EVENT_POWER_BUTTON, 0)) safe = false;
    if (events_started && AcpiDisableEvent(ACPI_EVENT_SLEEP_BUTTON, 0)) safe = false;
    fw_budget_end();
    if (fw_os->unsci && fw_os->unsci(AcpiGbl_FADT.SciInterrupt, NULL)) safe = false;
    fw_budget_begin(100000, 3500);
    if (changed_mode && mode(false)) safe = false;
    fw_budget_end();
    if (fw_writes && safe) for (unsigned i = 0; i < enable_count; i++) {
        struct saved_enable *e = &enables[i]; UINT64 value = 0;
        if (fw_os->write(0,e->address,8,e->value,NULL) || fw_os->access(0,e->address,8,&value,NULL) || value != e->value) safe = false;
    }
    fw_writes = false; s5_available = false;
    /* Dropping queued ACPICA objects before termination would leak their
     * callback payloads. Drain a finite admitted queue before cleanup. */
    fw_budget_begin(100000, 250); fw_queue_drain(16); fw_budget_end();
    if (fw_os->finish) fw_os->finish(safe);
    fw_power.ownership = safe ? CIUKI_POWER_FIRMWARE : CIUKI_POWER_QUARANTINED;
    fw_power.ac_online = fw_power.lid_open = CIUKI_POWER_UNKNOWN;
    for (unsigned i = 0; i < fw_power.battery_count; i++) {
        uint32_t id = fw_power.batteries[i].id;
        memset(&fw_power.batteries[i],0xff,sizeof(fw_power.batteries[i])); fw_power.batteries[i].id = id;
    }
    for (unsigned i = 0; i < fw_power.zone_count; i++) {
        fw_power.zones[i].temperature = fw_power.zones[i].cooling = CIUKI_POWER_UNKNOWN;
        fw_power.zones[i].passive_delta = INT32_MIN;
    }
    /* A quarantined owner keeps the reservation: APM must never take over
     * after a partial or uncertain ACPI ownership. */
    if (provider_reserved && fw_os->retire) fw_os->retire(!safe);
    provider_reserved = false;
    fw_power_publish();
    changed_mode = events_started = false;
}
static int enter_s5(void)
{
    UINT8 a, b;
    if (!fw_owned || !s5_available) return -95;
    fw_budget_begin(100000, 250);
    ACPI_STATUS s = AcpiGetSleepTypeData(ACPI_STATE_S5, &a, &b);
    if (fw_error()) s = fw_error(); fw_budget_end();
    if (s || a > 7 || b > 7) return -5;
    ACPI_OBJECT arg = {.Type = ACPI_TYPE_INTEGER}; arg.Integer.Value = 5;
    ACPI_OBJECT_LIST args = {1, &arg};
    s = fw_eval(ACPI_ROOT_OBJECT, "_PTS", &args, NULL);
    if (s && s != AE_NOT_FOUND) { fw_power.error = s; return -5; }
    /* ACPICA 20260930 hwsleep.c: clear wake/status and remove runtime
     * GPE enables before the sleep-control writes. No unqualified wake
     * policy is enabled for S5. The physical power button remains firmware's
     * platform wake/override mechanism. */
    fw_budget_begin(100000,250);
    s = AcpiWriteBitRegister(ACPI_BITREG_WAKE_STATUS,ACPI_CLEAR_STATUS);
    if (!s) s = AcpiDisableAllGpes();
    if (!s) s = AcpiHwClearAcpiStatus();
    if (fw_error()) s = fw_error();
    fw_budget_end();
    if (s) { fw_power.error = s; return -5; }
    /* S5 has no resume path. Preserve every non-sleep PM1 control bit. */
    UINT64 ca = 0, cb = 0;
    const ACPI_GENERIC_ADDRESS *ga = &AcpiGbl_FADT.XPm1aControlBlock, *gb = &AcpiGbl_FADT.XPm1bControlBlock;
    uint64_t pa, pb; memcpy(&pa, &ga->Address, sizeof(pa)); memcpy(&pb, &gb->Address, sizeof(pb));
    s = fw_os->access(0, pa, 16, &ca, NULL);
    if (!s && pb) s = fw_os->access(0, pb, 16, &cb, NULL);
    ca = (ca & ~0x3c00u) | ((UINT64)a << 10); cb = (cb & ~0x3c00u) | ((UINT64)b << 10);
    if (!s) s = fw_os->write(0, pa, 16, ca, NULL);
    if (!s && pb) s = fw_os->write(0, pb, 16, cb, NULL);
    if (!s) s = fw_os->write(0, pa, 16, ca | 0x2000, NULL);
    if (!s && pb) s = fw_os->write(0, pb, 16, cb | 0x2000, NULL);
    if (!s) fw_os->pause(2000);
    fw_power.error = s ? s : AE_NO_HARDWARE_RESPONSE;
    return -5;
}
bool acpi_power_can_off(void) { return fw_owned && s5_available; }
int acpi_power_off_owned(void)
{
    if (!acpi_power_can_off()) return -95;
    uint32_t flags = fw_os->lock();
    if (__atomic_load_n(&off_request,__ATOMIC_RELAXED)) { fw_os->unlock(flags); return -16; }
    __atomic_store_n(&off_request,1,__ATOMIC_RELEASE); fw_os->unlock(flags);
    for (unsigned i = 0; i < 4000; i++) {
        if (__atomic_load_n(&off_request, __ATOMIC_ACQUIRE) == 2) {
            int result = off_result; __atomic_store_n(&off_request,0,__ATOMIC_RELEASE); return result;
        }
        fw_os->pause(1);
    }
    /* The request stays pinned: no second caller can overlap uncertain S5. */
    return -110;
}
int acpi_power_snapshot(struct ciuki_power_status *out)
{
    if (!out) return -22;
    if (!published.version) {
        memset(out, 0xff, sizeof(*out)); out->version = 1; out->size = sizeof(*out);
        out->ownership = CIUKI_POWER_PENDING; out->battery_count = out->zone_count = 0;
        out->shutdown_requests = out->sequence = out->event_overflows = out->reserved = 0; return 0;
    }
    if (!fw_os) { *out = published; return 0; }
    uint32_t flags = fw_os->lock(); *out = published; fw_os->unlock(flags); return 0;
}
void fw_power_poll(void)
{
    if (!fw_owned) return;
    ACPI_STATUS route = fw_os->sci(AcpiGbl_FADT.SciInterrupt,NULL,NULL);
    if (route) { fw_power.error = route; fw_power_request(); fw_power_stop(); return; }
    if (__atomic_load_n(&off_request, __ATOMIC_ACQUIRE) == 1) {
        off_result = enter_s5(); fw_power_publish(); __atomic_store_n(&off_request, 2, __ATOMIC_RELEASE); return;
    }
    fw_budget_begin(100000, 250); fw_queue_drain(16);
    ACPI_STATUS event_error = fw_error(); fw_budget_end();
    if (event_error) { fw_power.error = event_error; fw_power_request(); fw_power_stop(); return; }
    fw_ec_poll();
    if (fw_ec_faulted()) { fw_power.error = AE_TIME; fw_power_request(); fw_power_stop(); return; }
    /* fx-3 follow-up 2: no fixed 100 ms thermal read; zones are read when
     * their _TZP period is due or they were notified (thermal.c). */
    uint64_t now = fw_os->clock();
    bool thermal = fw_thermal_pending(now);
    if (refresh_needed || thermal || now >= sources_at) {
        bool sources = refresh_needed || now >= sources_at;
        refresh_needed = false;
        ACPI_STATUS s = thermal ? fw_thermal_service(now) : AE_OK;
        if (sources) { sources_at = fw_os->clock() + 10000000; fw_sources_refresh(); }
        fw_power.sequence++;
        if (s) { fw_power.error = s; fw_power_request(); fw_power_stop(); }
        else fw_power_publish();
    }
}
