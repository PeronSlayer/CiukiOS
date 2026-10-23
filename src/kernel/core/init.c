/* F1 activation order: framebuffer, one input owner, then native ATA.
 * The copied loader flag is observed before the first driver call. An LFB
 * in safe mode is the required console, not an optional display takeover.
 * Firmware input failure permits one native attempt with normal lease checks.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/clock.h>
#include <ciuki/init.h>
#include <ciuki/fbdev.h>
#include <ciuki/i8042.h>
#include <ciuki/ata.h>
#include <ciuki/biosvm.h>
#include <ciuki/registry.h>
#include <ciuki/storage.h>
#include <ciuki/audio.h>
#include <ciuki/net.h>
#include <ciuki/usb.h>
#include <ciuki/entropy.h>
#include <ciuki/splash.h>
#include <ciuki/smi.h>
#include <ciuki/com.h>
#include <ciuki/joystick.h>
#include <ciuki/joystick_settings.h>
#pragma weak joystick_storage_init
#pragma weak gameport_init
#pragma weak com_init
#include <ciuki/lpt.h>
#pragma weak lpt_init
#include <ciuki/firmware.h>
#pragma weak firmware_inventory_start
#include "../drivers/floppy.h"
#pragma weak floppy_init
extern void apm_start(void) __attribute__((weak));
extern int power_platform_start(void) __attribute__((weak));
#pragma weak entropy_start
#pragma weak console_splash_phase
#pragma weak usb_init
#pragma weak audio_init
#pragma weak net_settings_load
extern void net_settings_load(struct vfs *);
#pragma weak net_init
#pragma weak fbdev_accel_schedule

static struct drivers_state state;
static struct activation_entry activation_ledger[ACTIVATION_LEDGER_MAX];
static unsigned activation_count;

/* f0-55: the boot splash bar follows these records (ciuki/splash.h). */
static void splash_report(enum splash_phase phase) { if (console_splash_phase) console_splash_phase(phase); }

void drivers_snapshot(struct drivers_state *out) { if (out) *out = state; }
bool drivers_input_firmware(void)
{
    if (state.initialized) return state.firmware;
    return !!(g_boot.flags & (CBI_F_INPUT_FORCED | CBI_F_INPUT_DETECTED | CBI_F_SAFE_MODE));
}
unsigned drivers_activation_count(void) { return activation_count; }
const struct activation_entry *drivers_activation_get(unsigned index)
{
    return index < activation_count ? &activation_ledger[index] : 0;
}

bool drivers_activation_ordered(void)
{
    if (!state.initialized || activation_count < 3 || state.flag_sequence != 1) return false;
    uint32_t seq = state.flag_sequence;
    uint64_t tick = 0;
    for (unsigned i = 0; i < activation_count; i++) {
        const struct activation_entry *e = &activation_ledger[i];
        if (e->seq <= seq || e->tick < tick || e->safe_flag_before != state.safe) return false;
        seq = e->seq;
        tick = e->tick;
    }
    return activation_ledger[0].seq == state.fb_sequence &&
           activation_ledger[1].seq == state.input_sequence &&
           activation_ledger[2].seq == state.ata_sequence;
}

static void activation_add(struct activation_entry entry)
{
    /* No allocation and no evidence sink at device activation time. */
    if (activation_count < ARRAY_SIZE(activation_ledger))
        activation_ledger[activation_count++] = entry;
    if (entry.kind == ACTIVATION_DEVICE)
        klog("[init] %s result=%s error=%d reason=%s activation_seq=%u tick=%llu safe_flag_before=%u",
             entry.device, entry.result, entry.error, entry.reason, entry.seq, entry.tick, entry.safe_flag_before);
    else
        klog("[storage] kind=%u disk=%u part=%u mode=%s error=%d writes=%llu qualified=%u reason=%s activation_seq=%u",
             entry.kind, entry.disk, entry.partition, entry.readonly ? "ro" : "rw", entry.error,
             entry.writes, entry.qualified, entry.reason, entry.seq);
    int phase = splash_storage_phase(entry.kind);
    if (phase >= 0) splash_report((enum splash_phase)phase);
}

void drivers_storage_add(struct activation_entry entry)
{
    entry.seq = activation_count + 2;
    entry.tick = g_ticks;
    entry.safe_flag_before = state.safe;
    activation_add(entry);
}

const struct activation_entry *drivers_mount_get(unsigned drive)
{
    for (unsigned i = 0; i < activation_count; i++)
        if (activation_ledger[i].kind == ACTIVATION_MOUNT && activation_ledger[i].drive == drive)
            return &activation_ledger[i];
    return 0;
}

/* After USB: joystick settings and game port (f1-113b), the firmware
 * inventory, then floppy (f1-115), the APM suspend-service owner and APM
 * (f1-121), in that order: the owner must exist before APM connects. */
