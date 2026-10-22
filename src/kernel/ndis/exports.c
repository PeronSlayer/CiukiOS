/* SPDX-License-Identifier: GPL-2.0-only
 * Native shim ABI reservations for ND6/ND7 and f6-06. No stubs or guessed
 * NT5 ordinals. Addresses remain zero until the corresponding real shim lands.
 * Stack bytes describe stack arguments (fastcall register arguments excluded).
 * Sources and ordinal admission rule: docs/design/ndis-loader-boundary.md. */
#include "ntimage.h"
#define S(m,n,b) {m,n,NDIS_NO_ORDINAL,b,NDIS_ABI_STDCALL,NDIS_FUNCTION,0,false}
#define F(m,n,b) {m,n,NDIS_NO_ORDINAL,b,NDIS_ABI_FASTCALL,NDIS_FUNCTION,0,false}
#define C(m,n) {m,n,NDIS_NO_ORDINAL,0,NDIS_ABI_CDECL,NDIS_FUNCTION,0,false}
static const struct ndis_export exports[] = {
    S("NDIS.SYS","NdisInitializeWrapper",16),
    S("NDIS.SYS","NdisTerminateWrapper",8),
    S("NDIS.SYS","NdisMRegisterMiniport",12),
    S("NDIS.SYS","NdisMSetAttributesEx",20),
    S("NDIS.SYS","NdisGetRoutineAddress",4),
    S("NDIS.SYS","NdisMRegisterInterrupt",28),
    S("NDIS.SYS","NdisMDeregisterInterrupt",4),
    S("NDIS.SYS","NdisMSynchronizeWithInterrupt",12),
    S("NDIS.SYS","NdisMAllocateSharedMemory",20),
    S("NDIS.SYS","NdisMFreeSharedMemory",24),
    S("NDIS.SYS","NdisMAllocateMapRegisters",20),
    S("NDIS.SYS","NdisMFreeMapRegisters",4),
    S("NDIS.SYS","NdisReadConfiguration",20),
    S("NDIS.SYS","NdisOpenConfiguration",12),
    S("NDIS.SYS","NdisCloseConfiguration",4),
    S("NDIS.SYS","NdisMIndicateReceivePacket",12),
    S("NTOSKRNL.EXE","MmGetSystemRoutineAddress",4),
    S("NTOSKRNL.EXE","ExAllocatePoolWithTag",12),
    S("NTOSKRNL.EXE","ExFreePool",4),
    S("NTOSKRNL.EXE","KeInitializeDpc",12),
    S("NTOSKRNL.EXE","KeInsertQueueDpc",12),
    S("NTOSKRNL.EXE","KeRemoveQueueDpc",4),
    S("NTOSKRNL.EXE","KeSetTimer",16),
    S("NTOSKRNL.EXE","KeCancelTimer",4),
    S("NTOSKRNL.EXE","KeInitializeEvent",12),
    S("NTOSKRNL.EXE","KeSetEvent",12),
    S("NTOSKRNL.EXE","KeResetEvent",4),
    S("NTOSKRNL.EXE","KeWaitForSingleObject",20),
    S("NTOSKRNL.EXE","KeBugCheckEx",20),
    F("NTOSKRNL.EXE","KefAcquireSpinLockAtDpcLevel",0),
    F("NTOSKRNL.EXE","KefReleaseSpinLockFromDpcLevel",0),
    {"NTOSKRNL.EXE","KeTickCount",NDIS_NO_ORDINAL,0,NDIS_ABI_DATA,NDIS_DATA,0,false},
    C("NTOSKRNL.EXE","memcpy"), C("NTOSKRNL.EXE","memset"),
    F("HAL.DLL","KfRaiseIrql",0), F("HAL.DLL","KfLowerIrql",0),
    F("HAL.DLL","KfAcquireSpinLock",0), F("HAL.DLL","KfReleaseSpinLock",0),
    S("HAL.DLL","KeGetCurrentIrql",0),
    S("HAL.DLL","KeStallExecutionProcessor",4)
};
#undef S
#undef F
#undef C
static bool equal(const char *a,const char *b,bool fold) {
    if(!a || !b) return false;
    for(;;a++,b++) {
        unsigned x=(unsigned char)*a,y=(unsigned char)*b;
        if(fold) { if(x>='a'&&x<='z') x-=32; if(y>='a'&&y<='z') y-=32; }
        if(x!=y) return false;
        if(!x) return true;
    }
}
const struct ndis_export *ndis_exports(size_t *n) { *n=sizeof(exports)/sizeof(exports[0]); return exports; }
const struct ndis_export *ndis_export_find(const struct ndis_export *table,size_t n,
                                         const char *module,const char *name,uint16_t ordinal) {
    if(!table) return 0;
    for(size_t i=0;i<n;i++) if(equal(module,table[i].module,true) &&
        (name?equal(name,table[i].name,false):(ordinal && ordinal==table[i].ordinal))) return &table[i];
    return 0;
}
uint32_t ndis_export_address(const struct ndis_export *e,const char **reason) {
    *reason=!e?"unknown-export":!e->implemented||!e->address?"unimplemented-export":0;
    return *reason?0:e->address;
}
uint32_t ndis_routine_address(const struct ndis_export *table,size_t n,const uint16_t *name,
                             uint16_t bytes,bool ndis_module) {
    char ascii[256];
    if(!name || !bytes || (bytes&1) || bytes/2>=sizeof(ascii)) return 0;
    for(unsigned i=0;i<bytes/2;i++) { if(!name[i] || name[i]>127) return 0; ascii[i]=(char)name[i]; }
    ascii[bytes/2]=0;
    const char *reason;
    const struct ndis_export *e=ndis_export_find(table,n,ndis_module?"NDIS.SYS":"NTOSKRNL.EXE",ascii,0);
    if(!e && !ndis_module) e=ndis_export_find(table,n,"HAL.DLL",ascii,0);
    if(!e || e->kind!=NDIS_FUNCTION) return 0;
    return ndis_export_address(e,&reason);
}
uint32_t ndis_mm_get_system_routine_address(const uint16_t *name,uint16_t bytes) {
    size_t n; const struct ndis_export *table=ndis_exports(&n);
    return ndis_routine_address(table,n,name,bytes,false);
}
uint32_t ndis_get_routine_address(const uint16_t *name,uint16_t bytes) {
    size_t n; const struct ndis_export *table=ndis_exports(&n);
    return ndis_routine_address(table,n,name,bytes,true);
}
