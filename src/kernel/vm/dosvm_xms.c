/* XMS 3.0 and the shared XMS/EMS/future-DPMI pool. No overcommit.
 * Research: docs/design/f3-acceptance.md#f3-03.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include "dosvm_internal.h"

void dosvm_page_remove(struct dosvm *v, uint32_t va)
{
    uint32_t *pd=P2V(v->as.pd_phys), pde=pd[va>>22];
    if (!(pde&PTE_P)) return;
    uint32_t *pt=P2V(pde&~0xFFFu), *pte=&pt[(va>>12)&1023];
    if (!(*pte&PTE_P)) return;
    *pte=0; v->as.pages--; g_user_mappings--;
    if (read_cr3()==v->as.pd_phys) invlpg(va);
}

static bool pool_find(struct dosvm *v, uint32_t pages, uint32_t *first)
{
    uint32_t limit=v->memory.quota/PAGE_SIZE, run=0;
    if (!pages) { *first=0; return true; }
    for (uint32_t i=0;i<limit;i++) {
        run=v->memory.occupied[i] ? 0 : run+1;
        if (run==pages) { *first=i+1-pages; return true; }
    }
    return false;
}

void dosvm_pool_query(struct dosvm *v, uint32_t *largest, uint32_t *total)
{
    uint32_t run=0, best=0, free=0;
    for (unsigned i=0;i<v->memory.quota/PAGE_SIZE;i++) {
        if (v->memory.occupied[i]) run=0;
        else { free++; if (++run>best) best=run; }
    }
    *largest=best*PAGE_SIZE; *total=free*PAGE_SIZE;
}

bool dosvm_pool_alloc(struct dosvm *v, struct vm_block *b, uint32_t bytes)
{
    if (bytes>v->memory.quota) return false;
    uint32_t pages=(bytes+PAGE_SIZE-1)/PAGE_SIZE, first;
    if (!pool_find(v,pages,&first)) return false;
    struct vm_block next={.base=VM_POOL_BASE+first*PAGE_SIZE,.bytes=bytes,.used=true};
    for (unsigned i=0;i<pages;i++) {
        uint32_t va=next.base+i*PAGE_SIZE;
        memset(P2V(as_lookup(&v->as,va,0)),0,PAGE_SIZE);
        v->memory.occupied[first+i]=1;
    }
    next.pages=pages; v->memory.used_pages+=pages;
    v->info.pool_used=v->memory.used_pages*PAGE_SIZE;
    *b=next;
    return true;
}

void dosvm_pool_free(struct dosvm *v, struct vm_block *b)
{
    if (!b->used) return;
    unsigned first=(b->base-VM_POOL_BASE)/PAGE_SIZE;
    for (unsigned i=0;i<b->pages;i++) {
        v->memory.occupied[first+i]=0;
        v->memory.used_pages--;
    }
    memset(b,0,sizeof(*b));
    v->info.pool_used=v->memory.used_pages*PAGE_SIZE;
}

/* Checked once, copied in small slices with physical interrupts enabled.
 * EMS frame aliases are never legal EMS region endpoints. */
int dosvm_copy_region(struct dosvm *v, uint32_t src, uint32_t dst, uint32_t n, bool exchange)
{
    if (!dosvm_range(v,src,n,exchange) || !dosvm_range(v,dst,n,true)) return -EFAULT;
    uint8_t a[256], b[256];
    bool reverse=dst>src && dst-src<n;
    uint32_t done=0;
    while (done<n) {
        unsigned count=n-done>sizeof(a) ? sizeof(a) : n-done;
        unsigned off=reverse ? n-done-count : done;
        if (dosvm_read(v,src+off,a,count)) return -EFAULT;
        if (exchange && dosvm_read(v,dst+off,b,count)) return -EFAULT;
        if (dosvm_write(v,dst+off,a,count) || (exchange && dosvm_write(v,src+off,b,count))) return -EFAULT;
        done+=count;
        if (g_need_resched) schedule();
    }
    return 0;
}