static void platform_devices_start(bool safe)
{
    if (joystick_storage_init && !safe) joystick_storage_init(&storage_get()->vfs);
    if (gameport_init && !safe) gameport_init();
    if (firmware_inventory_start) firmware_inventory_start();
    if (floppy_init && !safe) floppy_init();
    if (power_platform_start && !safe) (void)power_platform_start();
    if (apm_start && !safe) apm_start();
}
void drivers_init(void)
{
    if (state.initialized) return;
    if (entropy_start) entropy_start();
    state.initialized = true;        /* one attempt, including failed activation */
    state.boot_flags = g_boot.flags;
    state.safe = !!(state.boot_flags & CBI_F_SAFE_MODE);
    state.firmware = !!(state.boot_flags & (CBI_F_INPUT_FORCED | CBI_F_INPUT_DETECTED | CBI_F_SAFE_MODE));
    state.flag_sequence = 1;
    klog("[init] flag safe_mode=%u boot_flags=%08x activation_seq=%u",
         state.safe, state.boot_flags, state.flag_sequence);
    splash_report(SPLASH_PHASE_DRIVERS);
    if (g_boot.input_policy == CBI_INPUT_FIRMWARE && !state.firmware)
        klog("[init] input_policy=firmware source=unknown action=native");

    state.fb_sequence = 2;
    uint64_t tick = g_ticks;
    bool safe_flag_before = !!(g_boot.flags & CBI_F_SAFE_MODE);
    if (!(state.safe && ((g_boot.flags & CBI_F_TEXT_MODE) || !g_boot.fb_phys))) {
        state.fb_called = true;
        if (!state.safe) state.optional_activations++;
        state.fb_error = fbdev_init();
    }
    activation_add((struct activation_entry){ .seq = state.fb_sequence, .device = "framebuffer",
        .result = !state.fb_called ? "disabled" : state.fb_error ? "failed" : "ready",
        .reason = !state.fb_called ? "safe_text_console" : "boot_console", .error = state.fb_error,
        .tick = tick, .safe_flag_before = safe_flag_before, .required = state.safe });
    splash_report(SPLASH_PHASE_FRAMEBUFFER);

    state.input_sequence = 3;
    tick = g_ticks;
    safe_flag_before = !!(g_boot.flags & CBI_F_SAFE_MODE);
    if (state.firmware) {
        state.input_error = biosvm_init();
        if (!state.input_error) state.input_error = fwinput_adapter_init();
        if (state.input_error) {
            state.firmware_error = state.input_error;
            state.input_fallback = true;
            int handoff = biosvm_input_fallback();
            state.firmware = false;
            state.input_error = i8042_init();
            klog("[init] input fallback=native firmware_error=%d handoff_error=%d native_error=%d",
                 state.firmware_error, handoff, state.input_error);
        }
    } else {
        state.input_error = i8042_init();
    }
    activation_add((struct activation_entry){ .seq = state.input_sequence, .device = "input",
        .result = state.input_error ? "failed" : "ready",
        .reason = state.input_fallback ? "firmware_failed_native_fallback" : state.firmware ? "firmware" : "native",
        .error = state.input_error, .tick = tick, .safe_flag_before = safe_flag_before, .required = true });
    splash_report(SPLASH_PHASE_INPUT);

    state.ata_sequence = 4;
    tick = g_ticks;
    safe_flag_before = !!(g_boot.flags & CBI_F_SAFE_MODE);
    if (!state.safe) {
        state.ata_called = true;
        state.optional_activations++;
        state.ata_error = ata_init();
    }
    /* ata_init returns success even when discovery found no disks. Report
     * retained quarantine independently; never manufacture writable storage. */
    bool storage = false, quarantine = false;
    if (state.ata_called) {
        for (unsigned c = 0; c < ATA_CHANNELS; c++)
            for (unsigned u = 0; u < ATA_DEVICES; u++)
                if (ata_device_get(c, u)) storage = true;
        for (unsigned i = 0; i < registry_count(); i++) {
            const struct resource *r = registry_get(i);
            if (r->state == RS_QUARANTINED && !strncmp(r->owner, "ata", 3)) quarantine = true;
        }
    }
    activation_add((struct activation_entry){ .seq = state.ata_sequence, .device = "ata",
        .result = !state.ata_called ? "disabled" : (state.ata_error || quarantine) ? "failed" : storage ? "ready" : "absent",
        .reason = state.safe ? "safe_mode" : "native_discovery", .error = state.ata_error,
        .tick = tick, .safe_flag_before = safe_flag_before, .present = storage, .quarantined = quarantine });
    splash_report(SPLASH_PHASE_ATA);
    storage_init();
    file_clock_start(CIUKI_BUILD_EPOCH, true);
    klog("[init] storage done; files bootstrap");
    splash_report(SPLASH_PHASE_STORAGE);
    if (com_init && !state.safe) com_init();
    if (lpt_init && !state.safe) lpt_init();
    int native_files = files_bootstrap(&storage_get()->vfs);
    klog("[init] files result=%d; audio init", native_files);
    splash_report(SPLASH_PHASE_FILES);
    if (audio_init) audio_init(&storage_get()->vfs);
    klog("[init] audio init done");
    splash_report(SPLASH_PHASE_AUDIO);
    if (net_settings_load) net_settings_load(&storage_get()->vfs);
    if (net_init) net_init();
    if (usb_init) usb_init();
    platform_devices_start(state.safe);
    /* f1-94: after every UHCI handoff; SMI sampling and LEGACY_USB_EN. */
    if (smi_attribution_init) smi_attribution_init();
    splash_report(SPLASH_PHASE_DEVICES);
    if (native_files) klog("[files] initialization failed: %d", native_files);
    if (fbdev_accel_schedule && !(g_boot.flags & CBI_F_HW_CHECK))
        fbdev_accel_schedule(&storage_get()->vfs);
}
