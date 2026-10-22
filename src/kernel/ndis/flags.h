/* Kernel-private binary flag sites. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_FLAGS_H
#define CIUKI_NDIS_FLAGS_H
#include <stdint.h>
#include <stdbool.h>
#define NDIS_FLAG_SITES 2048u
#define NDIS_RELOC_OPERANDS 8192u
struct pe_image;
struct ndis_user_host;
struct trap_frame;
struct ndis_flag_site { uint32_t rva; uint8_t opcode, width, prefix, reserved; };
struct ndis_flag_table {
    uint8_t image_hash[32];
    uint32_t base, size, count;
    bool sealed;
    struct ndis_flag_site site[NDIS_FLAG_SITES];
};
/* Scratch is needed only before entry and is charged to the F6 reservation. */
struct ndis_flag_workspace { struct { uint32_t rva, bytes; } operand[NDIS_RELOC_OPERANDS]; unsigned count; };
int ndis_flags_patch(struct pe_image *,struct ndis_flag_table *,struct ndis_flag_workspace *,const uint8_t hash[32]);
struct ndis_flag_access {
    void *context;
    int (*read)(void *,uint32_t,void *,uint32_t);
    int (*write)(void *,uint32_t,const void *,uint32_t);
};
/* A failed emulation has no virtual-state change. Caller quarantines. */
int ndis_flags_breakpoint(struct ndis_user_host *,struct trap_frame *,const struct ndis_flag_access *);
#endif
