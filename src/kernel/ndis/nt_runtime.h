/* Per-adapter NT5 service state; all ownership is kernel-private.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_NT_RUNTIME_H
#define CIUKI_NDIS_NT_RUNTIME_H
#include "userhost.h"
#include <ciuki/ledger.h>
#define NDIS_NT_OBJECTS 32u
#define NDIS_NT_ALLOCS 128u
#define NDIS_NT_MAP_REGISTERS 64u
#define NDIS_NT_DMA_RESERVATION (128u*1024u)
#define NDIS_NT_SUCCESS 0u
#define NDIS_NT_TIMEOUT 0x102u
#define NDIS_NT_CANCELLED 0xc0000120u
#define NDIS_NT_INVALID 0xc000000du
#define NDIS_NT_SMALL 0xc0000023u
#define NDIS_NT_NOT_FOUND 0xc0000034u
struct ndis_nt_string { uint16_t length,maximum; uint32_t buffer; };
struct ndis_nt_mdl32 {
    uint32_t next;
    uint16_t size,flags;
    uint32_t process,mapped,start,bytes,offset;
};
struct ndis_nt_segment { uint32_t physical,bytes; };
struct ndis_nt_ops {
    void *context;
    int (*read)(void *,uint32_t,void *,uint32_t);
    int (*write)(void *,uint32_t,const void *,uint32_t);
    uint64_t (*now)(void *); /* monotonic 100 ns units, not a CPU guess */
    uint64_t (*system_time)(void *); /* NT epoch, validated real clock */
    int (*enqueue)(void *,uint32_t function,unsigned argc,const uint32_t *,uint32_t *token);
    bool (*cancel)(void *,uint32_t token);
    /* Wait rechecks predicate after publication under the scheduler's UP
     * exclusion, with cancellation and absolute monotonic deadline. */
    bool (*wait)(void *,bool (*predicate)(void *),void *,uint64_t deadline);
    void (*wake)(void *);
};
struct ndis_nt_runtime {
    struct ndis_user_host *host;
    struct ndis_nt_ops ops;
    uint32_t arena,bytes,pin_offset;
    struct mm_pin pin; /* allocator's original token, never user-supplied */
    struct { uint32_t address,bytes,tag; bool paged,live; } allocation[NDIS_NT_ALLOCS];
    struct { uint32_t address,holder; uint8_t old_irql; bool initialized,held,dpc; } lock[NDIS_NT_OBJECTS];
    struct { uint32_t address,callback,context,token,running_token; bool live,queued; } dpc[NDIS_NT_OBJECTS];
    struct { uint32_t address; int32_t count,limit; bool live,notification; } event[NDIS_NT_OBJECTS];
    struct { uint32_t address,buffer,bytes; bool live,mapped; } mdl[NDIS_NT_OBJECTS];
    struct { uint32_t address,dpc; uint64_t due,period; bool live,armed; } timer[NDIS_NT_OBJECTS];
    uint8_t irql_stack[NDIS_HOST_DEPTH];unsigned irql_depth;
    unsigned callback_irql_depth[NDIS_HOST_DEPTH];
    struct { struct mm_pin pin;uint32_t mdl,bytes;bool active; } dma[NDIS_NT_MAP_REGISTERS];
    unsigned map_registers;uint32_t dma_bytes,map_maximum_buffer;
};
int ndis_nt_init(struct ndis_nt_runtime *,struct ndis_user_host *,const struct ndis_nt_ops *,
                  uint32_t arena,uint32_t bytes,const struct mm_pin *,uint32_t pin_offset);
void ndis_nt_stop(struct ndis_nt_runtime *);
int ndis_nt_raise(struct ndis_nt_runtime *,uint8_t level,uint8_t *old);
int ndis_nt_lower(struct ndis_nt_runtime *,uint8_t old);
int ndis_nt_lock_init(struct ndis_nt_runtime *,uint32_t address);
int ndis_nt_lock(struct ndis_nt_runtime *,uint32_t address,bool at_dpc,uint8_t *old);
int ndis_nt_unlock(struct ndis_nt_runtime *,uint32_t address,bool at_dpc,uint8_t old);
uint32_t ndis_nt_pool(struct ndis_nt_runtime *,uint32_t type,uint32_t bytes,uint32_t tag);
int ndis_nt_free(struct ndis_nt_runtime *,uint32_t address);
int ndis_nt_dpc_init(struct ndis_nt_runtime *,uint32_t object,uint32_t function,uint32_t context);
int ndis_nt_dpc_insert(struct ndis_nt_runtime *,uint32_t object,uint32_t arg1,uint32_t arg2);
int ndis_nt_dpc_remove(struct ndis_nt_runtime *,uint32_t object);
void ndis_nt_dpc_started(struct ndis_nt_runtime *,uint32_t token);
int ndis_nt_event_init(struct ndis_nt_runtime *,uint32_t object,bool notification,int32_t count,int32_t limit);
int ndis_nt_event_set(struct ndis_nt_runtime *,uint32_t object,int32_t increment,int32_t *previous);
int ndis_nt_event_reset(struct ndis_nt_runtime *,uint32_t object,int32_t *previous);
uint32_t ndis_nt_wait(struct ndis_nt_runtime *,uint32_t object,const int64_t *timeout);
int ndis_nt_timer_init(struct ndis_nt_runtime *,uint32_t object);
int ndis_nt_timer_set(struct ndis_nt_runtime *,uint32_t object,int64_t due,uint32_t period_ms,uint32_t dpc,bool *was_set);
int ndis_nt_timer_cancel(struct ndis_nt_runtime *,uint32_t object,bool *was_set);
void ndis_nt_poll(struct ndis_nt_runtime *);
uint32_t ndis_nt_mdl(struct ndis_nt_runtime *,uint32_t buffer,uint32_t bytes);
uint32_t ndis_nt_map_mdl(struct ndis_nt_runtime *,uint32_t mdl,uint32_t mode,uint32_t cache,uint32_t requested);
int ndis_nt_free_mdl(struct ndis_nt_runtime *,uint32_t mdl);
uint32_t ndis_nt_unicode_append(struct ndis_nt_runtime *,uint32_t destination,uint32_t source,bool append);
int ndis_nt_dma_reserve(struct ndis_nt_runtime *,unsigned base_count,uint32_t maximum_buffer);
int ndis_nt_dma_start(struct ndis_nt_runtime *,unsigned index,uint32_t mdl,enum mm_direction,
                       uint32_t mask,uint32_t alignment,uint32_t boundary,uint32_t maximum_segment,
                       struct ndis_nt_segment *,unsigned *count);
int ndis_nt_dma_complete(struct ndis_nt_runtime *,unsigned index,uint32_t generation,uint32_t ticket);
int ndis_nt_dma_release(struct ndis_nt_runtime *);
#endif
