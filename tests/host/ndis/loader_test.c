/* SPDX-License-Identifier: GPL-2.0-only
 * Only authored synthetic images. These tests never open a driver pack. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/kernel/ndis/callback.h"
extern int user_accepts(const void *,uint32_t);
#define O 0x98u
#define T (O+224u)
#define BASE 0xc1000000u
static uint8_t raw[0x2400];
static unsigned allocations, frees, live, records, fences, quarantines, entries, exits;
static bool resident_ok=true, allocation_ok=true, fence_ok=true, claims_ok=true;
static uint32_t mapped_base=BASE;
static char last_record[512];
static void put(uint32_t at,uint32_t v) { pe_put32(raw+at,v); }
static uint32_t at(uint32_t rva) { return rva-0x2000+0x400; }
static void dir(unsigned i,uint32_t rva,uint32_t size) { put(O+96+i*8,rva); put(O+100+i*8,size); }
static void fixture(void) {
    memset(raw,0,sizeof(raw)); raw[0]='M';raw[1]='Z';put(60,0x80);put(0x80,0x4550);
    pe_put16(raw+0x84,0x14c);pe_put16(raw+0x86,2);pe_put16(raw+0x94,224);pe_put16(raw+0x96,0x102);
    pe_put16(raw+O,0x10b);put(O+16,0x1000);put(O+28,BASE);put(O+32,4096);put(O+36,512);
    put(O+56,0x4000);put(O+60,512);pe_put16(raw+O+68,1);put(O+92,16);
    put(T+8,512);put(T+12,0x1000);put(T+16,512);put(T+20,512);put(T+36,0x60000020);
    put(T+48,8192);put(T+52,0x2000);put(T+56,8192);put(T+60,1024);put(T+76,0xc0000040);
    memset(raw+512,0x90,512); raw[512]=0xc3;
}
static void import(const char *module,const char *name,uint32_t ordinal,bool fallback,bool bound) {
    dir(1,0x2000,40);put(at(0x2000),fallback?0:0x2100);
    put(at(0x2004),bound?123:0);put(at(0x200c),0x2080);put(at(0x2010),0x2120);
    strcpy((char *)raw+at(0x2080),module);
    uint32_t v=name?0x2140:0x80000000u|ordinal;
    put(at(0x2100),v);put(at(0x2120),bound&&!fallback?0xc1234567:v);
    if(name) strcpy((char *)raw+at(0x2142),name);
}
static void *allocate(void *ctx,uint32_t preferred,uint32_t n,uint32_t *base) {
    (void)ctx;(void)preferred;
    if(!allocation_ok) return 0;
    void *p=malloc(n);assert(p);allocations++;live++;*base=mapped_base;return p;
}
static void release(void *ctx,void *p,uint32_t n) {
    (void)ctx;(void)n;assert(live);live--;frees++;free(p);
}
static int resident(void *ctx,void *p,uint32_t n,uint32_t base) {
    (void)ctx;(void)p;(void)n;(void)base;return resident_ok;
}
static void record(void *ctx,const char *file,const char *module,const char *name,uint16_t ordinal,const char *reason) {
    (void)ctx;records++;
    if(module && name) snprintf(last_record,sizeof(last_record),"[ndis] load refused driver=%s import=%s!%s reason=%s",file,module,name,reason);
    else if(module) snprintf(last_record,sizeof(last_record),"[ndis] load refused driver=%s import=%s!#%u reason=%s",file,module,ordinal,reason);
}
static const struct ndis_export implemented[]={
    {"NDIS.SYS","FixtureFunction",11,8,NDIS_ABI_STDCALL,NDIS_FUNCTION,0xc2001000,true},
    {"NTOSKRNL.EXE","KeTickCount",42,0,NDIS_ABI_DATA,NDIS_DATA,0xc2002000,true},
    {"HAL.DLL","KfRaiseIrql",7,0,NDIS_ABI_FASTCALL,NDIS_FUNCTION,0xc2003000,true}
};
static struct ntimage_platform platform(void) {
    return (struct ntimage_platform){.memory={.allocate=allocate,.release=release},.resident=resident,
        .record=record,.exports=implemented,.export_count=3,.image_budget=65536};
}
static void reject(const char *why) {
    struct ntimage d={0};struct ntimage_platform p=platform();unsigned before=live;
    assert(!ntimage_load(&d,raw,sizeof(raw),"synthetic.sys",&p));
    if(!d.error.reason || strcmp(d.error.reason,why)) fprintf(stderr,"expected %s, got %s\n",why,d.error.reason?d.error.reason:"NULL");
    assert(d.error.reason && !strcmp(d.error.reason,why));assert(live==before && !d.loaded && !d.pe.image);
}
static void policies(void) {
    fixture();assert(!user_accepts(raw,sizeof(raw)));
    struct ntimage d={0};struct ntimage_platform p=platform();
    assert(ntimage_load(&d,raw,sizeof(raw),"synthetic.sys",&p));
    assert(!ndis_execution_available());assert(!ntimage_enter(&d,0,0));assert(!live && !d.loaded);
    pe_put16(raw+O+68,3);put(O+28,0x400000);assert(user_accepts(raw,sizeof(raw)));reject("kernel-requires-native-subsystem");
    fixture();put(O+16,0x2000);reject("driver-entry-not-executable");
    fixture();put(O+16,0x100);reject("driver-entry-not-executable");
    fixture();dir(9,0x2400,24);reject("driver-tls-unsupported");
    fixture();dir(13,0x2400,64);reject("driver-delay-imports-unsupported");
    fixture();put(T+60,0x600);reject("driver-section-truncated");
    fixture();p.image_budget=4096;d=(struct ntimage){0};assert(!ntimage_load(&d,raw,sizeof(raw),"budget.sys",&p));
    assert(!strcmp(d.error.reason,"network-image-budget"));
}
static void hostile(void) {
    fixture();raw[515]=0xfa;
    struct ntimage diagnostic={0};struct ntimage_platform pp=platform();
    assert(ntimage_load(&diagnostic,raw,sizeof(raw),"diagnostic.sys",&pp));
    assert(!diagnostic.scanned && diagnostic.scan_rva==0x1003);
    assert(ntimage_unload(&diagnostic));
    fixture();put(T+52,0x1000);reject("section-virtual-overlap");
    fixture();put(T+60,512);reject("driver-raw-overlap");
    fixture();put(T+20,0);reject("driver-raw-header-overlap");
    fixture();dir(1,0x3ff0,40);reject("directory-image-bounds");
    fixture();dir(3,0x1800,40);reject("rva-unmapped");
    fixture();dir(5,0x2700,12);put(at(0x2700),0xfffff000);put(at(0x2704),12);pe_put16(raw+at(0x2708),0x3fff);
    reject("relocation-target-bounds");
    fixture();dir(5,0x2700,12);put(at(0x2700),0x2000);put(at(0x2704),9);reject("relocation-block-size");
    fixture();import("NDIS.SYS","FixtureFunction",0,false,false);put(at(0x2100),0x8100000b);reject("import-ordinal-reserved-bits");
    fixture();import("NDIS.SYS","FixtureFunction",0,false,false);put(at(0x2100),0x3fff);reject("rva-bounds");
    fixture();import("NDIS.SYS","FixtureFunction",0,false,false);dir(1,0x2000,20);reject("import-descriptors-unterminated");
    fixture();allocation_ok=false;reject("image-map-failed");allocation_ok=true;
    fixture();resident_ok=false;reject("image-not-supervisor-pinned");resident_ok=true;
    fixture();mapped_base=0x100000;reject("image-not-supervisor-pinned");mapped_base=BASE;
    fixture();mapped_base=BASE+0x10000;reject("relocation-base-collision");mapped_base=BASE;
    fixture();mapped_base=0xfffff000;reject("image-address-wrap");mapped_base=BASE;
}
static void imports(void) {
    struct ntimage_platform p=platform();
    for(unsigned mode=0;mode<4;mode++) {
        fixture();import("ndis.sys",mode==1?0:"FixtureFunction",11,mode==2,mode==3);
        struct ntimage d={0};assert(ntimage_load(&d,raw,sizeof(raw),"synthetic.sys",&p));
        assert(pe_u32(d.pe.image+0x2120)==0xc2001000);assert(ntimage_unload(&d));assert(!live);
    }
    fixture();import("NTOSKRNL.EXE","KeTickCount",0,false,false);
    struct ntimage d={0};assert(ntimage_load(&d,raw,sizeof(raw),"data.sys",&p));
    assert(pe_u32(d.pe.image+0x2120)==0xc2002000);assert(ntimage_unload(&d));
    fixture();import("NTOSKRNL.EXE",0,42,false,false);d=(struct ntimage){0};
    assert(ntimage_load(&d,raw,sizeof(raw),"ordinal-data.sys",&p));
    assert(pe_u32(d.pe.image+0x2120)==0xc2002000);assert(ntimage_unload(&d));
    fixture();import("HAL.DLL","KfRaiseIrql",0,false,false);d=(struct ntimage){0};
    assert(ntimage_load(&d,raw,sizeof(raw),"fastcall.sys",&p));
    assert(pe_u32(d.pe.image+0x2120)==0xc2003000);assert(ntimage_unload(&d));
    fixture();import("NDIS.SYS","Unknown",0,false,false);reject("unknown-export");
    assert(strstr(last_record,"driver=synthetic.sys import=NDIS.SYS!Unknown reason=unknown-export"));
    fixture();import("NDIS.SYS",0,99,false,false);reject("unknown-export");assert(strstr(last_record,"NDIS.SYS!#99"));
    fixture();import("kernel32.dll","FixtureFunction",0,false,false);reject("unknown-module");
    fixture();import("NDIS.SYS","fixturefunction",0,false,false);reject("unknown-export");
    fixture();import("NDIS.SYS","FixtureFunction",0,true,true);put(at(0x2120),0xc1234567);reject("bound-iat-without-names");
    /* A later bad symbol rolls back even after the first IAT slot was patched. */
    fixture();import("NDIS.SYS","FixtureFunction",0,false,false);put(at(0x2104),0x80000063);put(at(0x2124),0x80000063);
    reject("unknown-export");assert(!live);
    size_t n;const struct ndis_export *table=ndis_exports(&n);assert(n);
    p.exports=0;
    for(size_t i=0;i<n;i++) {
        assert(!table[i].implemented && !table[i].address && !table[i].ordinal);
        fixture();import(table[i].module,table[i].name,0,false,false);d=(struct ntimage){0};
        assert(!ntimage_load(&d,raw,sizeof(raw),"unimplemented.sys",&p));
        assert(!strcmp(d.error.reason,"unimplemented-export"));assert(!live);
    }
    uint16_t routine[]={'K','f','R','a','i','s','e','I','r','q','l'};
    assert(ndis_routine_address(implemented,3,routine,sizeof(routine),false)==0xc2003000);
    assert(!ndis_routine_address(implemented,3,routine,sizeof(routine),true));
    assert(!ndis_mm_get_system_routine_address(routine,sizeof(routine)));
    assert(!ndis_get_routine_address(routine,sizeof(routine)));
    assert(!ndis_routine_address(implemented,3,routine,3,false));
    routine[1]=0x100;assert(!ndis_routine_address(implemented,3,routine,sizeof(routine),false));
}
static int corrupt_next(void *ctx,const struct pe_import *i) {
    struct pe_image *p=ctx;
    if(i->descriptor==0) pe_put32(p->image+0x2014,0xfffffffcu);
    return 1;
}
static void modified_imports(void) {
    fixture();import("NDIS.SYS","FixtureFunction",0,false,false);
    dir(1,0x2000,60);memcpy(raw+at(0x2014),raw+at(0x2000),20);
    struct pe_image p;struct pe_error error={0};
    assert(pe_parse(&p,raw,sizeof(raw),&error,0));
    void *image=malloc(p.size);assert(image);assert(pe_copy_image(&p,image));
    assert(pe_validate_mechanism(&p));
    assert(!pe_walk_imports(&p,&p,0,corrupt_next));assert(!strcmp(error.reason,"rva-bounds"));
    free(image);
}
static void relocations(void) {
    fixture();dir(5,0x2700,12);put(at(0x2700),0x2000);put(at(0x2704),12);pe_put16(raw+at(0x2708),0x3600);
    put(at(0x2600),BASE+0x1234);mapped_base=BASE+0x10000;
    struct ntimage d={0};struct ntimage_platform p=platform();
    assert(ntimage_load(&d,raw,sizeof(raw),"reloc.sys",&p));
    assert(pe_u32(d.pe.image+0x2600)==mapped_base+0x1234);assert(ntimage_unload(&d));mapped_base=BASE;
    /* Overflow in the requested mapping is distinct from defined PE32 value wrap. */
    fixture();dir(5,0x2700,12);put(at(0x2700),0x2000);put(at(0x2704),12);pe_put16(raw+at(0x2708),0x5600);
    reject("relocation-type");
}
static bool bad_user_import;
static int user_import(void *ctx,const struct ndis_export *e,uint32_t *address)
{
    (void)ctx;
    *address=bad_user_import?e->address:0x500000+(e->address-0xc2000000);
    return 1;
}
static void user_policy(void)
{
    struct ntimage d={0};struct ntimage_platform p=platform();
    p.user_host=true;p.user_import=user_import;mapped_base=0x100000;
    fixture();import("NTOSKRNL.EXE","KeTickCount",0,false,false);
    dir(5,0x2700,12);put(at(0x2700),0x2000);put(at(0x2704),12);
    pe_put16(raw+at(0x2708),0x3600);put(at(0x2600),BASE+0x1234);
    raw[512]=0xfa; /* admitted diagnostically, never executed by this model */
    assert(ntimage_load(&d,raw,sizeof(raw),"user.sys",&p));
    assert(d.pe.base==0x100000 && pe_u32(d.pe.image+0x2600)==0x101234);
    assert(pe_u32(d.pe.image+0x2120)==0x502000 && !d.scanned);
    assert(ntimage_unload(&d));
    bad_user_import=true;d=(struct ntimage){0};
    assert(!ntimage_load(&d,raw,sizeof(raw),"kernel-import.sys",&p));
    assert(!strcmp(d.error.reason,"user-import-unavailable") && !live);
    bad_user_import=false;mapped_base=BASE;d=(struct ntimage){0};
    assert(!ntimage_load(&d,raw,sizeof(raw),"kernel-va.sys",&p));
    assert(!strcmp(d.error.reason,"image-not-user-pinned") && !live);
}
bool registry_valid(int handle,gen_t generation) { return claims_ok && handle==7 && generation==11; }
int registry_quarantine(int handle,gen_t generation) { assert(handle==7 && generation==11);quarantines++;return 0; }
static bool fence(void *ctx,const char *why) { (void)ctx;assert(why);fences++;return fence_ok; }
static void trace(void *ctx,const char *event,const char *why,uint8_t level) {
    (void)ctx;(void)why;(void)level;if(!strcmp(event,"entry")) entries++;if(!strcmp(event,"exit")) exits++;
}
static struct ndis_callback boundary(struct ntimage *d) {
    return (struct ndis_callback){.image=d,.ops={.fence=fence,.record=trace},
        .claims={7},.generations={11},.claim_count=1,.dirql=5};
}
static int32_t NDIS_STDCALL sum(uint32_t a,uint32_t b) { return (int32_t)(a+b); }
static void callbacks(void) {
    fixture();struct ntimage d={0};struct ntimage_platform p=platform();
    assert(ntimage_load(&d,raw,sizeof(raw),"model.sys",&p));
    struct ndis_callback c=boundary(&d);int32_t result=0;
    assert(ndis_callback_model_call2(&c,BASE+0x1000,sum,3,4,0,10,20,&result));assert(result==7);
    assert(entries==1 && exits==1 && c.irql==0);
    assert(ndis_callback_begin(&c,BASE+0x1000,0,10,20));uint8_t old=99;
    assert(ndis_irql_raise(&c,1,&old)&&old==0);assert(ndis_irql_wait(&c));
    assert(ndis_irql_lower(&c,0));assert(ndis_irql_raise(&c,2,&old)&&old==0);
    assert(ndis_callback_begin(&c,BASE+0x1000,5,11,5));assert(ndis_callback_end(&c)&&c.irql==2);
    assert(ndis_irql_lower(&c,0));assert(ndis_callback_end(&c));assert(c.irql==0);
    /* Nonreturn is delivered independently while ACTIVE, with no end call. */
    assert(ndis_callback_begin(&c,BASE+0x1000,2,20,10));ndis_callback_watchdog(&c,29);assert(!d.quarantined);
    ndis_callback_watchdog(&c,30);assert(d.quarantined && fences==1 && quarantines==1 && c.fence_idle);
    assert(!ndis_callback_end(&c));assert(!ndis_callback_begin(&c,BASE+0x1000,0,40,10));
    assert(!ntimage_unload(&d)&&live==1);ndis_callback_fault(&c,"again");assert(fences==1);
    /* Fixture teardown only: foreign code was never executed or given DMA. */
    release(0,d.pe.image,d.pe.size);d.pe.image=0;
    for(unsigned mode=0;mode<5;mode++) {
        d=(struct ntimage){0};assert(ntimage_load(&d,raw,sizeof(raw),"fault-model.sys",&p));c=boundary(&d);
        assert(ndis_callback_begin(&c,BASE+0x1000,2,0,10));fence_ok=false;
        if(mode==0) ndis_callback_fault(&c,"synthetic-exception");
        if(mode==1) assert(!ndis_irql_wait(&c));
        if(mode==2) assert(!ndis_irql_raise(&c,0,&old));
        if(mode==3) assert(!ndis_irql_lower(&c,0));
        if(mode==4) assert(!ndis_callback_begin(&c,BASE+0x1000,2,1,10));
        assert(d.quarantined && !c.fence_idle && !ntimage_unload(&d));
        release(0,d.pe.image,d.pe.size);fence_ok=true;
    }
}
int main(void) {
    policies();hostile();imports();modified_imports();relocations();user_policy();callbacks();
    assert(!live && allocations==frees && records && quarantines==6);
    puts("PASS shared PE/kernel policy imports ordinals data rollback fault nonreturn IRQL model");fflush(stdout);return 0;
}
