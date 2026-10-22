/* f1-94: SMI attribution around long interrupt-off sections, and the one
 * ICH SMI source the OS owns once the UHCI legacy handoff is complete.
 * Register map: Intel 82801CAM ICH3-M datasheet 290716-001, sections
 * 9.8.3.14 (SMI_EN), 9.8.3.15 (SMI_STS), 11.1.16 (USB_LEGKEY), table 5-37.
 * Record format and per-bit decisions: docs/validation/f1-94-irqoff-blackout.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SMI_H
#define CIUKI_SMI_H
#include <stdint.h>
#include <stdbool.h>

/* SMI_EN, PMBASE+30h. */
#define SMI_EN_GBL         (1u << 0)
#define SMI_EN_EOS         (1u << 1)
#define SMI_EN_BIOS        (1u << 2)
#define SMI_EN_LEGACY_USB  (1u << 3)
#define SMI_EN_SLP         (1u << 4)
#define SMI_EN_APMC        (1u << 5)
#define SMI_EN_SWSMI_TMR   (1u << 6)
#define SMI_EN_BIOS_RLS    (1u << 7)   /* write-only, reads 0 */
#define SMI_EN_MCSMI       (1u << 11)
#define SMI_EN_TCO         (1u << 13)
#define SMI_EN_PERIODIC    (1u << 14)
/* SMI_STS, PMBASE+34h. Bits 8-10, 12, 15 and 16 have no SMI_EN enable. */
#define SMI_STS_BIOS       (1u << 2)
#define SMI_STS_LEGACY_USB (1u << 3)
#define SMI_STS_SLP        (1u << 4)
#define SMI_STS_APM        (1u << 5)
#define SMI_STS_SWSMI_TMR  (1u << 6)
#define SMI_STS_PM1        (1u << 8)
#define SMI_STS_GPE0       (1u << 9)
#define SMI_STS_GPE1       (1u << 10)
#define SMI_STS_MCSMI      (1u << 11)
#define SMI_STS_DEVMON     (1u << 12)
#define SMI_STS_TCO        (1u << 13)
#define SMI_STS_PERIODIC   (1u << 14)
#define SMI_STS_SERIRQ     (1u << 15)
#define SMI_STS_SMBUS      (1u << 16)
/* USB_LEGKEY trap/SMI enables (bits 0-4, 7) and A20 pass-through (5). */
#define SMI_LEGKEY_ARMED   0x00bfu

/* A blackout is at least this long; R1 bounds software IF-off work at 100 us. */
#define SMI_GAP_MIN_US     1000u

struct smi_chipset {
    uint32_t lpc_id;           /* vendor | device << 16, LPC bridge D31:F0 */
    const char *name;
    uint32_t en_rw;            /* defined read/write SMI_EN bits */
    uint32_t sts_defined;      /* defined SMI_STS bits */
    uint8_t uhci_functions;    /* D29 functions that carry USB_LEGKEY */
    uint16_t uhci_device[3];   /* their device ids, vendor 8086 */
    bool qualified;            /* datasheet and hardware evidence: one write allowed */
};
/* Null for every bridge without a checked register map. */
const struct smi_chipset *smi_chipset(uint32_t lpc_id);
/* Status bits that can have raised an SMI under this SMI_EN value. */
uint32_t smi_causal_status(const struct smi_chipset *, uint32_t en);

enum smi_reason { SMI_PLAN_CLEAR, SMI_PLAN_UNQUALIFIED, SMI_PLAN_DIAGNOSTIC,
    SMI_PLAN_GLOBAL_OFF, SMI_PLAN_NOT_ARMED, SMI_PLAN_USB_INCOMPLETE,
    SMI_PLAN_USB_NOT_OWNED, SMI_PLAN_LEGKEY_ARMED };
struct smi_plan { uint32_t clear, kept; enum smi_reason reason; };
/* Pure decision. Only LEGACY_USB_EN can ever be in clear; kept lists the
 * enabled sources left to the firmware. */
struct smi_plan smi_mitigation_plan(const struct smi_chipset *, uint32_t en,
    unsigned uhci_present, unsigned uhci_owned, const uint16_t legkey[3], bool allowed);
const char *smi_reason_name(enum smi_reason);
/* Read-modify-write value: everything as read, except the cleared enables
 * and the write-only BIOS_RLS (a 1 would raise an SCI). EOS keeps its value. */
static inline uint32_t smi_en_write_value(uint32_t read, uint32_t clear)
{ return read & ~clear & ~SMI_EN_BIOS_RLS; }

enum smi_class { SMI_CLASS_NONE, SMI_CLASS_STATUS, SMI_CLASS_TSC, SMI_CLASS_UNPROVEN };
/* One interrupt-off section of at least 1 ms, raw TSC. gap is the largest
 * interval between two consecutive phase marks; bounded means that both
 * marks enclose straight-line scheduler code (no loop over input, no wait). */
struct smi_section {
    uint64_t start, end, gap, gap_start;
    bool gap_bounded;
};
struct smi_sample {
    struct smi_section section;
    bool before_valid;         /* SMI_STS sampled before section start */
    uint32_t before, after, en;
};
/* Status rise in a causal bit first, then the TSC rule: a bounded gap of at
 * least 1 ms that is longer than all other work of the section. */
enum smi_class smi_classify(const struct smi_chipset *, const struct smi_sample *,
                            uint64_t per_ms, uint32_t *sources);

/* Activities whose time relation to the blackouts is recorded. */
enum smi_suspect { SMI_HID_BEGIN, SMI_HID_END, SMI_USB_SCAN, SMI_EC_IO };

/* Kernel hooks (core/smi.c). Weak: host builds without the module skip them. */
void smi_attribution_init(void) __attribute__((weak));
void smi_tick(uint64_t now, uint64_t previous) __attribute__((weak));
void smi_section_end(const struct smi_section *) __attribute__((weak));
void smi_adjacent_gap(uint64_t start, uint64_t end) __attribute__((weak));
void smi_mark(enum smi_suspect) __attribute__((weak));
/* Task context, once per cpustat window; verbose also prints when the
 * chipset sampler is unsupported (timer gaps and sections only). */
void smi_window_emit(uint64_t window_cycles, bool verbose) __attribute__((weak));
#endif
