/* F1 activation order: framebuffer, one input owner, then native ATA.
 * The copied loader flag is observed before the first driver call. An LFB
 * in safe mode is the required console, not an optional display takeover.
 * No error authorizes another backend or a retry of quarantined hardware.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/init.h>
#include <ciuki/fbdev.h>
#include <ciuki/i8042.h>
#include <ciuki/ata.h>
#include <ciuki/biosvm.h>
#include <ciuki/registry.h>

static struct drivers_state state;
static struct activation_entry activation_ledger[ACTIVATION_LEDGER_MAX];
static unsigned activation_count;

void drivers_snapshot(struct drivers_state *out) { if (out) *out = state; }
unsigned drivers_activation_count(void) { return activation_count; }
const struct activation_entry *drivers_activation_get(unsigned index)
{
    return index < activation_count ? &activation_ledger[index] : 0;
}

bool drivers_activation_ordered(void)
{
    if (!state.initialized || activation_count != 3 || state.flag_sequence != 1) return false;
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
    klog("[init] %s result=%s error=%d reason=%s activation_seq=%u tick=%llu safe_flag_before=%u",
         entry.device, entry.result, entry.error, entry.reason, entry.seq, entry.tick, entry.safe_flag_before);
}

void drivers_init(void)
{
    if (state.initialized) return;
    state.initialized = true;        /* one attempt, including failed activation */
    state.boot_flags = g_boot.flags;
    state.safe = !!(state.boot_flags & CBI_F_SAFE_MODE);
    state.firmware = g_boot.input_policy == CBI_INPUT_FIRMWARE ||
                     !!(state.boot_flags & CBI_F_INPUT_FORCED);
    state.flag_sequence = 1;
    klog("[init] flag safe_mode=%u boot_flags=%08x activation_seq=%u",
         state.safe, state.boot_flags, state.flag_sequence);

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

    state.input_sequence = 3;
    tick = g_ticks;
    safe_flag_before = !!(g_boot.flags & CBI_F_SAFE_MODE);
    if (state.firmware) {
        state.input_error = biosvm_init();
        if (!state.input_error) state.input_error = fwinput_adapter_init();
    } else {
        state.input_error = i8042_init();
    }
    activation_add((struct activation_entry){ .seq = state.input_sequence, .device = "input",
        .result = state.input_error ? "failed" : "ready", .reason = state.firmware ? "firmware" : "native",
        .error = state.input_error, .tick = tick, .safe_flag_before = safe_flag_before, .required = true });

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
}
