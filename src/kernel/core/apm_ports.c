/* fx-18: APM BIOS chipset port window. Sources and rules:
 * docs/design/acpi-events-power.md, "APM when the OS does not own ACPI".
 * - APM 1.2 (Intel/Microsoft, 1996) section 4: the APM BIOS performs the
 *   power management of the machine; Ciuki calls it through the real-mode
 *   interface in the contained BIOS VM (f1-121 Amendment 1).
 * - Intel 82801CAM ICH3-M datasheet 290716-001: 9.8.2 APM_CNT (B2h, a
 *   write raises an SMI when APMC_EN is set) and APM_STS (B3h); 9.8.3 the
 *   128-byte PMBASE block, PM1_CNT at +4 with SCI_EN (bit 0) and SLP_EN
 *   (bit 13).
 * - The T23 trial of e831e685: the BIOS's installation check executes
 *   IN AL,DX with DX=1004h (PM1_CNT) and TEST AL,1 (SCI_EN) at f000:4ccf.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/apm_ports.h>

static void add(struct apm_port_window *w, uint32_t first, uint32_t length, uint8_t kind)
{
    if (!first || !length || first > 0xffffu || length > 0x10000u - first || w->count >= APM_PORT_RANGES) return;
    w->ranges[w->count++] = (struct apm_port_range){(uint16_t)first, (uint16_t)(first + length - 1), kind};
}
void apm_port_window_build(struct apm_port_window *w, const struct apm_pm_blocks *f,
                           uint16_t chipset_base, uint16_t chipset_len)
{
    *w = (struct apm_port_window){0};
    add(w, 0xb2, 2, APM_PORT_APMC);
    if (f) {
        if (f->smi_cmd && f->smi_cmd != 0xb2 && f->smi_cmd != 0xb3) add(w, f->smi_cmd, 1, APM_PORT_APMC);
        add(w, f->pm1a_cnt, f->pm1_cnt_len, APM_PORT_PM1_CNT);
        add(w, f->pm1b_cnt, f->pm1_cnt_len, APM_PORT_PM1_CNT);
        add(w, f->pm1a_evt, f->pm1_evt_len, APM_PORT_PM1_EVT);
        add(w, f->pm1b_evt, f->pm1_evt_len, APM_PORT_PM1_EVT);
        add(w, f->pm2_cnt, f->pm2_cnt_len, APM_PORT_PM2_CNT);
        add(w, f->pm_tmr, f->pm_tmr_len, APM_PORT_PM_TMR);
        add(w, f->gpe0, f->gpe0_len, APM_PORT_GPE);
        add(w, f->gpe1, f->gpe1_len, APM_PORT_GPE);
        if (f->pm1a_cnt && f->pm1_cnt_len) w->source |= APM_PORT_SOURCE_FADT;
    }
    if (chipset_base && chipset_len) {
        unsigned before = w->count;
        add(w, chipset_base, chipset_len, APM_PORT_CHIPSET);
        if (w->count != before) w->source |= APM_PORT_SOURCE_CHIPSET;
    }
    add(w, 0x80, 1, APM_PORT_DELAY);
}
const char *apm_port_kind_name(uint8_t kind)
{
    static const char *const names[] = {"none", "apmc", "pm1_evt", "pm1_cnt", "pm2_cnt",
                                        "pm_tmr", "gpe", "chipset", "delay"};
    return kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "none";
}
enum apm_port_verdict apm_port_check(const struct apm_port_window *w, uint16_t port, unsigned width,
                                     bool write, uint32_t value, uint16_t function,
                                     uint8_t *kind, const char **reason)
{
    const char *why = "outside_window";
    uint8_t k = APM_PORT_NONE;
    enum apm_port_verdict v = APM_PORT_DENY;
    if (!w || (width != 1 && width != 2 && width != 4) || (uint32_t)port + width - 1 > 0xffffu) why = "width";
    else for (unsigned i = 0; i < w->count; i++) {
        const struct apm_port_range *r = &w->ranges[i];
        if (port < r->first || (uint32_t)port + width - 1 > r->last) continue;
        k = r->kind; why = "ok"; v = APM_PORT_ALLOW;
        switch (k) {
        case APM_PORT_APMC:
            if (width != 1) { v = APM_PORT_DENY; why = "apmc_width"; }
            break;
        case APM_PORT_PM1_CNT:
            if (write) {
                /* 16-bit view of the register bytes this access writes. */
                uint32_t shifted = 0, mask = 0, offset = port - r->first;
                for (unsigned b = 0; b < width; b++) {
                    unsigned at = offset + b;
                    if (at < 2) {
                        shifted |= ((value >> (8 * b)) & 0xffu) << (8 * at);
                        mask |= 0xffu << (8 * at);
                    }
                }
                if ((mask & 1u) && (shifted & 1u)) { v = APM_PORT_DENY; why = "sci_en"; }
                else if ((mask & 0x2000u) && (shifted & 0x2000u) && function != 0x5307) {
                    v = APM_PORT_DENY; why = "slp_en";
                }
            }
            break;
        case APM_PORT_PM_TMR: case APM_PORT_CHIPSET:
            if (write) { v = APM_PORT_DENY; why = "read_only"; }
            break;
        case APM_PORT_DELAY: v = APM_PORT_ABSORB; break;
        default: break;
        }
        break; /* the first containing range decides */
    }
    if (kind) *kind = k;
    if (reason) *reason = why;
    return v;
}
