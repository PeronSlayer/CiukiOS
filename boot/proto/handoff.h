#ifndef HANDOFF_H
#define HANDOFF_H

#include <stdint.h>

#define HANDOFF_V0_MAGIC 0x30464F484B554943ULL /* "CIUKHOF0" */
#define HANDOFF_V0_VERSION 0ULL

typedef struct handoff_v0 {
    uint64_t magic;
    uint64_t version;
    uint64_t stage2_load_addr;
    uint64_t stage2_size;
    uint64_t flags;
    uint64_t com_phys_base;   /* physical address of loaded COM binary, 0 if none */
    uint64_t com_phys_size;   /* size in bytes of loaded COM binary, 0 if none */
} handoff_v0_t;

#endif

