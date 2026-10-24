/* SPDX-License-Identifier: MIT
 * System Configuration state <-> call 99 and BOOT.CFG (pure: host-tested). */
#include "msconfig.h"

const char *msconfig_class_label(unsigned c)
{
    static const char *const labels[MC_CLASSES] = { "Load the accelerated display driver", "Load the sound drivers",
        "Load the network drivers", "Load the USB drivers", "Load the serial, parallel, floppy and game ports",
        "Load power management (APM and ACPI)", "Run the startup programs" };
    return c < MC_CLASSES ? labels[c] : "";
}
void msconfig_from_status(const struct ciuki_recovery_status *s, const char *bootcfg, size_t n, struct msconfig_state *out)
{
    out->enabled = (uint8_t)(CIUKI_RECOVERY_CLASS_MASK & ~(s ? s->disabled : 0));
    out->mode = s && (s->request & CIUKI_RECOVERY_REQ_MENU) ? MC_DIAGNOSTIC : s && s->disabled ? MC_SELECTIVE : MC_NORMAL;
    bootcfg_read(bootcfg ? bootcfg : "", bootcfg ? n : 0, &out->advanced);
}
void msconfig_to_request(const struct msconfig_state *st, uint8_t current, uint8_t *disabled, uint8_t *request)
{
    *disabled = st->mode == MC_SELECTIVE ? (uint8_t)(CIUKI_RECOVERY_CLASS_MASK & ~st->enabled) : 0;
    *request = (uint8_t)((current & (CIUKI_RECOVERY_REQ_SCANDISK | CIUKI_RECOVERY_REQ_THOROUGH)) |
                         (st->mode == MC_DIAGNOSTIC ? CIUKI_RECOVERY_REQ_MENU : 0));
}