bool dosvm_pool_resize(struct dosvm *v, struct vm_block *b, uint32_t bytes)
{
    if (bytes>v->memory.quota) return false;
    uint32_t pages=(bytes+PAGE_SIZE-1)/PAGE_SIZE;
    if (pages<=b->pages) {
        struct vm_block tail={.base=b->base+pages*PAGE_SIZE,.pages=b->pages-pages,.used=true};
        dosvm_pool_free(v,&tail); b->pages=pages; b->bytes=bytes; return true;
    }
    /* Growth never modifies the old block before the complete extent fits. */
    unsigned first=(b->base-VM_POOL_BASE)/PAGE_SIZE;
    bool adjacent=first+pages<=v->memory.quota/PAGE_SIZE;
    for (unsigned i=b->pages;adjacent && i<pages;i++) adjacent=!v->memory.occupied[first+i];
    if (adjacent) {
        for (unsigned i=b->pages;i<pages;i++) {
            memset(P2V(as_lookup(&v->as,b->base+i*PAGE_SIZE,0)),0,PAGE_SIZE);
            v->memory.occupied[first+i]=1;
        }
        v->memory.used_pages+=pages-b->pages;
        v->info.pool_used=v->memory.used_pages*PAGE_SIZE;
        b->bytes=bytes; b->pages=pages; return true;
    }

    struct vm_block next;
    if (!dosvm_pool_alloc(v,&next,bytes)) return false;
    if (dosvm_copy_region(v,b->base,next.base,b->bytes,false)) { dosvm_pool_free(v,&next); return false; }
    dosvm_pool_free(v,b); *b=next;
    return true;
}

int dosvm_memory_init(struct dosvm *v, const struct dosvm_config *config)
{
    v->memory.quota=v->info.commitment-DOSVM_LOW_CAP;
    if (v->memory.quota>VM_POOL_CAP) v->memory.quota=VM_POOL_CAP;
    v->info.pool_quota=v->memory.quota;
    uint16_t frame=config ? config->ems_frame : 0;
    if (frame && frame!=0xE000 && frame!=0xD000) return -EINVAL;
    v->memory.frame_segment=frame ? frame : 0xE000;
    v->memory.umb_enabled=!(config && config->disable_umb);
    /* Eager backing makes synthetic BIOS RAM usable even before a client
     * obtains handles. Admission has already reserved this entire pool. */
    unsigned tables=v->as.tables;
    for (uint32_t off=0;off<v->memory.quota;off+=PAGE_SIZE) {
        uint32_t phys=pmm_alloc_zone(ZONE_NORMAL);
        if (!phys) { v->info.resident+=(v->as.tables-tables)*PAGE_SIZE; return -ENOMEM; }
        memset(P2V(phys),0,PAGE_SIZE);
        if (as_map(&v->as,VM_POOL_BASE+off,phys,PTE_U|PTE_W)) {
            pmm_free(phys); v->info.resident+=(v->as.tables-tables)*PAGE_SIZE; return -ENOMEM;
        }
        v->info.resident+=PAGE_SIZE;
    }
    v->info.resident+=(v->as.tables-tables)*PAGE_SIZE;
    /* Far entry: short jump (patchable XMS convention), INT FE, RETF.
     * Return trampoline for EMS alter-map call is at C800:0010. */
    uint8_t *stub=P2V(v->frames[0xC8]);
    static const uint8_t code[]={0xEB,0,0xCD,0xFE,0xCB};
    memcpy(stub,code,sizeof(code)); stub[16]=0xCD; stub[17]=0xFE;
    int e=as_map(&v->as,0xC8000,v->frames[0xC8],PTE_U);
    if (e) return e;
    /* Dedicated EMS stub: the eight-byte signature must not overwrite the
     * adjacent sixteen-byte interrupt stub. */
    uint8_t *ems=P2V(v->frames[0xF1]);
    ems[0]=0xCD; ems[1]=0xFE; ems[2]=0xCF; memcpy(ems+10,"EMMXXXX0",8);
    /* Vector F100:0000: LIM EMS 4.0 detection reads the name at the handler
     * segment's offset 000Ah, so the stub page must be the segment base
     * (f3-01 review, 2026-10-11); F000:1000 also collided with the INT 33h
     * callback continuation. */
    const uint16_t entry[2]={0x0000,0xF100};
    if (dosvm_write(v,0x67*4,entry,sizeof(entry))) return -EFAULT;
    for (unsigned i=0;i<16;i++) dosvm_page_remove(v,(uint32_t)v->memory.frame_segment*16+i*PAGE_SIZE);
    return 0;
}

