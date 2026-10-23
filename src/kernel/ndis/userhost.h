/* ND14 CPU boundary. Kernel-owned metadata, never mapped to the miniport.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_USERHOST_H
#define CIUKI_NDIS_USERHOST_H
#include "ntimage.h"
#include <ciuki/arch.h>
#include "../arch/io_permissions.h"
#define NDIS_HOST_MAX 2u /* existing NETIF_MAX, no new network tasks */
#define NDIS_HOST_MAPS 16u
#define NDIS_HOST_QUEUE 16u
#define NDIS_HOST_DEPTH 8u
#define NDIS_HOST_TIMER_US 1000u
enum ndis_host_state { NDIS_HOST_NEW, NDIS_HOST_LIVE, NDIS_HOST_FENCING,
                       NDIS_HOST_QUARANTINED };
enum ndis_host_kind { NDIS_HOST_ISR, NDIS_HOST_DPC, NDIS_HOST_PASSIVE,
                      NDIS_HOST_LIFECYCLE, NDIS_HOST_INLINE };
enum ndis_host_memory { NDIS_HOST_BAR, NDIS_HOST_DMA };
struct ndis_host_map {
    uint32_t va, physical, bytes, generation, ticket;
    enum ndis_host_memory kind;
    bool uncached, live;
};
struct ndis_host_event {
    uint32_t token, address, argument, generation;
    enum ndis_host_kind kind;
    uint64_t deadline;
};
struct ndis_user_host;
struct ndis_native;
struct ndis_channel;
struct ndis_nt_runtime;
/* These are trusted, nonblocking native operations. All state transitions run
 * with UP IRQ exclusion. No callback here may call a driver or wait for one.
 * Map validation includes registry generation, whole-page ownership, ledger
 * ticket identity and cache-alias checks; user code cannot create grants. */
struct ndis_host_ops {
    void *context;
    bool (*owned)(void *,uint32_t generation);
    bool (*mapping)(void *,const struct ndis_host_map *);
    int (*map)(void *,const struct ndis_host_map *,bool install);
    bool (*mask)(void *,bool masked); /* verified per-function or exclusive */
    int (*cause)(void *); /* native pending-cause recheck: <0 failure */
    void (*notify)(void *);
    bool (*fence)(void *); /* false means DMA idle unproved */
    void (*detach)(void *,uint32_t old_generation);
    void (*retain)(void *); /* transfer all uncertain backing to boot owner */
    void (*cancel)(void *); /* no sleep; prevent return and cancel own syscalls */
    void (*record)(void *,const char *,uint32_t vector,uint32_t offset);
};
struct ndis_user_host {
    struct ntimage *image;
    struct ndis_host_ops ops;
    enum ndis_host_state state;
    uint32_t generation, retired_generation, next_token;
    struct arch_io_range ports[6];
    unsigned port_count;
    struct ndis_host_map mappings[NDIS_HOST_MAPS];
    struct ndis_host_event queue[NDIS_HOST_QUEUE], active[NDIS_HOST_DEPTH];
    unsigned head, count, depth;
    uint64_t irq_deadline;
    uint32_t irq_token, irq_sequence;
    uint32_t dispatcher, dispatch_stack[NDIS_HOST_DEPTH], stack_bytes;
    struct trap_frame continuation[NDIS_HOST_DEPTH];
    bool continuation_valid[NDIS_HOST_DEPTH];
    uint8_t irql, dirql;
    bool virtual_if, synchronized, irq_pending, irq_masked, idle_proved;
    bool sti_shadow, shadow_tf;
    uint8_t saved_irql[NDIS_HOST_DEPTH];
    uint32_t inline_token[NDIS_HOST_DEPTH];
    bool syscall_active, cancellation_stalled;
    uint64_t cancel_deadline;
    uint32_t fault_vector, fault_offset;
    struct ndis_native *native;
    struct ndis_channel *channel;
    bool qualified;
    struct ndis_nt_runtime *nt;
};
/* Production LIVE requires the native binding and a compiled exact profile
 * qualification. No profile is qualified by default. */
bool ndis_user_host_available(void);
int ndis_host_init(struct ndis_user_host *,struct ntimage *,uint32_t generation,
                    const struct ndis_host_ops *,bool shared,bool qualified_masker,uint8_t dirql);
bool ndis_host_live(const struct ndis_user_host *);
void ndis_host_stop(struct ndis_user_host *,const char *,uint32_t vector,uint32_t eip,uint64_t now);
void ndis_host_watchdog(struct ndis_user_host *,uint64_t now);
int ndis_host_mapping(struct ndis_user_host *,const struct ndis_host_map *);
void ndis_host_ports(const struct ndis_user_host *,uint8_t bitmap[ARCH_IO_BYTES]);
/* Queued deadlines begin at publication and cannot be extended by user code. */
int ndis_host_enqueue(struct ndis_user_host *,enum ndis_host_kind,uint32_t address,
                       uint32_t argument,uint64_t now,uint32_t *token);
int ndis_host_take(struct ndis_user_host *,uint64_t now,struct ndis_host_event *);
int ndis_host_complete(struct ndis_user_host *,uint32_t generation,uint32_t token,uint64_t now);
bool ndis_host_cancel_queued(struct ndis_user_host *,uint32_t generation,uint32_t token);
/* Kernel-owned upcall/return records: user completion cannot keep executing an
 * abandoned callback, erase an outer deadline, or supply a return context. */
int ndis_host_deliver(struct ndis_user_host *,struct trap_frame *,uint64_t now);
int ndis_host_return_event(struct ndis_user_host *,struct trap_frame *,uint32_t generation,
                           uint32_t token,uint64_t now);
/* Pending != handled or not-mine. The registry accounts this separately. */
int ndis_host_interrupt(struct ndis_user_host *,uint64_t now);
int ndis_host_rearm(struct ndis_user_host *,uint32_t generation,uint32_t token,uint64_t now);
/* Exact unprefixed CLI/STI/HLT only. Others fail into host quarantine. */
int ndis_host_gp(struct ndis_user_host *,struct trap_frame *,uint8_t opcode);
/* Retire an emulated instruction, or consume the TF #DB after a native one. */
void ndis_host_retired(struct ndis_user_host *,struct trap_frame *);
int ndis_host_debug(struct ndis_user_host *,struct trap_frame *);
/* Explicit shim flag protocol, NOT transparent PUSHF/POPF virtualization.
 * Dependent raw flag patterns stay unqualified. Physical IF always stays 1. */
uint32_t ndis_host_flags(const struct ndis_user_host *,uint32_t physical);
int ndis_host_restore_flags(struct ndis_user_host *,uint32_t saved);
#endif
