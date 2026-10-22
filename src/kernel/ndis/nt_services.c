/* Exact NT/HAL union for the three UIU hashes, not the broad pack union.
 * Unsupported objects fault this adapter; optional absent files return NT
 * errors. No fallback success stubs. SPDX-License-Identifier: GPL-2.0-only */
#include "nt_services.h"
#include "user_memory.h"
#include <ciuki/kernel.h>
struct spec { const char *module,*name; enum ndis_abi abi; uint16_t bytes; unsigned args; };
#define N(n,a,b,c) {"NTOSKRNL.EXE",n,a,b,c}
#define H(n,a,b,c) {"HAL.DLL",n,a,b,c}
static const struct spec specs[]={
    N("InterlockedPopEntrySList",NDIS_ABI_FASTCALL,0,1),
    N("InterlockedPushEntrySList",NDIS_ABI_FASTCALL,0,2),
    N("IoFreeMdl",NDIS_ABI_STDCALL,4,1),
    N("IofCompleteRequest",NDIS_ABI_FASTCALL,0,2),
    N("KeBugCheckEx",NDIS_ABI_STDCALL,20,5),
    N("KeInitializeSpinLock",NDIS_ABI_STDCALL,4,1),
    N("KeQueryTimeIncrement",NDIS_ABI_STDCALL,0,0),
    N("KefAcquireSpinLockAtDpcLevel",NDIS_ABI_FASTCALL,0,1),
    N("KefReleaseSpinLockFromDpcLevel",NDIS_ABI_FASTCALL,0,1),
    N("MmMapLockedPagesSpecifyCache",NDIS_ABI_STDCALL,24,6),
    N("RtlAppendUnicodeStringToString",NDIS_ABI_STDCALL,8,2),
    N("RtlCopyUnicodeString",NDIS_ABI_STDCALL,8,2),
    N("RtlInitUnicodeString",NDIS_ABI_STDCALL,8,2),
    N("WRITE_REGISTER_UCHAR",NDIS_ABI_STDCALL,8,2),
    N("WRITE_REGISTER_ULONG",NDIS_ABI_STDCALL,8,2),
    N("WRITE_REGISTER_USHORT",NDIS_ABI_STDCALL,8,2),
    N("ZwClose",NDIS_ABI_STDCALL,4,1),
    N("ZwOpenFile",NDIS_ABI_STDCALL,24,6),
    N("_alldiv",NDIS_ABI_STDCALL,16,4),
    N("_allmul",NDIS_ABI_STDCALL,16,4),
    N("strncpy",NDIS_ABI_CDECL,0,3),
    H("KeGetCurrentIrql",NDIS_ABI_STDCALL,0,0),
    H("KeStallExecutionProcessor",NDIS_ABI_STDCALL,4,1),
    H("KfAcquireSpinLock",NDIS_ABI_FASTCALL,0,1),
    H("KfReleaseSpinLock",NDIS_ABI_FASTCALL,0,2),
    H("READ_PORT_UCHAR",NDIS_ABI_STDCALL,4,1),
    H("READ_PORT_ULONG",NDIS_ABI_STDCALL,4,1),
    H("READ_PORT_USHORT",NDIS_ABI_STDCALL,4,1),
    H("WRITE_PORT_UCHAR",NDIS_ABI_STDCALL,8,2),
    H("WRITE_PORT_ULONG",NDIS_ABI_STDCALL,8,2),
    H("WRITE_PORT_USHORT",NDIS_ABI_STDCALL,8,2)
};
#undef N
#undef H
int ndis_nt_veneers(void *page,uint32_t bytes,uint32_t base,uint32_t tick,
                     struct ndis_user_export *out,unsigned capacity,unsigned *count)
{
    const unsigned functions=sizeof(specs)/sizeof(specs[0]);
    if(!page || !out || !count || capacity<functions+1 || bytes<functions*32 ||
       base<0x10000 || base>=0xc0000000 || bytes>0xc0000000-base || tick<0x10000 || tick>0xbffffff4) return 0;
    size_t total;const struct ndis_export *table=ndis_exports(&total);
    *count=0;
    for(unsigned i=0;i<functions;i++) {
        const struct spec *s=&specs[i];
        const struct ndis_export *e=ndis_export_find(table,total,s->module,s->name,0);
        if(!e || e->abi!=s->abi || e->stack_bytes!=s->bytes) return 0;
        if(!e->implemented) continue;
        uint8_t *p=(uint8_t *)page+i*32;unsigned n=0;p[n++]=0x53; /* push ebx */
        if(s->abi==NDIS_ABI_FASTCALL) { p[n++]=0x52;p[n++]=0x51;p[n++]=0x89;p[n++]=0xe1; }
        else { p[n++]=0x8d;p[n++]=0x4c;p[n++]=0x24;p[n++]=8; }
        p[n++]=0xbb;pe_put32(p+n,i);n+=4;
        p[n++]=0xb8;pe_put32(p+n,0x4e440009);n+=4;
        p[n++]=0xcd;p[n++]=0x80;
        if(s->abi==NDIS_ABI_FASTCALL) { p[n++]=0x83;p[n++]=0xc4;p[n++]=8; }
        p[n++]=0x5b;
        if(s->abi!=NDIS_ABI_CDECL && s->bytes) { p[n++]=0xc2;pe_put16(p+n,s->bytes);n+=2; }
        else p[n++]=0xc3;
        out[(*count)++]=(struct ndis_user_export){e,base+i*32,n};
    }
    const struct ndis_export *e=ndis_export_find(table,total,"NTOSKRNL.EXE","KeTickCount",0);
    if(!e || !e->implemented || e->kind!=NDIS_DATA) return 0;
    out[(*count)++]=(struct ndis_user_export){e,tick,12};return 1;
}
static uint64_t fault(struct ndis_nt_services *s,const char *why)
{
    struct ndis_nt_runtime *r=s?s->runtime:0;
    if(r) ndis_host_stop(r->host,why,0,0,r->ops.now(r->ops.context)/10);
    return NDIS_NT_INVALID;
}
static int rd(struct ndis_nt_services *s,uint32_t at,void *out,uint32_t bytes)
{ return s->runtime->ops.read(s->runtime->ops.context,at,out,bytes); }
static int wr(struct ndis_nt_services *s,uint32_t at,const void *in,uint32_t bytes)
{ return s->runtime->ops.write(s->runtime->ops.context,at,in,bytes); }
static uint64_t divide(struct ndis_nt_services *s,uint64_t a,uint64_t b)
{
    if(!b || (a==0x8000000000000000ull && b==UINT64_MAX)) return fault(s,"nt-alldiv");
    bool neg=!!(a>>63)^!!(b>>63);if(a>>63) a=0-a;if(b>>63) b=0-b;
    uint64_t q=0,rem=0;
    for(unsigned bit=64;bit--;) {
        bool carry=!!(rem>>63);rem=(rem<<1)|((a>>bit)&1);
        if(carry || rem>=b) { rem-=b;q|=1ull<<bit; }
    }
    return neg?0-q:q;
}
uint64_t ndis_nt_service(struct ndis_nt_services *s,uint32_t op,uint32_t arguments)
{
    if(!s || !s->runtime || !ndis_host_live(s->runtime->host)) return NDIS_NT_CANCELLED;
    struct ndis_nt_runtime *r=s->runtime;uint32_t a[6]={0};
    if(op>=sizeof(specs)/sizeof(specs[0]) || (specs[op].args && !rd(s,arguments,a,specs[op].args*4))) return fault(s,"nt-service-arguments");
    uint8_t old=0;
    switch(op) {
    case 0:case 1: {
        uint8_t header[8];uint32_t next;
        if((a[0]&7) || !rd(s,a[0],header,8)) return fault(s,"nt-slist-header");
        uint32_t first=pe_u32(header);uint16_t depth=pe_u16(header+4),seq=pe_u16(header+6);
        if(op==1) {
            if(!a[1] || depth==65535 || !wr(s,a[1],&first,4)) return fault(s,"nt-slist-push");
            next=a[1];depth++;
        } else {
            if(!first) return 0;
            if(!depth || !rd(s,first,&next,4)) return fault(s,"nt-slist-pop");depth--;
        }
        pe_put32(header,next);pe_put16(header+4,depth);pe_put16(header+6,(uint16_t)(seq+1));
        if(!wr(s,a[0],header,8)) return fault(s,"nt-slist-write");return first;
    }
    case 2:return ndis_nt_free_mdl(r,a[0])?0:NDIS_NT_INVALID;
    case 3:
        if(r->host->irql>2) return fault(s,"nt-irp-irql");
        for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->lock[i].held) return fault(s,"nt-irp-spinlock");
        for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(s->irp[i].live && s->irp[i].address==a[0] &&
            s->irp[i].generation==r->host->generation) {
            uint32_t status[2];
            if(!rd(s,s->irp[i].status_address,status,8) || status[0]==0x103 ||
               status[1]>s->irp[i].maximum_information) return fault(s,"nt-irp-status");
            void (*complete)(void *,int32_t,uint32_t)=s->irp[i].complete;void *ctx=s->irp[i].context;
            s->irp[i].live=false;complete(ctx,(int32_t)status[0],status[1]);return 0;
        }
        return fault(s,"nt-irp-not-owned");
    case 4:return fault(s,"driver-bugcheck");
    case 5:return ndis_nt_lock_init(r,a[0])?0:NDIS_NT_INVALID;
    case 6:return 10000; /* actual PIT service: 1 ms in 100 ns units */
    case 7:return ndis_nt_lock(r,a[0],true,&old)?0:NDIS_NT_INVALID;
    case 8:return ndis_nt_unlock(r,a[0],true,0)?0:NDIS_NT_INVALID;
    case 9:
        if(a[4]) return fault(s,"nt-mdl-bugcheck-policy");
        return ndis_nt_map_mdl(r,a[0],a[1],a[2],a[3]);
    case 10:return ndis_nt_unicode_append(r,a[0],a[1],true);
    case 11:
        if(!a[1]) { struct ndis_nt_string empty;
            if(!rd(s,a[0],&empty,8)) return fault(s,"nt-unicode-null");
            empty.length=0;return wr(s,a[0],&empty,8)?0:fault(s,"nt-unicode-null"); }
        return ndis_nt_unicode_append(r,a[0],a[1],false);
    case 12: {
        uint32_t length=0;uint16_t ch=0;
        if(a[1]) for(;;) {
            if(length>=4096 || a[1]>UINT32_MAX-length || !rd(s,a[1]+length,&ch,2)) return fault(s,"nt-unicode-init");
            if(!ch) break;length+=2;
        }
        struct ndis_nt_string str={(uint16_t)length,a[1]?(uint16_t)(length+2):0,a[1]};
        return wr(s,a[0],&str,8)?0:fault(s,"nt-unicode-init-write");
    }
    case 13:case 14:case 15: {
        unsigned width=op==13?1:op==14?4:2;
        return s->mmio && s->mmio(s->context,a[0],width,a[1])?0:fault(s,"nt-mmio-ownership");
    }
    case 16:
        if(r->host->irql) return fault(s,"nt-file-irql");
        for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(s->handle[i].live && s->handle[i].value==a[0]) { s->handle[i].live=false;return 0; }
        return 0xc0000008u;
    case 17: {
        if(r->host->irql) return fault(s,"nt-file-irql");
        uint32_t attrs[6];struct ndis_nt_string name;
        /* Only declared read-only support files; no root handle, arbitrary
         * device namespace, writes, delete-on-close or kernel handles. */
        if(!rd(s,a[2],attrs,24) || attrs[0]!=24 || attrs[1] || !rd(s,attrs[2],&name,8) ||
           (name.length&1) || name.length>name.maximum || name.length>512) return NDIS_NT_INVALID;
        uint32_t status=NDIS_NT_NOT_FOUND,handle=0,information=0;
        if(a[1]&~(0x80000000u|0x00120089u) || a[5]&~(0x20u|0x40u|0x4000u) || a[4]&~7u) status=0xc0000022u;
        else for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(s->file[i].present && s->file[i].bytes==name.length) {
            bool same=true;uint16_t left,right;
            for(unsigned j=0;j<name.length;j+=2) {
                if(!rd(s,name.buffer+j,&left,2)) { same=false;break; }
                right=s->file[i].name[j/2];
                if(attrs[3]&0x40) { if(left>='a' && left<='z') left-=32;if(right>='a' && right<='z') right-=32; }
                if(left!=right) { same=false;break; }
            }
            unsigned slot=0;while(slot<NDIS_NT_OBJECTS && s->handle[slot].live) slot++;
            bool sharing=true;
            for(unsigned j=0;j<NDIS_NT_OBJECTS;j++) if(s->handle[j].live && s->handle[j].file==i &&
                (!(a[4]&1) || !(s->handle[j].share&1))) sharing=false;
            if(same && !sharing) { status=0xc0000043u;break; }
            if(same && s->next_handle<UINT32_MAX && slot<NDIS_NT_OBJECTS) {
                handle=++s->next_handle;s->handle[slot].value=handle;s->handle[slot].file=i;s->handle[slot].live=true;
                s->handle[slot].share=a[4];
                status=0;information=1;break;
            }
            if(same) { status=0xc000009au;break; }
        }
        uint32_t iosb[2]={status,information};
        if(!wr(s,a[0],&handle,4) || !wr(s,a[3],iosb,8)) return fault(s,"nt-file-output");return status;
    }
    case 18:return divide(s,(uint64_t)a[0]|(uint64_t)a[1]<<32,(uint64_t)a[2]|(uint64_t)a[3]<<32);
    case 19:return ((uint64_t)a[0]|(uint64_t)a[1]<<32)*((uint64_t)a[2]|(uint64_t)a[3]<<32);
    case 20: {
        if(a[2]>4096 || a[0]>UINT32_MAX-a[2] || a[1]>UINT32_MAX-a[2]) return fault(s,"nt-strncpy-bounds");
        bool end=false;for(uint32_t i=0;i<a[2];i++) {
            uint8_t ch=0;if(!end && !rd(s,a[1]+i,&ch,1)) return fault(s,"nt-strncpy-source");
            if(!ch) end=true;if(!wr(s,a[0]+i,&ch,1)) return fault(s,"nt-strncpy-destination");
        }
        return a[0];
    }
    case 21:return r->host->irql;
    case 22:
        return s->stall && !s->stall(s->context,a[0])?0:fault(s,"nt-stall-unqualified");
    case 23:return ndis_nt_lock(r,a[0],false,&old)?old:NDIS_NT_INVALID;
    case 24:return ndis_nt_unlock(r,a[0],false,(uint8_t)a[1])?0:NDIS_NT_INVALID;
    default: {
        unsigned width=(op==25 || op==28)?1:(op==26 || op==29)?4:2;bool ok=false;
        uint32_t result=s->port?s->port(s->context,a[0],width,op>=28,a[1],&ok):0;
        return ok?result:fault(s,"nt-port-ownership");
    }
    }
}

int ndis_nt_irp_register(struct ndis_nt_services *s,uint32_t address,uint32_t status_address,
                         uint32_t maximum,void *ctx,void (*complete)(void *,int32_t,uint32_t))
{
    uint32_t status[2];
    if(!s || !s->runtime || !ndis_host_live(s->runtime->host) || !complete || !address ||
       !rd(s,status_address,status,8)) return 0;
    unsigned slot=NDIS_NT_OBJECTS;
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) {
        if(s->irp[i].live && s->irp[i].address==address) return 0;
        if(!s->irp[i].live && slot==NDIS_NT_OBJECTS) slot=i;
    }
    if(slot==NDIS_NT_OBJECTS) return 0;
    s->irp[slot]=(struct ndis_nt_irp){address,status_address,s->runtime->host->generation,maximum,ctx,complete,true};return 1;
}
void ndis_nt_services_stop(struct ndis_nt_services *s)
{
    if(!s) return;
    for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) {
        s->handle[i].live=false;
        if(s->irp[i].live) {
            s->irp[i].live=false;
            s->irp[i].complete(s->irp[i].context,(int32_t)NDIS_NT_CANCELLED,0);
        }
    }
}
