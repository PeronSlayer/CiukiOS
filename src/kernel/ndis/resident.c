/* SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include "resident.h"
static void release(void *ctx,void *image,uint32_t bytes) {
    struct ndis_resident_memory *m=ctx;
    if(!m->physical || image!=P2V(m->physical) || bytes>m->bytes) return;
    /* An unpin failure keeps the arena owned. No freeing uncertain pins. */
    if(m->pin.ticket && (!mm_unpin || mm_unpin(&m->pin,m->generation))) return;
    for(uint32_t at=0;at<m->bytes;at+=PAGE_SIZE) mm_free(m->physical+at,MM_NET);
    m->physical=m->bytes=0;
}
static void *allocate(void *ctx,uint32_t preferred,uint32_t bytes,uint32_t *base) {
    (void)preferred;
    struct ndis_resident_memory *m=ctx;
    if(m->physical || !m->generation || !mm_pin || !mm_unpin || !bytes ||
       bytes>NDIS_IMAGE_LIMIT || PAGE_ALIGN_UP(bytes)>m->reserved_bytes) return 0;
    uint32_t pages=PAGE_ALIGN_UP(bytes)/PAGE_SIZE;
    m->physical=pmm_alloc_contig(pages);
    if(!m->physical) return 0;
    m->bytes=pages*PAGE_SIZE; mm_claim(m->physical,pages,MM_NET);
    m->pin=(struct mm_pin){.generation=m->generation,.first_pfn=m->physical/PAGE_SIZE,
        .pages=pages,.address_mask=UINT32_MAX,.alignment=PAGE_SIZE,.direction=MM_PIN_CPU};
    if(mm_pin(&m->pin)) { release(m,P2V(m->physical),bytes); return 0; }
    *base=(uint32_t)(uintptr_t)P2V(m->physical);
    return P2V(m->physical);
}
static int resident(void *ctx,void *image,uint32_t bytes,uint32_t base) {
    struct ndis_resident_memory *m=ctx;
    if(!m->physical || !m->pin.ticket || bytes>m->bytes || image!=P2V(m->physical) ||
       (uint32_t)(uintptr_t)image!=base) return 0;
    for(uint32_t at=0;at<bytes;at+=PAGE_SIZE) {
        uint32_t pte=vmm_kernel_pte(base+at);
        if(!(pte&PTE_P) || (pte&PTE_U) || !ledger_page_pinned((m->physical+at)/PAGE_SIZE)) return 0;
    }
    return 1;
}
static void record(void *ctx,const char *file,const char *module,const char *name,
                   uint16_t ordinal,const char *reason) {
    (void)ctx;
    if(module && name) klog("[ndis] load refused driver=%s import=%s!%s reason=%s",file,module,name,reason);
    else if(module) klog("[ndis] load refused driver=%s import=%s!#%u reason=%s",file,module,ordinal,reason);
    else klog("[ndis] load refused driver=%s reason=%s",file,reason);
}
static void scan_record(void *ctx,const char *file,uint32_t rva,const char *reason) {
    (void)ctx;
    klog("[ndis] scan refused driver=%s eip=+%x reason=%s",file,rva,reason);
}
void ntimage_kernel_platform(struct ntimage_platform *p,struct ndis_resident_memory *m) {
    *p=(struct ntimage_platform){.memory={.context=m,.allocate=allocate,.release=release},
        .resident=resident,.record=record,.image_budget=m->reserved_bytes,.scan_record=scan_record};
}
