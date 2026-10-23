/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_CALLBACK_H
#define CIUKI_NDIS_CALLBACK_H
#include "ntimage.h"
#include <ciuki/registry.h>
#define NDIS_CALLBACK_DEPTH 8u
#define NDIS_CALLBACK_CLAIMS 3u
#define NDIS_CALLBACK_LOCKS 16u
enum ndis_callback_state { NDIS_CALLBACK_IDLE, NDIS_CALLBACK_ACTIVE, NDIS_CALLBACK_QUARANTINED };
struct ndis_callback_ops {
    void *context;
    /* Independent host code only: mask selected source, revoke PCI BM with
     * readback, drain IRQ/DPC, prove engine idle. Never calls foreign code.
     * False retains all image/DMA/context pins permanently. */
    bool (*fence)(void *,const char *);
    void (*record)(void *,const char *event,const char *reason,uint8_t irql);
    void (*fault_record)(void *,const char *driver,uint32_t vector,uint32_t offset,bool in_shim);
};
struct ndis_callback_frame { uint8_t previous, level; uint64_t deadline; unsigned locks; };
struct ndis_callback {
    struct ntimage *image;
    const char *owner; /* pinned registry owner; required by native PCI fence */
    struct ndis_callback_ops ops;
    struct ndis_callback_frame stack[NDIS_CALLBACK_DEPTH];
    int claims[NDIS_CALLBACK_CLAIMS];
    gen_t generations[NDIS_CALLBACK_CLAIMS];
    unsigned claim_count, depth;
    enum ndis_callback_state state;
    uint8_t irql, dirql;
    bool fence_idle;
    /* Only registered, owned, pinned lock words; never arbitrary pointers. */
    struct { uint32_t *word; uint8_t previous; bool dpc; } locks[NDIS_CALLBACK_LOCKS];
    unsigned lock_count;
    uint32_t fault_vector, fault_offset;
    bool fault_in_shim, timeout_pending;
};
/* Host-testable transitions shared with execution.c. Production ntimage_enter
 * remains disabled until the independent recovery prerequisites are reviewed. */
int ndis_callback_begin(struct ndis_callback *,uint32_t address,uint8_t level,uint64_t now,uint64_t budget);
int ndis_callback_end(struct ndis_callback *);
void ndis_callback_fault(struct ndis_callback *,const char *reason);
void ndis_callback_watchdog(struct ndis_callback *,uint64_t now);
bool ndis_callback_expired(const struct ndis_callback *,uint64_t now);
int ndis_irql_raise(struct ndis_callback *,uint8_t level,uint8_t *old);
int ndis_irql_lower(struct ndis_callback *,uint8_t level);
int ndis_irql_wait(struct ndis_callback *);
/* Called inside a marked shim, with owned resident storage already validated. */
int ndis_spin_acquire(struct ndis_callback *,uint32_t *owned_pinned_word,bool dpc,uint8_t *old);
int ndis_spin_release(struct ndis_callback *,uint32_t *owned_pinned_word,bool dpc,uint8_t old);
/* Synthetic host-only thunk: deliberately absent from production objects. */
#ifdef CIUKI_NDIS_HOST_MODEL
typedef int32_t (NDIS_STDCALL *ndis_callback2)(uint32_t,uint32_t);
int ndis_callback_model_call2(struct ndis_callback *,uint32_t address,ndis_callback2,
                            uint32_t,uint32_t,uint8_t,uint64_t,uint64_t,int32_t *);
#endif
#endif
