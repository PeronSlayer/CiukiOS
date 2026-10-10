/* Safe-mode evidence consumes boot state; it never enables a device.
 * CBI1 options preserve BOOT.CFG text, but there is no separate menu-source
 * bit. A safe flag with neither positive config nor QEMU selector therefore
 * identifies menu by elimination; report that inference explicitly.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/init.h>
#include <ciuki/registry.h>
#include <ciuki/i8042.h>
#include <ciuki/fwinput.h>
#include <ciuki/fbdev.h>
#include <ciuki/probe.h>

static bool safe_option(const char *text, unsigned length)
{
    for (unsigned i = 0; i + 6 <= length && text[i]; i++) {
        if (i && text[i - 1] != ' ' && text[i - 1] != '\t' && text[i - 1] != '\n' && text[i - 1] != '\r') continue;
        if (!strncmp(text + i, "safe=1", 6) &&
            (i + 6 == length || !text[i + 6] || text[i + 6] == ' ' || text[i + 6] == '\t' ||
             text[i + 6] == '\n' || text[i + 6] == '\r')) return true;
    }
    return false;
}

int probe_safe(void)
{
    rec_emit("safe", "BEGIN", 0);
    struct drivers_state state;
    drivers_snapshot(&state);
    bool fw_cfg = (g_boot.flags & (CBI_F_SMBIOS_QEMU | CBI_F_TEST_REQUEST)) ==
                  (CBI_F_SMBIOS_QEMU | CBI_F_TEST_REQUEST) &&
                  g_boot.test_request_len <= CIUKI_TEST_REQ_MAX &&
                  safe_option(g_boot.test_request, g_boot.test_request_len);
    bool config = safe_option(g_boot.options, sizeof(g_boot.options));
    bool menu = state.safe && !fw_cfg && !config;
    rec_emit("safe", "DATA", "case=option safe_mode=%u fw_cfg=%u boot_cfg=%u menu=%u menu_inferred=%u boot_flags=%08x",
             state.safe, fw_cfg, config, menu, menu, state.boot_flags);
    bool ordered = drivers_activation_ordered();
    rec_emit("safe", "DATA", "case=activation flag_sequence=%u framebuffer_sequence=%u input_sequence=%u ata_sequence=%u flag_before_activation=%u optional_activations=%u",
             state.flag_sequence, state.fb_sequence, state.input_sequence, state.ata_sequence,
             ordered, state.optional_activations);
    static const char *disabled[] = { "audio", "acceleration", "network", "dma", "power", "optional_firmware", "ata" };
    for (unsigned i = 0; i < ARRAY_SIZE(disabled); i++)
        rec_emit("safe", "DATA", "case=disabled device=%s reason=safe_mode", disabled[i]);
    bool no_lfb = (g_boot.flags & CBI_F_TEXT_MODE) || !g_boot.fb_phys;
    if (no_lfb) rec_emit("safe", "DATA", "case=disabled device=framebuffer reason=no_lfb console=text");
    unsigned owners = 0;
    for (unsigned i = 0; i < registry_count(); i++) {
        const struct resource *r = registry_get(i);
        if (r->state != RS_ACTIVE) continue;
        owners++;
        rec_emit("safe", "DATA", "case=active owner=%s generation=%u type=%u start=%08x end=%08x",
                 r->owner, r->generation, r->type, r->start, r->end);
    }
    struct i8042_stats input;
    i8042_snapshot(&input);
    bool input_ok = input.active && !input.quarantined && !state.input_error;
    if (state.firmware) {
        struct fwinput_backend_state backend;
        fwinput_backend_state(&backend);
        input_ok = input_ok && backend.keyboard && backend.key_releases && !backend.disabled;
    }
    bool console = no_lfb || (fbdev_get()->present && !state.fb_error);
    /* Exercise the selected console sink even when there is no disk/LFB. */
    console_write("[safe] Console ready.\n", sizeof("[safe] Console ready.\n") - 1);
    rec_emit("safe", "DATA", "case=required console=%u console_mode=%s input=%u backend=%s active_resources=%u disk_log=unavailable",
             console, no_lfb ? "text" : "lfb", input_ok, state.firmware ? "firmware" : "native", owners);
    bool ok = state.safe && (g_boot.flags & CBI_F_SAFE_MODE) && ordered &&
              !state.optional_activations && !state.ata_called && console && input_ok;
    rec_emit("safe", "DATA", "group=safe safe_mode=%u flag_before_activation=%u optional_activations=%u input_works=%u console_works=%u bios_retries=0 source=%s",
             state.safe, ordered, state.optional_activations, input_ok, console,
             fw_cfg ? "fw-cfg" : config ? "boot-cfg" : menu ? "menu" : "none");
    rec_emit("safe", "DATA", "group=metadata subcase=required owner=%s generation=%u errors=%u gate=safe timing_domain=%s",
             state.firmware ? "firmware-input" : "i8042", input.generation, !ok,
             (g_boot.flags & CBI_F_SMBIOS_QEMU) ? "icount" : "hardware");
    rec_emit("safe", "READY", "console=%u input=%u optional_activations=%u", console, input_ok, state.optional_activations);
    rec_emit("safe", "END", "status=%s reason=%s", ok ? "PASS" : "FAIL", ok ? "required_devices" : "safe_contract");
    return ok ? 0 : 1;
}
CIUKI_F1_PROBE("safe", probe_safe);

/* Keep the final two registrations in the F1 acceptance-table order. */
int probe_bootlog(void);
CIUKI_F1_PROBE("bootlog", probe_bootlog);
