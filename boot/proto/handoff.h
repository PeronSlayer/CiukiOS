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
} handoff_v0_t;

#endif

