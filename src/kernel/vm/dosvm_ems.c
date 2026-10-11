/* LIM EMS 4.0 application interface, with page-table windows and private state.
 * OS/E hardware calls are denied under LIM's A4h policy; no VCPI.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "dosvm_internal.h"
#define EMS_MAP_BYTES 40u
#define EMS_MAP_MAGIC 0x344D4543u

static struct vm_ems_handle *ems_handle(struct dosvm *v, unsigned h)
{ return h && h<=VM_MEM_HANDLES && v->memory.ems[h-1].block.used ? &v->memory.ems[h-1] : 0; }
static bool ems_overlap(uint32_t a,uint32_t b,uint32_t n)
{ return n && (a<b ? b-a<n : a-b<n); }
static bool ems_frame_overlap(struct dosvm *v,uint32_t p,unsigned n)
{ uint32_t frame=(uint32_t)v->memory.frame_segment*16; return n && p<frame+65536 && p+n>frame; }
static bool ems_buffer(struct dosvm *v,uint32_t seg,uint32_t off,unsigned n,bool write)
{ return vm_buffer(v,seg,off,n,write) && !ems_frame_overlap(v,vm_ptr(seg,off),n); }
static unsigned ems_count(struct dosvm *v)
{ unsigned n=0; for (unsigned i=0;i<VM_MEM_HANDLES;i++) n+=v->memory.ems[i].block.used; return n; }
static unsigned ems_total(struct dosvm *v)
{ return (v->memory.quota<VM_EMS_CAP ? v->memory.quota : VM_EMS_CAP)/VM_EMS_PAGE; }
static unsigned ems_free(struct dosvm *v)
{
    uint32_t largest,total; dosvm_pool_query(v,&largest,&total); (void)largest;
    uint32_t remain=ems_total(v)*VM_EMS_PAGE-v->memory.ems_bytes;
    return (total<remain ? total : remain)/VM_EMS_PAGE;
}
static uint8_t ems_map_valid(struct dosvm *v,const struct vm_map map[4])
{
    for (unsigned i=0;i<4;i++) if (map[i].handle) {
        struct vm_ems_handle *h=ems_handle(v,map[i].handle);
        if (!h || map[i].serial!=h->serial) return 0x83;
        if (map[i].page>=h->block.bytes/VM_EMS_PAGE) return 0x8A;
    }
    return 0;
}
static void ems_map_apply(struct dosvm *v,const struct vm_map map[4])
{
    for (unsigned i=0;i<4;i++) for (unsigned j=0;j<4;j++) {
        uint32_t window=(uint32_t)v->memory.frame_segment*16+i*VM_EMS_PAGE+j*PAGE_SIZE;
        if (!map[i].handle) dosvm_page_remove(v,window);
        else {
            struct vm_ems_handle *h=ems_handle(v,map[i].handle);
            uint32_t phys=as_lookup(&v->as,h->block.base+map[i].page*VM_EMS_PAGE+j*PAGE_SIZE,0);
            /* The low page table and all pool backing already exist. */
            as_map(&v->as,window,phys,PTE_U|PTE_W);
        }
    }
    memcpy(v->memory.map,map,sizeof(v->memory.map));
}
static uint8_t ems_pairs(struct dosvm *v,struct vm_map map[4],unsigned handle,unsigned count,uint32_t ptr,bool segments)
{
    if (count>4) return 0x8B;
    struct vm_ems_handle *h=ems_handle(v,handle);
    if (!h) return 0x83;
    uint8_t pairs[16];
    if (dosvm_read(v,ptr,pairs,count*4)) return 0x8B;
    for (unsigned i=0;i<count;i++) {
        unsigned page=vm_u16(pairs+i*4), physical=vm_u16(pairs+i*4+2);
        if (segments) {
            if (physical<v->memory.frame_segment || (physical-v->memory.frame_segment)%0x400) return 0x8B;
            physical=(physical-v->memory.frame_segment)/0x400;
        }
        if (physical>=4) return 0x8B;
        if (page!=0xFFFF && page>=h->block.bytes/VM_EMS_PAGE) return 0x8A;
        map[physical]=(struct vm_map){page==0xFFFF ? 0 : handle,page,h->serial};
    }
    return 0;
}
static void ems_encode(struct dosvm *v,uint8_t *p,const struct vm_map map[4])
{
    vm_w32(p,EMS_MAP_MAGIC); vm_w32(p+4,v->info.key.generation);
    for (unsigned i=0;i<4;i++) {
        vm_w16(p+8+i*8,map[i].handle); vm_w16(p+10+i*8,map[i].page); vm_w32(p+12+i*8,map[i].serial);
    }
}
static uint8_t ems_decode(struct dosvm *v,const uint8_t *p,struct vm_map map[4])
{
    if (vm_u32(p)!=EMS_MAP_MAGIC || vm_u32(p+4)!=v->info.key.generation) return 0x83;
    for (unsigned i=0;i<4;i++) map[i]=(struct vm_map){vm_u16(p+8+i*8),vm_u16(p+10+i*8),vm_u32(p+12+i*8)};
    return ems_map_valid(v,map);
}
static uint8_t ems_allocate(struct dosvm *v,struct v86_frame *f,unsigned pages,bool zero_ok)
{
    if (!pages && !zero_ok) return 0x89;
    if (pages>ems_total(v)) return 0x87;
    if (pages>ems_free(v)) return 0x88;
    unsigned i; for (i=0;i<VM_MEM_HANDLES && v->memory.ems[i].block.used;i++) {}
    if (i==VM_MEM_HANDLES || v->memory.serial==UINT32_MAX) return 0x85;
    struct vm_ems_handle *h=&v->memory.ems[i];
    if (!dosvm_pool_alloc(v,&h->block,pages*VM_EMS_PAGE)) return 0x88;
    h->serial=++v->memory.serial; v->memory.ems_bytes+=pages*VM_EMS_PAGE;
    vm_low(&f->tf.edx,i+1); return 0;
}
static uint8_t ems_move(struct dosvm *v,struct v86_frame *f,bool exchange)
{
    uint8_t request[18];
    if (!ems_buffer(v,f->ds,f->tf.esi,sizeof(request),false) || dosvm_read(v,vm_ptr(f->ds,f->tf.esi),request,sizeof(request))) return 0xA2;
    uint32_t n=vm_u32(request), addr[2];
    if (n>0x100000) return 0x96;
    for (unsigned i=0;i<2;i++) {
        const uint8_t *p=request+4+i*7;
        unsigned type=p[0], handle=vm_u16(p+1), offset=vm_u16(p+3), page=vm_u16(p+5);
        if (type>1) return 0x98;
        if (type) {
            struct vm_ems_handle *h=ems_handle(v,handle);
            if (!h) return 0x83;
            if (page>=h->block.bytes/VM_EMS_PAGE) return 0x8A;
            if (offset>=VM_EMS_PAGE) return 0x95;
            uint32_t off=page*VM_EMS_PAGE+offset;
            if (n>h->block.bytes-off) return 0x93;
            addr[i]=h->block.base+off;
        } else {
            addr[i]=page*16u+offset;
            if (addr[i]>0x100000 || n>0x100000-addr[i]) return 0xA2;
            if (ems_frame_overlap(v,addr[i],n)) return 0x94;
        }
        if (!dosvm_range(v,addr[i],n,i || exchange)) return 0xA2;
    }
    bool overlap=ems_overlap(addr[0],addr[1],n);
    if (exchange && overlap) return 0x97;
    if (dosvm_copy_region(v,addr[0],addr[1],n,exchange)) return 0xA2;
    return overlap ? 0x92 : 0;
}
static uint8_t ems_partial(struct dosvm *v,struct v86_frame *f,unsigned sub)
{
    uint8_t data[50], input[10];
    unsigned count=(uint16_t)f->tf.ebx;
    if (sub==2) { if (count>4) return 0x8B; f->tf.eax=(f->tf.eax&~255u)|(10+10*count); return 0; }
    if (sub>2) return 0x8F;
    uint32_t src=vm_ptr(f->ds,f->tf.esi), dst=vm_ptr(f->es,f->tf.edi);
    if (!ems_buffer(v,f->ds,f->tf.esi,sub==0 ? 2 : 10,false)) return 0x8B;
    if (sub==0) {
        dosvm_read(v,src,input,2); count=vm_u16(input);
        if (count>4 || !ems_buffer(v,f->ds,f->tf.esi,2+2*count,false) ||
            !ems_buffer(v,f->es,f->tf.edi,10+10*count,true)) return 0x8B;
        dosvm_read(v,src,input,2+2*count);
        vm_w32(data,EMS_MAP_MAGIC); vm_w32(data+4,v->info.key.generation); vm_w16(data+8,count);
        for (unsigned i=0;i<count;i++) {
            unsigned seg=vm_u16(input+2+i*2);
            if (seg<v->memory.frame_segment || (seg-v->memory.frame_segment)%0x400) return 0x8B;
            unsigned window=(seg-v->memory.frame_segment)/0x400;
            if (window>=4) return 0x8B;
            /* Opaque map records bind handle lifetimes, including reuse. */
            vm_w16(data+10+i*10,window); vm_w16(data+12+i*10,v->memory.map[window].handle); vm_w16(data+14+i*10,v->memory.map[window].page); vm_w32(data+16+i*10,v->memory.map[window].serial);
        }
        dosvm_write(v,dst,data,10+10*count); return 0;
    }
    dosvm_read(v,src,data,10); count=vm_u16(data+8);
    if (vm_u32(data)!=EMS_MAP_MAGIC || vm_u32(data+4)!=v->info.key.generation || count>4 ||
        !ems_buffer(v,f->ds,f->tf.esi,10+10*count,false)) return 0x8B;
    dosvm_read(v,src,data,10+10*count);
    struct vm_map map[4]; memcpy(map,v->memory.map,sizeof(map));
    for (unsigned i=0;i<count;i++) {
        unsigned window=vm_u16(data+10+i*10), handle=vm_u16(data+12+i*10), page=vm_u16(data+14+i*10);
        if (window>=4) return 0x8B;
        struct vm_ems_handle *h=ems_handle(v,handle);
        if (handle && (!h || h->serial!=vm_u32(data+16+i*10))) return 0x83;
        map[window]=(struct vm_map){handle,page,h ? h->serial : 0};
    }
    uint8_t e=ems_map_valid(v,map); if (!e) ems_map_apply(v,map); return e;
}
static uint8_t ems_alter(struct dosvm *v,struct v86_frame *f,unsigned fn,unsigned sub)
{
    if (fn==0x56 && sub==2) { vm_low(&f->tf.ebx,10); return 0; }
    if (sub>1) return 0x8F;
    uint8_t data[22], frame[10]; unsigned bytes=fn==0x55 ? 9 : 22;
    if (!vm_buffer(v,f->ds,f->tf.esi,bytes,false) || dosvm_read(v,vm_ptr(f->ds,f->tf.esi),data,bytes)) return 0x8B;
    unsigned count=data[4], oldcount=fn==0x56 ? data[9] : 0;
    if (count>4 || oldcount>4) return 0x8B;
    uint32_t ptr=vm_u32(data+5), oldptr=fn==0x56 ? vm_u32(data+10) : 0;
    if (!vm_buffer(v,ptr>>16,(uint16_t)ptr,count*4,false) ||
        (fn==0x56 && !vm_buffer(v,oldptr>>16,(uint16_t)oldptr,oldcount*4,false))) return 0x8B;
    struct vm_map map[4], restore[4]; memcpy(map,v->memory.map,sizeof(map)); memcpy(restore,map,sizeof(map));
    uint8_t e=ems_pairs(v,map,(uint16_t)f->tf.edx,count,vm_far(ptr),sub);
    if (!e && fn==0x56) e=ems_pairs(v,restore,(uint16_t)f->tf.edx,oldcount,vm_far(oldptr),sub);
    if (e) return e;
    unsigned sp=(uint16_t)f->tf.user_esp;
    if (!ems_buffer(v,f->tf.user_ss,sp,6,true)) return 0x8B;
    if (fn==0x56 && (v->memory.depth==VM_EMS_CALLS || sp<10 || !ems_buffer(v,f->tf.user_ss,sp-10,10,true))) return 0x8B;
    struct vm_map before[4]; memcpy(before,v->memory.map,sizeof(before)); ems_map_apply(v,map);
    if (!dosvm_range(v,vm_far(vm_u32(data)),1,false)) { ems_map_apply(v,before); return 0x8B; }
    if (fn==0x55) { dosvm_write(v,vm_ptr(f->tf.user_ss,sp),data,4); return 0; }
    struct vm_ems_call *c=&v->memory.calls[v->memory.depth++];
    memcpy(c->restore,restore,sizeof(restore)); c->ss=f->tf.user_ss; c->sp=sp;
    memcpy(frame,data,4); dosvm_read(v,vm_ptr(c->ss,sp+4),frame+4,2);
    vm_w16(frame+6,16); vm_w16(frame+8,VM_XMS_SEG);
    dosvm_write(v,vm_ptr(c->ss,sp-10),frame,10); vm_low(&f->tf.user_esp,sp-10);
    return 0;
}
int dosvm_ems_return(struct dosvm *v,struct v86_frame *f)
{
    if (!v->memory.depth) return -EFAULT;
    struct vm_ems_call *c=&v->memory.calls[v->memory.depth-1];
    uint8_t frame[6];
    if (f->tf.user_ss!=c->ss || (uint16_t)f->tf.user_esp!=c->sp ||
        !ems_buffer(v,c->ss,c->sp,6,false) || dosvm_read(v,vm_ptr(c->ss,c->sp),frame,6) || ems_map_valid(v,c->restore)) return -EFAULT;
    ems_map_apply(v,c->restore);
    if (!dosvm_range(v,vm_ptr(vm_u16(frame+2),vm_u16(frame)),1,false)) return -EFAULT;
    f->tf.eip=vm_u16(frame); f->tf.cs=vm_u16(frame+2); vm_low(&f->tf.user_esp,c->sp+6);
    unsigned flags=vm_u16(frame+4);
    v->monitor.vif=!!(flags&V86_IF);
    f->tf.eflags=(flags&0xCD5u)|V86_VM|V86_IF|2;
    f->tf.eax&=~0xFF00u; v->memory.depth--; return 0;
}