void dosvm_memory_close(struct dosvm *v)
{
    if (!v->as.pd_phys) return;
    /* Restore every owned low page, even windows currently unmapped. This
     * removes all EMS aliases; as_destroy then sees each physical page once. */
    for (unsigned i=0;i<DOSVM_PAGES;i++) if (v->frames[i]) {
        uint32_t flags=PTE_U|PTE_W;
        if ((i>=0xC0 && i<0xC9) || (i>=0xF0 && i<0x100)) flags&=~PTE_W;
        as_map(&v->as,i*PAGE_SIZE,v->frames[i],flags);
    }
    v->a20=true;
}

static struct vm_block *xms_handle(struct dosvm *v, unsigned handle)
{ return handle && handle<=VM_MEM_HANDLES && v->memory.xms[handle-1].used ? &v->memory.xms[handle-1] : 0; }
static unsigned xms_free_handles(struct dosvm *v)
{ unsigned n=0; for (unsigned i=0;i<VM_MEM_HANDLES;i++) n+=!v->memory.xms[i].used; return n; }

static uint8_t xms_move(struct dosvm *v, struct v86_frame *f)
{
    uint8_t move[16];
    if (!vm_buffer(v,f->ds,f->tf.esi,sizeof(move),false) ||
        dosvm_read(v,vm_ptr(f->ds,f->tf.esi),move,sizeof(move))) return 0xA7;
    uint32_t len=vm_u32(move), address[2];
    if (len&1) return 0xA7;
    for (unsigned i=0;i<2;i++) {
        unsigned handle=vm_u16(move+4+i*6); uint32_t off=vm_u32(move+6+i*6);
        struct vm_block *b=xms_handle(v,handle);
        if (handle && !b) return i ? 0xA5 : 0xA3;
        if (handle) {
            if (off>b->bytes) return i ? 0xA6 : 0xA4;
            if (len>b->bytes-off) return 0xA7;
            address[i]=b->base+off;
        } else {
            address[i]=vm_far(off);
            if (address[i]>=DOSVM_LINEAR_END) return i ? 0xA6 : 0xA4;
            if (len>DOSVM_LINEAR_END-address[i]) return 0xA7;
        }
    }
    bool a20=v->a20;
    if (dosvm_set_a20(v,true)) return 0x82;
    uint8_t result=0;
    if (!dosvm_range(v,address[0],len,false)) result=0xA4;
    else if (!dosvm_range(v,address[1],len,true)) result=0xA6;
    else if (dosvm_copy_region(v,address[0],address[1],len,false)) result=0xA7;
    if (dosvm_set_a20(v,a20)) result=0x82;
    return result;
}

