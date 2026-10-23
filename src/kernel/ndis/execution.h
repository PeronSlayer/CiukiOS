/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_EXECUTION_H
#define CIUKI_NDIS_EXECUTION_H
#include "callback.h"
#include <ciuki/arch.h>
/* Layout shared with arch/ndis_context.asm, always 32 bit. */
struct ndis_saved_context { uint32_t esp, ebx, esi, edi, ebp, eip, flags; };
struct ndis_execution {
    struct ndis_saved_context saved;
    struct ndis_execution *parent;
    struct ndis_callback *callback;
    unsigned irq_depth, shim_depth, critical_depth;
};
enum ndis_callback_kind { NDIS_PASSIVE, NDIS_DPC, NDIS_ISR };
struct ndis_runtime {
    struct ndis_callback *callback;
    struct ndis_execution frames[NDIS_CALLBACK_DEPTH];
    /* Explicit qualification inputs, monotonic microseconds. No guessed
     * passive default. A zero budget refuses entry. */
    uint64_t budgets[3];
    uint64_t (*now)(void);
};
typedef int32_t (NDIS_STDCALL *ndis_foreign2)(uint32_t,uint32_t);
int ndis_context_save(struct ndis_saved_context *) __attribute__((returns_twice));
void ndis_context_recover(void); /* assembly entry, EAX = saved context */
/* Per-task slot in production, supplied by synthetic execution harnesses. */
struct ndis_execution **ndis_execution_slot(void);
unsigned ndis_execution_irq_depth(void);
/* Executable only in CIUKI_NDIS_HOST_MODEL fixtures. Production refuses
 * unconditionally, like ntimage_enter, pending part-1 execution evidence. */
int ndis_execute2(struct ndis_runtime *,uint32_t,ndis_foreign2,uint32_t,uint32_t,
                  enum ndis_callback_kind,int32_t *);
int ndis_shim_enter(void);
void ndis_shim_leave(void);
void ndis_host_critical_enter(void);
void ndis_host_critical_leave(void);
/* Hooks run before the ordinary exception path and AFTER timer IRQ EOI and
 * accounting. A host IRQ nested over a shim is not a foreign shim fault. */
bool ndis_execution_exception(struct trap_frame *);
void ndis_execution_tick(struct trap_frame *,uint64_t now);
uint64_t ndis_execution_now(void);
void ndis_execution_fault_record(void *,const char *,uint32_t,uint32_t,bool);
#endif