int dosvm_ems(struct dosvm *v,struct v86_frame *f)
{
    unsigned fn=(f->tf.eax>>8)&255, sub=f->tf.eax&255, handle=(uint16_t)f->tf.edx;
    if (fn>=0x40 && fn<=0x5D) v->info.ems_calls|=1u<<(fn-0x40);
    struct vm_ems_handle *h=ems_handle(v,handle);
    uint8_t error=0, data[VM_MEM_HANDLES*10];
    struct vm_map map[4];
    memcpy(map,v->memory.map,sizeof(map));
    switch (fn) {
    case 0x40: break;
    case 0x41: vm_low(&f->tf.ebx,v->memory.frame_segment); break;
    case 0x42: vm_low(&f->tf.ebx,ems_free(v)); vm_low(&f->tf.edx,ems_total(v)); break;
    case 0x43: error=ems_allocate(v,f,(uint16_t)f->tf.ebx,false); break;
    case 0x44: {
        unsigned window=sub, page=(uint16_t)f->tf.ebx;
        if (window>=4) error=0x8B;
        else if (!h) error=0x83;
        else if (page!=0xFFFF && page>=h->block.bytes/VM_EMS_PAGE) error=0x8A;
        else { map[window]=(struct vm_map){page==0xFFFF ? 0 : handle,page,h->serial}; ems_map_apply(v,map); }
        break;
    }
    case 0x45:
        if (!h) error=0x83;
        else {
            for (unsigned i=0;i<4;i++) if (map[i].handle==handle) map[i]=(struct vm_map){0};
            ems_map_apply(v,map);
            v->memory.ems_bytes-=h->block.bytes; dosvm_pool_free(v,&h->block); memset(h,0,sizeof(*h));
        }
        break;
    case 0x46: f->tf.eax=(f->tf.eax&~255u)|0x40; break;
    case 0x47:
        if (!h) error=0x83; else if (h->saved) error=0x8D;
        else { memcpy(h->map,map,sizeof(map)); h->saved=true; } break;
    case 0x48:
        if (!h) error=0x83; else if (!h->saved) error=0x8E;
        else { error=ems_map_valid(v,h->map); if (!error) { ems_map_apply(v,h->map); h->saved=false; } } break;
    case 0x49: case 0x4A: error=0x84; break;
    case 0x4B: vm_low(&f->tf.ebx,ems_count(v)); break;
    case 0x4C: if (!h) error=0x83; else vm_low(&f->tf.ebx,h->block.bytes/VM_EMS_PAGE); break;
    case 0x4D: case 0x54:
        if (fn==0x54 && sub==2) { vm_low(&f->tf.ebx,VM_MEM_HANDLES); break; }
        if (fn==0x54 && sub==1) {
            if (!ems_buffer(v,f->ds,f->tf.esi,8,false)) { error=0xA0; break; }
            dosvm_read(v,vm_ptr(f->ds,f->tf.esi),data,8);
            const char zero[8]={0};
            if (!memcmp(data,zero,8)) { error=0xA1; break; }
            unsigned i; for (i=0;i<VM_MEM_HANDLES;i++) if (v->memory.ems[i].block.used && !memcmp(v->memory.ems[i].name,data,8)) break;
            if (i==VM_MEM_HANDLES) error=0xA0; else vm_low(&f->tf.edx,i+1);
            break;
        }
        if (fn==0x54 && sub) { error=0x8F; break; }
        {
            unsigned n=0, stride=fn==0x4D ? 4 : 10;
            for (unsigned i=0;i<VM_MEM_HANDLES;i++) if (v->memory.ems[i].block.used) {
                vm_w16(data+n*stride,i+1);
                if (stride==4) vm_w16(data+n*stride+2,v->memory.ems[i].block.bytes/VM_EMS_PAGE);
                else memcpy(data+n*stride+2,v->memory.ems[i].name,8);
                n++;
            }
            if (!ems_buffer(v,f->es,f->tf.edi,n*stride,true)) { error=0x8B; break; }
            dosvm_write(v,vm_ptr(f->es,f->tf.edi),data,n*stride);
            if (fn==0x4D) vm_low(&f->tf.ebx,n); else f->tf.eax=(f->tf.eax&~255u)|n;
        }
        break;
    case 0x4E:
        if (sub==3) { f->tf.eax=(f->tf.eax&~255u)|EMS_MAP_BYTES; break; }
        if (sub>3) { error=0x8F; break; }
        if ((sub!=0 && !ems_buffer(v,f->ds,f->tf.esi,EMS_MAP_BYTES,false)) ||
            (sub!=1 && !ems_buffer(v,f->es,f->tf.edi,EMS_MAP_BYTES,true))) { error=0x8B; break; }
        if (sub!=0) { dosvm_read(v,vm_ptr(f->ds,f->tf.esi),data,EMS_MAP_BYTES); error=ems_decode(v,data,map); }
        if (!error) {
            if (sub!=1) { ems_encode(v,data,v->memory.map); dosvm_write(v,vm_ptr(f->es,f->tf.edi),data,EMS_MAP_BYTES); }
            if (sub!=0) ems_map_apply(v,map);
        }
        break;
    case 0x4F: error=ems_partial(v,f,sub); break;
    case 0x50:
        if (sub>1) error=0x8F;
        else if ((uint16_t)f->tf.ecx>4 || !ems_buffer(v,f->ds,f->tf.esi,(uint16_t)f->tf.ecx*4,false)) error=0x8B;
        else { error=ems_pairs(v,map,handle,(uint16_t)f->tf.ecx,vm_ptr(f->ds,f->tf.esi),sub); if (!error) ems_map_apply(v,map); }
        break;
    case 0x51:
        if (!h) error=0x83;
        else {
            unsigned pages=(uint16_t)f->tf.ebx, old=h->block.bytes/VM_EMS_PAGE;
            if (pages>ems_total(v)) error=0x87;
            else if (pages>old && pages-old>ems_free(v)) error=0x88;
            else {
                /* Remove aliases before relocation/free; reapply on failure. */
                struct vm_map empty[4]={{0}}; ems_map_apply(v,empty);
                if (!dosvm_pool_resize(v,&h->block,pages*VM_EMS_PAGE)) error=0x88;
                else {
                    v->memory.ems_bytes=(v->memory.ems_bytes-old*VM_EMS_PAGE)+pages*VM_EMS_PAGE;
                    for (unsigned i=0;i<4;i++) if (map[i].handle==handle && map[i].page>=pages) map[i]=(struct vm_map){0};
                }
                ems_map_apply(v,map);
            }
        }
        break;
    case 0x52:
        if (sub==2) f->tf.eax&=~255u; /* no nonvolatile RAM */
        else if (sub>2) error=0x8F;
        else if (!h) error=0x83;
        else if (!sub) f->tf.eax&=~255u;
        else if ((f->tf.ebx&255)>1) error=0x90;
        else if (f->tf.ebx&255) error=0x91;
        break;
    case 0x53:
        if (sub>1) error=0x8F;
        else if (!h) error=0x83;
        else if (!sub) {
            if (!ems_buffer(v,f->es,f->tf.edi,8,true)) error=0x8B;
            else dosvm_write(v,vm_ptr(f->es,f->tf.edi),h->name,8);
        } else {
            if (!ems_buffer(v,f->ds,f->tf.esi,8,false)) { error=0x8B; break; }
            dosvm_read(v,vm_ptr(f->ds,f->tf.esi),data,8);
            const char zero[8]={0};
            for (unsigned i=0;i<VM_MEM_HANDLES;i++) if (&v->memory.ems[i]!=h && v->memory.ems[i].block.used && memcmp(data,zero,8) && !memcmp(v->memory.ems[i].name,data,8)) error=0xA1;
            if (!error) memcpy(h->name,data,8);
        }
        break;
    case 0x55: case 0x56: error=ems_alter(v,f,fn,sub); break;
    case 0x57: error=sub>1 ? 0x8F : ems_move(v,f,sub); break;
    case 0x58:
        if (sub>1) error=0x8F;
        else {
            if (!sub) {
                for (unsigned i=0;i<4;i++) { vm_w16(data+i*4,v->memory.frame_segment+i*0x400); vm_w16(data+i*4+2,i); }
                if (!ems_buffer(v,f->es,f->tf.edi,16,true)) { error=0x8B; break; }
                dosvm_write(v,vm_ptr(f->es,f->tf.edi),data,16);
            }
            vm_low(&f->tf.ecx,4);
        }
        break;
    case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: error=0xA4; break;
    default: error=0x84; break;
    }
    f->tf.eax=(f->tf.eax&~0xFF00u)|((uint32_t)error<<8);
    return 0;
}
