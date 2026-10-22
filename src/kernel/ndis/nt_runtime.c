/* NT service mechanisms for the process-local shim; no foreign pointers are
 * dereferenced directly. Sources: docs/design/ndis-user-host.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "nt_runtime.h"
#include <ciuki/kernel.h>
#include <stddef.h>
_Static_assert(sizeof(struct ndis_nt_string)==8,"NT32 UNICODE_STRING");
_Static_assert(sizeof(struct ndis_nt_mdl32)==28,"NT32 MDL");
_Static_assert(offsetof(struct ndis_nt_mdl32,offset)==24,"NT32 MDL ByteOffset");
static bool live(struct ndis_nt_runtime *r)
{ return r && ndis_host_live(r->host) && mm_pin_valid && mm_pin_valid(&r->pin,r->host->generation); }
static int bad(struct ndis_nt_runtime *r,const char *why)
{ if(r && r->host) ndis_host_stop(r->host,why,0,0,r->ops.now?r->ops.now(r->ops.context)/10:0);return 0; }
static int read_mem(struct ndis_nt_runtime *r,uint32_t address,void *out,uint32_t bytes)
{ return live(r) && r->ops.read(r->ops.context,address,out,bytes); }
static int write_mem(struct ndis_nt_runtime *r,uint32_t address,const void *in,uint32_t bytes)
{ return live(r) && r->ops.write(r->ops.context,address,in,bytes); }
static bool extent(uint32_t at,uint32_t bytes,uint32_t base,uint32_t span)
{ return bytes && at>=base && at-base<span && bytes<=span-(at-base); }
int ndis_nt_init(struct ndis_nt_runtime *r,struct ndis_user_host *h,const struct ndis_nt_ops *ops,
                  uint32_t arena,uint32_t bytes,const struct mm_pin *pin,uint32_t pin_offset)
{
    if(!r || !h || !ndis_host_live(h) || !ops || !ops->read || !ops->write || !ops->now ||
       !ops->enqueue || !ops->cancel || !ops->wait || !ops->wake || !pin || !mm_pin_valid ||
       !mm_pin_valid(pin,h->generation) || !bytes || (arena&4095) || (bytes&4095) ||
       arena<0x10000 || arena>=0xc0000000 || bytes>0xc0000000-arena || (pin_offset&4095) ||
       (uint64_t)pin_offset+bytes>pin->pages*4096ull) return 0;
    *r=(struct ndis_nt_runtime){.host=h,.ops=*ops,.arena=arena,.bytes=bytes,.pin=*pin,.pin_offset=pin_offset};return 1;
}
void ndis_nt_stop(struct ndis_nt_runtime *r)
{
    if(!r || !r->host) return;
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) {
        if(r->dpc[i].queued) (void)r->ops.cancel(r->ops.context,r->dpc[i].token);
        r->dpc[i].queued=false;r->dpc[i].running_token=0;r->timer[i].armed=false;r->lock[i].held=false;
    }
    r->irql_depth=0;r->host->irql=0;r->host->synchronized=false;r->ops.wake(r->ops.context);
    /* Pool/MDL and original ledger pins deliberately survive quarantine. */
}
int ndis_nt_raise(struct ndis_nt_runtime *r,uint8_t level,uint8_t *old)
{
    if(!live(r) || !old || level<r->host->irql || level>r->host->dirql ||
       r->irql_depth==NDIS_HOST_DEPTH) return bad(r,"nt-irql-raise");
    *old=r->host->irql;r->irql_stack[r->irql_depth++]=*old;r->host->irql=level;return 1;
}
int ndis_nt_lower(struct ndis_nt_runtime *r,uint8_t old)
{
    if(!live(r) || !r->irql_depth || old!=r->irql_stack[r->irql_depth-1] || old>r->host->irql)
        return bad(r,"nt-irql-restore");
    r->irql_depth--;r->host->irql=old;return 1;
}
int ndis_nt_lock_init(struct ndis_nt_runtime *r,uint32_t address)
{
    if(!live(r) || (address&3)) return bad(r,"nt-lock-address");
    unsigned slot=NDIS_NT_OBJECTS;
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) {
        if(r->lock[i].initialized && r->lock[i].address==address) {
            if(r->lock[i].held) return bad(r,"nt-lock-reinitialize");slot=i;break;
        }
        if(!r->lock[i].initialized && slot==NDIS_NT_OBJECTS) slot=i;
    }
    uint32_t zero=0;
    if(slot==NDIS_NT_OBJECTS || !write_mem(r,address,&zero,4)) return bad(r,"nt-lock-capacity");
    r->lock[slot].address=address;r->lock[slot].initialized=true;return 1;
}
int ndis_nt_lock(struct ndis_nt_runtime *r,uint32_t address,bool at_dpc,uint8_t *old)
{
    if(!live(r) || !old || !r->host->depth || (at_dpc?r->host->irql!=2:r->host->irql>2)) return bad(r,"nt-lock-irql");
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->lock[i].initialized && r->lock[i].address==address) {
        if(r->lock[i].held) return bad(r,"nt-lock-reentry");
        *old=r->host->irql;
        if(!at_dpc && !ndis_nt_raise(r,2,old)) return 0;
        r->lock[i].held=true;r->lock[i].holder=r->host->active[r->host->depth-1].token;
        r->lock[i].old_irql=*old;r->lock[i].dpc=at_dpc;uint32_t one=1;
        if(!write_mem(r,address,&one,4)) return bad(r,"nt-lock-write");return 1;
    }
    return bad(r,"nt-lock-uninitialized");
}
int ndis_nt_unlock(struct ndis_nt_runtime *r,uint32_t address,bool at_dpc,uint8_t old)
{
    if(!live(r) || !r->host->depth || r->host->irql!=2) return bad(r,"nt-unlock-irql");
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->lock[i].initialized && r->lock[i].address==address) {
        if(!r->lock[i].held || r->lock[i].holder!=r->host->active[r->host->depth-1].token ||
           r->lock[i].dpc!=at_dpc || (!at_dpc && r->lock[i].old_irql!=old)) return bad(r,"nt-unlock-owner");
        uint32_t zero=0;if(!write_mem(r,address,&zero,4)) return bad(r,"nt-lock-write");
        r->lock[i].held=false;return at_dpc?1:ndis_nt_lower(r,old);
    }
    return bad(r,"nt-unlock-uninitialized");
}
uint32_t ndis_nt_pool(struct ndis_nt_runtime *r,uint32_t type,uint32_t bytes,uint32_t tag)
{
    if(!live(r)) return 0;
    if(type>1 || (type==1 && r->host->irql>1) || r->host->irql>2 || !bytes)
        { bad(r,"nt-pool-type-or-irql");return 0; }
    if(bytes>r->bytes || bytes>UINT32_MAX-7) return 0;
    bytes=(bytes+7)&~7u;unsigned slot=0;
    while(slot<NDIS_NT_ALLOCS && r->allocation[slot].live) slot++;
    if(slot==NDIS_NT_ALLOCS) return 0;
    uint32_t alignment=bytes>=4096?4096:8;
    for(uint32_t offset=0;offset<=r->bytes-bytes;) {
        offset=(offset+alignment-1)&~(alignment-1);
        if(offset>r->bytes-bytes) break;
        /* NT small pool allocations cannot cross a page. */
        if(bytes<4096 && (offset&4095)+bytes>4096) { offset=(offset+4095)&~4095u;continue; }
        uint32_t next=offset;bool conflict=false;
        for(unsigned i=0;i<NDIS_NT_ALLOCS;i++) if(r->allocation[i].live &&
            r->arena+offset<r->allocation[i].address+r->allocation[i].bytes &&
            r->allocation[i].address<r->arena+offset+bytes) {
            uint32_t end=r->allocation[i].address-r->arena+r->allocation[i].bytes;
            if(end>next) next=end;conflict=true;
        }
        if(conflict) { offset=next;continue; }
        r->allocation[slot].address=r->arena+offset;r->allocation[slot].bytes=bytes;
        r->allocation[slot].tag=tag;r->allocation[slot].paged=type==1;r->allocation[slot].live=true;
        return r->arena+offset;
    }
    return 0;
}
static int allocation(struct ndis_nt_runtime *r,uint32_t address)
{ for(unsigned i=0;i<NDIS_NT_ALLOCS;i++) if(r->allocation[i].live && r->allocation[i].address==address) return (int)i;return -1; }
int ndis_nt_free(struct ndis_nt_runtime *r,uint32_t address)
{
    if(!live(r)) return 0;
    int i=allocation(r,address);
    if(i<0 || r->host->irql>2 || (r->allocation[i].paged && r->host->irql>1)) return bad(r,"nt-pool-free");
    for(unsigned j=0;j<NDIS_NT_OBJECTS;j++) if(r->mdl[j].live &&
        (r->mdl[j].address==address || extent(r->mdl[j].buffer,r->mdl[j].bytes,address,r->allocation[i].bytes)))
        return bad(r,"nt-pool-mdl-still-owned");
    r->allocation[i].live=false;return 1;
}
int ndis_nt_dpc_init(struct ndis_nt_runtime *r,uint32_t object,uint32_t function,uint32_t context)
{
    if(!live(r) || !object || !ntimage_executable(r->host->image,function)) return bad(r,"nt-dpc-function");
    unsigned slot=NDIS_NT_OBJECTS;
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) {
        if(r->dpc[i].live && r->dpc[i].address==object) { if(r->dpc[i].queued || r->dpc[i].running_token) return bad(r,"nt-dpc-reinitialize");slot=i;break; }
        if(!r->dpc[i].live && slot==NDIS_NT_OBJECTS) slot=i;
    }
    if(slot==NDIS_NT_OBJECTS) return 0;
    r->dpc[slot].address=object;r->dpc[slot].callback=function;r->dpc[slot].context=context;r->dpc[slot].live=true;return 1;
}
int ndis_nt_dpc_insert(struct ndis_nt_runtime *r,uint32_t object,uint32_t a,uint32_t b)
{
    if(!live(r)) return 0;
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->dpc[i].live && r->dpc[i].address==object) {
        if(r->dpc[i].queued) return 0;
        uint32_t args[4]={object,r->dpc[i].context,a,b};
        if(!r->ops.enqueue(r->ops.context,r->dpc[i].callback,4,args,&r->dpc[i].token)) return 0;
        r->dpc[i].queued=true;return 1;
    }
    return bad(r,"nt-dpc-uninitialized");
}
int ndis_nt_dpc_remove(struct ndis_nt_runtime *r,uint32_t object)
{
    if(!live(r)) return 0;
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->dpc[i].live && r->dpc[i].address==object) {
        if(!r->dpc[i].queued || !r->ops.cancel(r->ops.context,r->dpc[i].token)) return 0;
        r->dpc[i].queued=false;return 1;
    }
    return bad(r,"nt-dpc-uninitialized");
}
void ndis_nt_dpc_started(struct ndis_nt_runtime *r,uint32_t token)
{ if(r) for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->dpc[i].queued && r->dpc[i].token==token) {
    r->dpc[i].queued=false;r->dpc[i].running_token=token;
} }
int ndis_nt_event_init(struct ndis_nt_runtime *r,uint32_t object,bool notification,int32_t count,int32_t limit)
{
    if(!live(r) || !object || count<0 || count>limit || limit<1) return bad(r,"nt-event-state");
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(!r->event[i].live || r->event[i].address==object) {
        r->event[i].address=object;r->event[i].count=count;r->event[i].limit=limit;
        r->event[i].notification=notification;r->event[i].live=true;return 1;
    }
    return 0;
}
int ndis_nt_event_set(struct ndis_nt_runtime *r,uint32_t object,int32_t increment,int32_t *previous)
{
    if(!live(r) || !previous || increment<1) return bad(r,"nt-event-increment");
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->event[i].live && r->event[i].address==object) {
        *previous=r->event[i].count;
        if(r->event[i].limit==1) r->event[i].count=1;
        else { if(increment>r->event[i].limit-r->event[i].count) return bad(r,"nt-semaphore-limit");r->event[i].count+=increment; }
        r->ops.wake(r->ops.context);return 1;
    }
    return bad(r,"nt-event-uninitialized");
}
int ndis_nt_event_reset(struct ndis_nt_runtime *r,uint32_t object,int32_t *previous)
{
    if(!live(r) || !previous) return 0;
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->event[i].live && r->event[i].address==object) {
        *previous=r->event[i].count;r->event[i].count=0;return 1;
    }
    return bad(r,"nt-event-uninitialized");
}
static uint64_t add_time(uint64_t now,uint64_t delta) { return delta>UINT64_MAX-now?UINT64_MAX:now+delta; }
static int due_time(struct ndis_nt_runtime *r,int64_t due,uint64_t *out)
{
    uint64_t now=r->ops.now(r->ops.context);
    if(due<0) *out=add_time(now,0u-(uint64_t)due);
    else {
        if(!r->ops.system_time) return 0;
        uint64_t wall=r->ops.system_time(r->ops.context);
        *out=due<=(int64_t)wall?now:add_time(now,(uint64_t)due-wall);
    }
    return 1;
}
struct wait_context { struct ndis_nt_runtime *r; unsigned index; };
static bool ready(void *ctx)
{ struct wait_context *w=ctx;return !live(w->r) || w->r->event[w->index].count>0; }
uint32_t ndis_nt_wait(struct ndis_nt_runtime *r,uint32_t object,const int64_t *timeout)
{
    if(!live(r)) return NDIS_NT_CANCELLED;
    if(r->host->irql>1) { bad(r,"nt-wait-irql");return NDIS_NT_INVALID; }
    unsigned i=0;while(i<NDIS_NT_OBJECTS && (!r->event[i].live || r->event[i].address!=object)) i++;
    if(i==NDIS_NT_OBJECTS) { bad(r,"nt-wait-object");return NDIS_NT_INVALID; }
    uint64_t due=UINT64_MAX;
    if(timeout && *timeout==0) due=r->ops.now(r->ops.context);
    else if(timeout && !due_time(r,*timeout,&due)) return NDIS_NT_INVALID;
    struct wait_context w={r,i};
    if(!ready(&w) && !r->ops.wait(r->ops.context,ready,&w,due)) return live(r)?NDIS_NT_TIMEOUT:NDIS_NT_CANCELLED;
    if(!live(r)) return NDIS_NT_CANCELLED;
    if(!r->event[i].count) return NDIS_NT_TIMEOUT;
    if(!r->event[i].notification) r->event[i].count--;
    return NDIS_NT_SUCCESS;
}
int ndis_nt_timer_init(struct ndis_nt_runtime *r,uint32_t object)
{
    if(!live(r) || !object) return 0;
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(!r->timer[i].live || r->timer[i].address==object) {
        r->timer[i].address=object;r->timer[i].live=true;r->timer[i].armed=false;return 1;
    }
    return 0;
}
int ndis_nt_timer_set(struct ndis_nt_runtime *r,uint32_t object,int64_t due,uint32_t period_ms,uint32_t dpc,bool *previous)
{
    if(!live(r) || !previous || r->host->irql>2) return 0;
    bool found=!dpc;
    for(unsigned j=0;j<NDIS_NT_OBJECTS;j++) if(r->dpc[j].live && r->dpc[j].address==dpc) found=true;
    if(!found) return bad(r,"nt-timer-dpc");
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->timer[i].live && r->timer[i].address==object) {
        uint64_t when;if(!due_time(r,due,&when)) return 0;
        *previous=r->timer[i].armed;r->timer[i].due=when;r->timer[i].dpc=dpc;
        r->timer[i].period=(uint64_t)period_ms*10000;r->timer[i].armed=true;return 1;
    }
    return bad(r,"nt-timer-object");
}
int ndis_nt_timer_cancel(struct ndis_nt_runtime *r,uint32_t object,bool *previous)
{
    if(!live(r) || !previous) return 0;
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->timer[i].live && r->timer[i].address==object) {
        *previous=r->timer[i].armed;r->timer[i].armed=false;return 1;
    }
    return bad(r,"nt-timer-object");
}
void ndis_nt_poll(struct ndis_nt_runtime *r)
{
    if(!live(r)) return;
    uint64_t now=r->ops.now(r->ops.context);
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->timer[i].armed && now>=r->timer[i].due) {
        uint64_t period=r->timer[i].period;r->timer[i].armed=period && now<=UINT64_MAX-period;
        if(r->timer[i].armed) r->timer[i].due=now+period;
        if(r->timer[i].dpc) (void)ndis_nt_dpc_insert(r,r->timer[i].dpc,(uint32_t)now,(uint32_t)(now>>32));
        r->ops.wake(r->ops.context);
    }
}
uint32_t ndis_nt_mdl(struct ndis_nt_runtime *r,uint32_t buffer,uint32_t bytes)
{
    if(!live(r) || !extent(buffer,bytes,r->arena,r->bytes) || bytes>UINT32_MAX-(buffer&4095)-4095) return 0;
    bool owned=false;
    for(unsigned i=0;i<NDIS_NT_ALLOCS;i++) if(r->allocation[i].live &&
        extent(buffer,bytes,r->allocation[i].address,r->allocation[i].bytes)) owned=true;
    if(!owned) return 0;
    unsigned slot=0;while(slot<NDIS_NT_OBJECTS && r->mdl[slot].live) slot++;
    if(slot==NDIS_NT_OBJECTS) return 0;
    uint32_t pages=(bytes+(buffer&4095)+4095)/4096,span=28+pages*4;
    if(span>32767) return 0;
    uint32_t address=ndis_nt_pool(r,0,span,0x6c644d4e);
    if(!address) return 0;
    struct ndis_nt_mdl32 mdl={.size=(uint16_t)span,.flags=4,.mapped=buffer,
        .start=buffer&~4095u,.bytes=bytes,.offset=buffer&4095};
    if(!write_mem(r,address,&mdl,28)) { bad(r,"nt-mdl-write");return 0; }
    for(uint32_t i=0;i<pages;i++) {
        uint32_t pfn=r->pin.first_pfn+r->pin_offset/4096+((buffer-r->arena)/4096)+i;
        if(!write_mem(r,address+28+i*4,&pfn,4)) { bad(r,"nt-mdl-write");return 0; }
    }
    r->mdl[slot].address=address;r->mdl[slot].buffer=buffer;r->mdl[slot].bytes=bytes;r->mdl[slot].live=true;
    return address;
}
uint32_t ndis_nt_map_mdl(struct ndis_nt_runtime *r,uint32_t address,uint32_t mode,uint32_t cache,uint32_t requested)
{
    if(!live(r) || mode!=0 || cache!=1 || requested) return 0; /* owned WB, kernel-personality view */
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->mdl[i].live && r->mdl[i].address==address) {
        /* Never trust a driver's overwritten MDL/PFN array. Its original
         * allocation and exact live ledger token determine the mapping. */
        r->mdl[i].mapped=true;return r->mdl[i].buffer;
    }
    return 0;
}
int ndis_nt_free_mdl(struct ndis_nt_runtime *r,uint32_t address)
{
    if(!live(r)) return 0;
    for(unsigned i=0;i<r->map_registers;i++) if(r->dma[i].active && r->dma[i].mdl==address)
        return bad(r,"nt-mdl-dma-owned");
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->mdl[i].live && r->mdl[i].address==address) {
        r->mdl[i].live=false;return ndis_nt_free(r,address);
    }
    return bad(r,"nt-mdl-free");
}
uint32_t ndis_nt_unicode_append(struct ndis_nt_runtime *r,uint32_t destination,uint32_t source,bool append)
{
    struct ndis_nt_string d,s;
    if(!read_mem(r,destination,&d,8) || !read_mem(r,source,&s,8) ||
       (append && ((d.length&1) || d.length>d.maximum)) || (s.length&1) || s.length>s.maximum)
        { bad(r,"nt-unicode-descriptor");return NDIS_NT_INVALID; }
    uint32_t at=append?d.length:0,bytes=s.length;
    if(bytes>d.maximum-at) { if(append) return NDIS_NT_SMALL;bytes=(d.maximum-at)&~1u; }
    if(d.buffer>UINT32_MAX-at || bytes>UINT32_MAX-d.buffer-at || bytes>UINT32_MAX-s.buffer || bytes>4096)
        return NDIS_NT_INVALID;
    uint8_t copy[128];
    /* memmove direction permits overlapping counted buffers. */
    bool reverse=d.buffer+at>s.buffer && d.buffer+at<s.buffer+bytes;
    for(uint32_t done=0;done<bytes;) {
        uint32_t n=bytes-done;if(n>sizeof(copy)) n=sizeof(copy);
        uint32_t off=reverse?bytes-done-n:done;
        if(!read_mem(r,s.buffer+off,copy,n) || !write_mem(r,d.buffer+at+off,copy,n)) {
            bad(r,"nt-unicode-buffer");return NDIS_NT_INVALID;
        }
        done+=n;
    }
    d.length=(uint16_t)(at+bytes);
    if(!write_mem(r,destination,&d,8)) { bad(r,"nt-unicode-write");return NDIS_NT_INVALID; }
    return NDIS_NT_SUCCESS;
}
int ndis_nt_dma_reserve(struct ndis_nt_runtime *r,unsigned count,uint32_t maximum_buffer)
{
    if(!live(r) || r->host->irql || !count || count>NDIS_NT_MAP_REGISTERS || r->map_registers ||
       !maximum_buffer || maximum_buffer>NDIS_NT_DMA_RESERVATION) return 0;
    /* NDIS's 64-register ceiling counts worst-case physical pages, not
     * just base send-buffer indices. An unaligned 1514-byte buffer needs 2. */
    uint32_t per_base=(maximum_buffer+8190u)/4096;
    if(count>NDIS_NT_MAP_REGISTERS/per_base) return 0;
    r->map_registers=count;r->map_maximum_buffer=maximum_buffer;return 1;
}
int ndis_nt_dma_start(struct ndis_nt_runtime *r,unsigned index,uint32_t mdl,enum mm_direction direction,
                       uint32_t mask,uint32_t alignment,uint32_t boundary,uint32_t max_segment,
                       struct ndis_nt_segment *segments,unsigned *count)
{
    if(!live(r) || !mm_pin || !mm_unpin || index>=r->map_registers || r->dma[index].active ||
       direction==MM_PIN_CPU || (unsigned)direction>MM_PIN_BIDIRECTIONAL || !segments || !count ||
       !*count || *count>NDIS_NT_MAP_REGISTERS || !alignment || (alignment&(alignment-1)) ||
       (boundary && (boundary&(boundary-1))) || (mask!=UINT32_MAX && (mask&(mask+1))) || !max_segment || r->host->irql>2) return 0;
    unsigned m=0;while(m<NDIS_NT_OBJECTS && (!r->mdl[m].live || r->mdl[m].address!=mdl)) m++;
    if(m==NDIS_NT_OBJECTS) return 0;
    for(unsigned i=0;i<r->map_registers;i++) if(r->dma[i].active && r->dma[i].mdl==mdl) return 0;
    uint64_t first=(uint64_t)r->pin.first_pfn*4096+r->pin_offset+r->mdl[m].buffer-r->arena;
    uint32_t bytes=r->mdl[m].bytes;
    if(bytes>r->map_maximum_buffer) return 0;
    if(bytes>NDIS_NT_DMA_RESERVATION-r->dma_bytes) return 0;
    if(first>UINT32_MAX || bytes-1>UINT32_MAX-first || (first&(alignment-1)) ||
       ((first+bytes-1)&~(uint64_t)mask)) return 0;
    unsigned used=0;uint32_t remaining=bytes,physical=(uint32_t)first;
    while(remaining) {
        if(used==*count) return 0;
        uint32_t n=remaining;if(n>max_segment) n=max_segment;
        if(boundary && n>boundary-(physical&(boundary-1))) n=boundary-(physical&(boundary-1));
        segments[used++]=(struct ndis_nt_segment){physical,n};remaining-=n;physical+=n;
    }
    struct mm_pin pin={.generation=r->host->generation,.first_pfn=(uint32_t)first/4096,
        .pages=((uint32_t)(first&4095)+bytes+4095)/4096,.address_mask=mask,
        .alignment=1,.direction=direction};
    if(mm_pin(&pin)) return 0;
    r->dma[index].pin=pin;r->dma[index].mdl=mdl;r->dma[index].bytes=bytes;r->dma[index].active=true;*count=used;
    r->dma_bytes+=bytes;
    __atomic_thread_fence(__ATOMIC_RELEASE);return 1;
}
int ndis_nt_dma_complete(struct ndis_nt_runtime *r,unsigned index,uint32_t generation,uint32_t ticket)
{
    if(!live(r) || !mm_unpin || generation!=r->host->generation || index>=r->map_registers ||
       !r->dma[index].active || ticket!=r->dma[index].pin.ticket || !mm_pin_valid(&r->dma[index].pin,generation)) return 0;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if(mm_unpin(&r->dma[index].pin,generation)) return 0;
    r->dma[index].active=false;r->dma_bytes-=r->dma[index].bytes;return 1;
}
int ndis_nt_dma_release(struct ndis_nt_runtime *r)
{
    if(!live(r) || r->host->irql) return 0;
    for(unsigned i=0;i<r->map_registers;i++) if(r->dma[i].active) return 0;
    r->map_registers=0;return 1;
}
