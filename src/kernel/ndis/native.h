/* Trusted adapter/task integration. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_NATIVE_H
#define CIUKI_NDIS_NATIVE_H
#include "userhost_kernel.h"
#include "user_memory.h"
#include "channel.h"
#include "nt_services.h"
#include "ndis_services.h"
struct ndis_native;
/* Only compiled, reviewed records can admit execution. No user enable switch.
 * Exact revision/subsystem and image hash; qualification is lead-owned. */
struct ndis_qualification {
    const char *name,*evidence;
    uint8_t hash[32];
    uint8_t package_seal[32]; /* exact INF selection, parameters, dependencies */
    uint16_t vendor,device,sub_vendor,sub_device;
    uint8_t revision;
    bool qualified,shared_mask,runtime_cpu,runtime_irq,runtime_fence;
    uint32_t bar_bytes[6]; /* measured exact extents; never guessed by BAR sizing */
    uint32_t config_write[8]; /* qualified device-specific config bytes only */
    bool (*mask)(struct ndis_native *,bool);
    int (*cause)(struct ndis_native *);
    bool (*stop)(struct ndis_native *);
};
const struct ndis_qualification *ndis_qualified_profile(const struct ntimage *,const struct pci_func *);
bool ndis_profiles_available(void);
bool ndis_qualified_package(const uint8_t hash[32],const struct pci_func *,const uint8_t seal[32]);
bool ndis_package_busy(const struct pci_func *);
struct ndis_native {
    struct ndis_user_host host;
    struct ndis_user_memory memory;
    struct ndis_channel channel;
    struct task *task;
    struct pci_func pci;
    const struct ndis_qualification *qualification;
    const char *owner;
    int irq_claim;
    int port_claim[6];gen_t port_generation[6];
    unsigned port_count;
    gen_t irq_generation;
    uint32_t isr, dpc, irq_delivery, isr_callback,dpc_callback;
    uint32_t shared_address,adapter_context,callback_token[NDIS_RING_SLOTS];
    uint32_t result_token[NDIS_RING_SLOTS],result[NDIS_RING_SLOTS];
    bool irq_attached,detach_pending,retained,irq_accounted,dpc_phase;
    void *worker_context;
    void (*wake_worker)(void *);
    void (*complete_tx)(void *,uint32_t,int);
    struct mm_pin runtime_pin;
    uint32_t runtime_physical,runtime_bytes,runtime_base,runtime_mapped;
    struct ndis_user_export nt_exports[NDIS_NT_EXPORTS+NDIS_SERVICE_COUNT];
    struct ndis_nt_runtime nt;
    struct ndis_nt_services services;
    struct kwait nt_wait;
    struct ndis_miniport miniport;
    struct ndis_services ndis;
    struct netif *netif;
    uint32_t net_token[NDIS_RING_SLOTS];uint16_t net_length[NDIS_RING_SLOTS];
};
/* n and its metadata have boot-pinned lifetime (never stack storage). Caller
 * reserves all image, payload, stacks, rings and metadata in F6 budget,
 * installs typed grants and supplies a blocked raw user task. No native owner
 * may still be attached. Returns false before touching hardware if unqualified. */
int ndis_native_bind(struct ndis_native *,struct task *,struct ntimage *,unsigned pci_index,
                      int irq_claim,gen_t,const char *owner,uint32_t generation,
                      uint32_t dispatcher,uint32_t stack_top,uint32_t stack_bytes,
                      struct ndis_channel_shared *,bool shared);
/* Native top half and worker-side drain; foreign callbacks never run here. */
void ndis_native_poll(struct ndis_native *);
void ndis_native_tick(struct ndis_native *);
int ndis_native_enqueue(struct ndis_native *,enum ndis_host_kind,uint32_t address,
                         unsigned argc,const uint32_t *arguments,uint64_t now,uint32_t *token);
int ndis_native_irq_bind(struct ndis_native *,uint32_t isr,uint32_t dpc);
int ndis_native_irq_unbind(struct ndis_native *);
int ndis_native_miniport_init(struct ndis_native *);
int ndis_native_take_result(struct ndis_native *,uint32_t generation,uint32_t token,uint32_t *result);
unsigned ndis_native_pci_read(struct ndis_native *,unsigned offset,void *,unsigned bytes);
unsigned ndis_native_pci_write(struct ndis_native *,unsigned offset,const void *,unsigned bytes);
/* Select only after exact-revision PCI INTx-disable/status qualification. */
bool ndis_native_pci_intx_mask(struct ndis_native *,bool masked);
int ndis_native_pci_intx_cause(struct ndis_native *);
bool ndis_native_no_idle_proof(struct ndis_native *);
bool ndis_native_e100_mask(struct ndis_native *,bool masked);
int ndis_native_e100_cause(struct ndis_native *);
int ndis_native_isr_result(struct ndis_native *,uint32_t generation,uint32_t delivery,
                            bool recognized,bool queue_dpc,uint64_t now);
void ndis_native_callback_result(struct ndis_native *,const struct ndis_host_event *,uint32_t result);
/* Bounded resident payload copy; uses the same original live ledger pin. */
int ndis_native_prepare(struct ndis_native *,struct ntimage *,const void *file,uint32_t bytes,
                         const char *name,uint32_t generation,uint32_t reserved_bytes,
                         uint32_t image_base,uint32_t runtime_base);
/* Pre-entry rollback only; quarantined/entered memory is retained. */
int ndis_native_discard(struct ndis_native *);
int ndis_native_miniport_start(struct ndis_native *,uint32_t driver,uint32_t path);
#endif
