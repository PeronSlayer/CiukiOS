/* F1 activation order: framebuffer, one input owner, then native ATA.
 * The copied loader flag is observed before the first driver call. An LFB
 * in safe mode is the required console, not an optional display takeover.
 * No error authorizes another backend or a retry of quarantined hardware.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/init.h>
#include <ciuki/probe.h>
#include <ciuki/fbdev.h>
#include <ciuki/i8042.h>
#include <ciuki/ata.h>
#include <ciuki/biosvm.h>
#include <ciuki/registry.h>

static struct drivers_state state;

void drivers_snapshot(struct drivers_state *out) { if (out) *out = state; }

void drivers_init(void)
{
    if (state.initialized) return;
    state.initialized = true;        /* one attempt, including failed activation */
    state.boot_flags = g_boot.flags;
    state.safe = !!(state.boot_flags & CBI_F_SAFE_MODE);
    state.firmware = g_boot.input_policy == CBI_INPUT_FIRMWARE ||
                     !!(state.boot_flags & CBI_F_INPUT_FORCED);
    state.flag_sequence = 1;
    rec_emit("boot", "DATA", "group=activation_flag activation_seq=1 safe_mode=%u boot_flags=%08x",
             state.safe, state.boot_flags);

    state.fb_sequence = 2;
    if (!(state.safe && ((g_boot.flags & CBI_F_TEXT_MODE) || !g_boot.fb_phys))) {
        state.fb_called = true;
        if (!state.safe) state.optional_activations++;
        state.fb_error = fbdev_init();
    }
    rec_emit("boot", "DATA", "group=activation device=framebuffer result=%s error=%d activation_seq=%u required=%u reason=%s",
             !state.fb_called ? "disabled" : state.fb_error ? "failed" : "ready",
             state.fb_error, state.fb_sequence, state.safe,
             !state.fb_called ? "safe_text_console" : "boot_console");

    state.input_sequence = 3;
    if (state.firmware) {
        state.input_error = biosvm_init();
        if (!state.input_error) state.input_error = fwinput_adapter_init();
    } else {
        state.input_error = i8042_init();
    }
    rec_emit("boot", "DATA", "group=activation device=input result=%s error=%d activation_seq=%u backend=%s required=1",
             state.input_error ? "failed" : "ready", state.input_error, state.input_sequence,
             state.firmware ? "firmware" : "native");

    state.ata_sequence = 4;
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
    rec_emit("boot", "DATA", "group=activation device=ata result=%s error=%d activation_seq=%u present=%u quarantined=%u reason=%s",
             !state.ata_called ? "disabled" : (state.ata_error || quarantine) ? "failed" : storage ? "ready" : "absent",
             state.ata_error, state.ata_sequence, storage, quarantine,
             state.safe ? "safe_mode" : "native_discovery");
}
