/* Independent Mach64 setup-engine model shared by the f7-27 host tests and
 * the H1 GL replay. It decodes exactly the registers the backend writes.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef MACH64_3D_MODEL_H
#define MACH64_3D_MODEL_H
#include "../../src/kernel/drivers/video/mach64/mach64_3d.h"
#define M64M_VRAM (8u * 1024u * 1024u)
struct mach64_3d_model {
    bool bad_selfcheck, bad_texture_restore;
    struct mach64_3d_device device;
    struct m64_engine engine;
    uint32_t regs[1024];
    uint8_t *vram;
    uint64_t clock, writes, memory_writes, memory_reads, fragments, draws, fills, blits, reads;
    unsigned credits, locks, busy, max_group, group;
    /* Fault and hypothesis injections. */
    bool dead, fifo_error, stuck, full, frozen, drop, corrupt_guard, bad_restore;
    bool block1_off, mirror_off, wrong_tie, hang_after_draw, depth_truncate;
    unsigned full_after; /* FIFO stops draining after this many writes */
};
extern struct mach64_3d_model m64m;
void m64m_init(unsigned width, unsigned height);
void m64m_destroy(void);
void m64m_resources(struct render3d_service *, unsigned, unsigned, bool hardware, bool full);
void *m64m_alloc(void *, size_t);
void m64m_free(void *, void *);
uint64_t m64m_now(void *);
/* 2D-visible registers saved before a qualification; equal afterwards. */
void m64m_snapshot(uint32_t out[1024]);
bool m64m_restored(const uint32_t before[1024]);
#endif
