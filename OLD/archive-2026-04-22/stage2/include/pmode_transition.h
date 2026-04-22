#ifndef STAGE2_PMODE_TRANSITION_H
#define STAGE2_PMODE_TRANSITION_H

#include "types.h"

typedef struct pmode_descriptor_snapshot {
    u16 limit;
    u64 base;
} pmode_descriptor_snapshot_t;

typedef struct pmode_transition_state {
    u32 magic;
    u32 version;
    pmode_descriptor_snapshot_t gdtr_pre;
    pmode_descriptor_snapshot_t idtr_pre;
    u64 intended_cr0_set;
    u64 intended_cr0_clear;
    u32 return_path_status;
    u32 reserved;
} pmode_transition_state_t;

#define PMODE_TRANSITION_MAGIC 0x4D365453U /* M6TS */
#define PMODE_TRANSITION_VERSION 2U
#define PMODE_RETURN_PATH_OK 1U
#define PMODE_RETURN_PATH_FAIL 0U

#endif