int dosvm_xms(struct dosvm *v, struct v86_frame *f)
{
    unsigned fn=(f->tf.eax>>8)&255, handle=(uint16_t)f->tf.edx;
    if (fn<=0x12) v->info.xms_calls|=1u<<fn;
    else if (fn==0x88) v->info.xms_calls|=1u<<19;
    else if (fn==0x89) v->info.xms_calls|=1u<<20;
    else if (fn==0x8E) v->info.xms_calls|=1u<<21;
    else if (fn==0x8F) v->info.xms_calls|=1u<<22;
    struct vm_block *b=xms_handle(v,handle);
    uint8_t error=0;
    switch (fn) {
    case 0: vm_low(&f->tf.eax,0x300); vm_low(&f->tf.ebx,0x100); vm_low(&f->tf.edx,1); return 0;
    case 1: if (v->memory.hma) error=0x91; else v->memory.hma=true; break;
    case 2: if (!v->memory.hma) error=0x93; else v->memory.hma=false; break;
    case 3: case 5: {
        uint16_t *count=fn==3 ? &v->memory.global_a20 : &v->memory.local_a20;
        if (*count==0xFFFF || dosvm_set_a20(v,true)) error=0x82; else (*count)++;
        break;
    }
    case 4: case 6: {
        uint16_t *count=fn==4 ? &v->memory.global_a20 : &v->memory.local_a20;
        if (*count) (*count)--;
        if (v->memory.global_a20 || v->memory.local_a20) error=0x94;
        else if (dosvm_set_a20(v,false)) error=0x82;
        break;
    }
    case 7: vm_low(&f->tf.eax,v->a20); f->tf.ebx&=~255u; return 0;
    case 8: case 0x88: {
        uint32_t largest,total; dosvm_pool_query(v,&largest,&total);
        if (fn==8) { vm_low(&f->tf.eax,largest/1024); vm_low(&f->tf.edx,total/1024); }
        else { f->tf.eax=largest/1024; f->tf.edx=total/1024; f->tf.ecx=VM_POOL_BASE+v->memory.quota-1; }
        f->tf.ebx=(f->tf.ebx&~255u)|(total ? 0 : 0xA0); return 0;
    }
    case 9: case 0x89: {
        unsigned i; for (i=0;i<VM_MEM_HANDLES && v->memory.xms[i].used;i++) {}
        uint32_t kb=fn==9 ? (uint16_t)f->tf.edx : f->tf.edx;
        if (i==VM_MEM_HANDLES) error=0xA1;
        else if (kb>v->memory.quota/1024 || !dosvm_pool_alloc(v,&v->memory.xms[i],kb*1024)) error=0xA0;
        vm_low(&f->tf.edx,error ? 0 : i+1); break;
    }
    case 0xA: if (!b) error=0xA2; else if (b->locks) error=0xAB; else dosvm_pool_free(v,b); break;
    case 0xB: error=xms_move(v,f); break;
    case 0xC:
        if (!b) error=0xA2;
        else if (b->locks==255) error=0xAC;
        else { b->locks++; vm_low(&f->tf.edx,b->base>>16); vm_low(&f->tf.ebx,b->base); vm_low(&f->tf.eax,1); return 0; }
        break;
    case 0xD: if (!b) error=0xA2; else if (!b->locks) error=0xAA; else b->locks--; break;
    case 0xE: case 0x8E:
        if (!b) error=0xA2;
        else {
            unsigned free=xms_free_handles(v);
            vm_low(&f->tf.ebx,((uint16_t)b->locks<<8)|(fn==0xE ? free : 0));
            if (fn==0xE) vm_low(&f->tf.edx,b->bytes/1024);
            else { f->tf.edx=b->bytes/1024; vm_low(&f->tf.ecx,free); }
            vm_low(&f->tf.eax,1); return 0;
        }
        break;
    case 0xF: case 0x8F: {
        uint32_t kb=fn==0xF ? (uint16_t)f->tf.ebx : f->tf.ebx;
        if (!b) error=0xA2;
        else if (b->locks) error=0xAB;
        else if (kb>v->memory.quota/1024 || !dosvm_pool_resize(v,b,kb*1024)) error=0xA0;
        break;
    }
    case 0x10: case 0x11: case 0x12:
        error=dosvm_umb(v,f,fn);
        if (!error) { vm_low(&f->tf.eax,1); return 0; }
        break;
    default: error=0x80; break;
    }
    vm_low(&f->tf.eax,!error); f->tf.ebx=(f->tf.ebx&~255u)|error;
    return 0;
}
