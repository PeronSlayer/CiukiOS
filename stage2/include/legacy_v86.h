/*
 * OPENGEM-044 Task B — Legacy-PM v86 host.
 *
 * Contract: see docs/opengem-044-mode-switch-split.md §3.2
 */

#ifndef CIUKIOS_STAGE2_LEGACY_V86_H
#define CIUKIOS_STAGE2_LEGACY_V86_H

#include <stdint.h>

#define LEGACY_V86_ARM_MAGIC   0xC1D39450u
#define LEGACY_V86_SENTINEL    0x0450u

#define LEGACY_V86_OK                               0
#define LEGACY_V86_ERR_NOT_ARMED                   -1
#define LEGACY_V86_ERR_BAD_INPUT                   -2

#define LEGACY_V86_FAULT_MODE_SWITCH_NOT_ARMED      0x04500001u
#define LEGACY_V86_FAULT_MODE_SWITCH_NOT_IMPLEMENTED 0x04500002u
#define LEGACY_V86_FAULT_PM32_BODY_RETURNED         0x04500003u
#define LEGACY_V86_FAULT_MODE_SWITCH_ERROR          0x0450FFFFu

typedef struct {
    uint16_t cs, ip;
    uint16_t ss, sp;
    uint16_t ds, es, fs, gs;
    uint32_t eflags;
    uint32_t reserved[6]; /* runtime ABI: EAX, EBX, ECX, EDX, ESI, EDI snapshot */
} legacy_v86_frame_t;

typedef enum {
    LEGACY_V86_EXIT_NORMAL = 0,
    LEGACY_V86_EXIT_GP_INT,
    LEGACY_V86_EXIT_HALT,
    LEGACY_V86_EXIT_FAULT,
} legacy_v86_exit_reason_t;

typedef struct {
    legacy_v86_exit_reason_t reason;
    uint8_t int_vector;
    legacy_v86_frame_t frame;
    uint32_t fault_code;
} legacy_v86_exit_t;

int legacy_v86_enter(const legacy_v86_frame_t *entry, legacy_v86_exit_t *out);

int  legacy_v86_arm(uint32_t magic);
void legacy_v86_disarm(void);
int  legacy_v86_is_armed(void);
int  legacy_v86_probe(void);

#endif /* CIUKIOS_STAGE2_LEGACY_V86_H */