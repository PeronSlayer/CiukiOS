/* SPDX-License-Identifier: GPL-2.0-only */
#include "user_memory.h"
#include <ciuki/cpu.h>
#include <ciuki/kernel.h>
static bool stopped(struct ndis_user_memory *m)
{ return m && m->as && m->as->pd_phys && !m->as->memory && m->generation &&
    !m->retained && m->quiescent && m->quiescent(m->context); }
static bool span(uint32_t physical,uint32_t bytes)
{ return bytes && !(bytes&4095) && !(physical&4095) && bytes-1<=UINT32_MAX-physical; }
int ndis_user_grant_bar(struct ndis_user_memory *m,unsigned slot,int claim,gen_t gen,
                        uint32_t physical,uint32_t bytes)
{
    if(!stopped(m) || slot>=NDIS_HOST_MAPS || m->grant[slot].admitted ||
       !span(physical,bytes) || !registry_valid(claim,gen)) return 0;
    const struct resource *r=registry_get((unsigned)claim);
    if(!r || r->state!=RS_ACTIVE || r->type!=RES_MMIO || r->start!=physical ||
       r->end-r->start!=bytes || !vmm_cache_alias_ok(physical,bytes,PTE_PCD|PTE_PWT)) return 0;
    m->grant[slot]=(struct ndis_user_grant){.view={.physical=physical,.bytes=bytes,
        .generation=m->generation,.kind=NDIS_HOST_BAR,.uncached=true},.claim=claim,
        .claim_generation=gen,.admitted=true}; return 1;
}
int ndis_user_grant_dma(struct ndis_user_memory *m,unsigned slot,const struct mm_pin *pin)
{
    if(!stopped(m) || slot>=NDIS_HOST_MAPS || m->grant[slot].admitted || !pin ||
       !pin->ticket || !mm_pin_valid || !mm_pin_valid(pin,m->generation) || pin->pfn_table ||
       !pin->pages || pin->first_pfn>=PMM_MAX_PAGES || pin->pages>PMM_MAX_PAGES-pin->first_pfn) return 0;
    for(uint32_t i=0;i<pin->pages;i++) if(!ledger_page_pinned(pin->first_pfn+i)) return 0;
    uint32_t physical=pin->first_pfn*4096u, bytes=pin->pages*4096u;
    /* The existing permanent CPU direct map is WB. Do not create a UC alias
     * of ordinary RAM. Noncoherent/UC DMA needs a separate qualified arena. */
    if(!vmm_cache_alias_ok(physical,bytes,0)) return 0;
    m->grant[slot]=(struct ndis_user_grant){.view={.physical=physical,.bytes=bytes,
        .generation=m->generation,.ticket=pin->ticket,.kind=NDIS_HOST_DMA},.pin=*pin,
        .admitted=true}; return 1;
}
bool ndis_user_mapping_owned(void *ctx,const struct ndis_host_map *view)
{
    struct ndis_user_memory *m=ctx;
    if(!m || m->retained || !view || view->generation!=m->generation) return false;
    for(unsigned i=0;i<NDIS_HOST_MAPS;i++) {
        const struct ndis_user_grant *g=&m->grant[i];
        if(!g->admitted || g->view.kind!=view->kind || g->view.physical!=view->physical ||
           g->view.bytes!=view->bytes || g->view.ticket!=view->ticket ||
           g->view.uncached!=view->uncached) continue;
        if(view->kind==NDIS_HOST_BAR) {
            const struct resource *r=registry_get((unsigned)g->claim);
            return registry_valid(g->claim,g->claim_generation) && r && r->state==RS_ACTIVE &&
                r->type==RES_MMIO && r->start==view->physical && r->end-r->start==view->bytes;
        }
        if(!mm_pin_valid || !mm_pin_valid(&g->pin,m->generation)) return false;
        for(uint32_t p=0;p<g->pin.pages;p++)
            if(!ledger_page_pinned(g->pin.first_pfn+p)) return false;
        return true;
    }
    return false;
}
int ndis_user_mapping_apply(void *ctx,const struct ndis_host_map *view,bool install)
{
    struct ndis_user_memory *m=ctx;
    if(!m || !m->as || !view || !span(view->physical,view->bytes) ||
       (view->va&4095) || view->va<0x10000u || view->va>=0xc0000000u ||
       view->bytes>0xc0000000u-view->va || (install &&
        (!stopped(m) || !ndis_user_mapping_owned(m,view)))) return -EINVAL;
    uint32_t at=0;
    for(;at<view->bytes;at+=4096) {
        int err=install?as_map_borrowed(m->as,view->va+at,view->physical+at,
            PTE_U|PTE_W|(view->uncached?PTE_PCD|PTE_PWT:0)):
            as_revoke_borrowed(m->as,view->va+at,view->physical+at);
        if(err) {
            if(install) while(at) {
                at-=4096; (void)as_revoke_borrowed(m->as,view->va+at,view->physical+at);
            }
            return err;
        }
    }
    return 0;
}
static void release(void *ctx,void *image,uint32_t bytes)
{
    struct ndis_user_memory *m=ctx;
    if(m->retained || !m->physical || image!=P2V(m->physical) || bytes>m->bytes) return;
    while(m->mapped) {
        uint32_t at=m->mapped-4096;
        if(as_revoke_borrowed(m->as,m->execution_base+at,m->physical+at)) {
            m->retained=true; return;
        }
        m->mapped=at;
    }
    if(m->image_pin.ticket && (!mm_unpin || mm_unpin(&m->image_pin,m->generation))) {
        m->retained=true; return;
    }
    for(uint32_t at=0;at<m->bytes;at+=4096) mm_free(m->physical+at,MM_NET);
    m->physical=m->bytes=0;
}
static void *allocate(void *ctx,uint32_t preferred,uint32_t bytes,uint32_t *base)
{
    (void)preferred;
    struct ndis_user_memory *m=ctx;
    if(!stopped(m) || m->physical || !mm_pin || !mm_unpin || !bytes ||
       bytes>NDIS_IMAGE_LIMIT || PAGE_ALIGN_UP(bytes)>m->reserved_bytes ||
       m->execution_base<0x10000 || (m->execution_base&4095) ||
       m->execution_base>=0xc0000000 || PAGE_ALIGN_UP(bytes)>0xc0000000-m->execution_base) return 0;
    m->bytes=PAGE_ALIGN_UP(bytes); m->physical=pmm_alloc_contig(m->bytes/4096);
    if(!m->physical) { m->bytes=0; return 0; }
    mm_claim(m->physical,m->bytes/4096,MM_NET);
    m->image_pin=(struct mm_pin){.first_pfn=m->physical/4096,.pages=m->bytes/4096,
        .generation=m->generation,.address_mask=UINT32_MAX,.alignment=4096,.direction=MM_PIN_CPU};
    if(mm_pin(&m->image_pin)) { release(m,P2V(m->physical),bytes); return 0; }
    for(;m->mapped<m->bytes;m->mapped+=4096) {
        if(as_map_borrowed(m->as,m->execution_base+m->mapped,m->physical+m->mapped,PTE_U|PTE_W)) {
            release(m,P2V(m->physical),bytes); return 0;
        }
    }
    *base=m->execution_base; return P2V(m->physical);
}
static int resident(void *ctx,void *image,uint32_t bytes,uint32_t base)
{
    struct ndis_user_memory *m=ctx;
    if(!stopped(m) || !m->physical || !mm_pin_valid ||
       !mm_pin_valid(&m->image_pin,m->generation) || image!=P2V(m->physical) ||
       bytes>m->bytes || base!=m->execution_base) return 0;
    for(uint32_t at=0;at<bytes;at+=4096) {
        uint32_t pte=0;
        if(as_lookup(m->as,base+at,&pte)!=m->physical+at ||
           (pte&(PTE_P|PTE_U|PTE_BORROWED))!=(PTE_P|PTE_U|PTE_BORROWED) ||
           (pte&(PTE_PCD|PTE_PWT)) || !ledger_page_pinned((m->physical+at)/4096)) return 0;
    }
    return 1;
}
static int user_import(void *ctx,const struct ndis_export *symbol,uint32_t *address)
{
    struct ndis_user_memory *m=ctx;
    for(unsigned i=0;i<m->export_count;i++) {
        const struct ndis_user_export *e=&m->exports[i];
        if(e->symbol!=symbol || !e->bytes || e->address<0x10000 || e->address>=0xc0000000 ||
           e->bytes>0xc0000000-e->address || !as_range_ok(m->as,e->address,e->bytes,false)) continue;
        for(uint32_t at=0;at<e->bytes;) {
            uint32_t pte=0,phys=as_lookup(m->as,e->address+at,&pte);
            if(!(pte&PTE_U) || !ledger_page_pinned(phys/4096)) return 0;
            if(pte&PTE_W) return 0; /* immutable function veneers and tick data */
            uint32_t step=4096-((e->address+at)&4095);
            if(step>=e->bytes-at) break;
            at+=step;
        }
        *address=e->address; return 1;
    }
    return 0;
}
static int seal_code(void *ctx,const struct pe_image *p)
{
    struct ndis_user_memory *m=ctx;
    if(!stopped(m) || !p || p->base!=m->execution_base || p->size>m->bytes) return 0;
    /* Validate page sharing before changing anything. A writable section must
     * not inhabit any executable page (i686 has no NX bit here). */
    for(unsigned i=0;i<p->section_count;i++) {
        const struct pe_section *s=&p->sections[i];
        if(!(s->flags&0x20000000) || !s->span) continue;
        uint32_t first=s->rva&~4095u,last=PAGE_ALIGN_UP(s->rva+s->span);
        for(unsigned j=0;j<p->section_count;j++) {
            const struct pe_section *d=&p->sections[j];
            if((d->flags&0x80000000) && d->span && d->rva<last && first<d->rva+d->span) return 0;
        }
    }
    for(unsigned i=0;i<p->section_count;i++) {
        const struct pe_section *s=&p->sections[i];
        if(!(s->flags&0x20000000) || !s->span) continue;
        for(uint32_t at=s->rva&~4095u;at<PAGE_ALIGN_UP(s->rva+s->span);at+=4096)
            if(as_seal_borrowed(m->as,p->base+at,m->physical+at)) return 0;
    }
    return 1;
}
void ntimage_user_platform(struct ntimage_platform *p,struct ndis_user_memory *m)
{
    *p=(struct ntimage_platform){.memory={.context=m,.allocate=allocate,.release=release},
        .resident=resident,.image_budget=m->reserved_bytes,.user_host=true,.user_import=user_import,
        .flag_table=m->flag_table,.flag_workspace=m->flag_workspace,.seal_code=seal_code};
}
