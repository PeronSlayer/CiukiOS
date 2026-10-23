/* SPDX-License-Identifier: GPL-2.0-only
 * Native shim ABI reservations for ND6/ND7 and f6-06. No success stubs or guessed
 * NT5 ordinals. Kernel addresses remain zero; implemented entries require pinned user veneers.
 * Stack bytes describe stack arguments (fastcall register arguments excluded).
 * Sources and ordinal admission rule: docs/design/ndis-loader-boundary.md. */
#include "ntimage.h"
#define S(m,n,b) {m,n,NDIS_NO_ORDINAL,b,NDIS_ABI_STDCALL,NDIS_FUNCTION,0,false}
#define F(m,n,b) {m,n,NDIS_NO_ORDINAL,b,NDIS_ABI_FASTCALL,NDIS_FUNCTION,0,false}
#define U(m,n,b,a) {m,n,NDIS_NO_ORDINAL,b,a,NDIS_FUNCTION,0,true}
#define C(m,n) {m,n,NDIS_NO_ORDINAL,0,NDIS_ABI_CDECL,NDIS_FUNCTION,0,false}
static const struct ndis_export exports[] = {
#define ND(name,args,bytes,implemented) {"NDIS.SYS",#name,NDIS_NO_ORDINAL,bytes,bytes?NDIS_ABI_STDCALL:NDIS_ABI_CDECL,NDIS_FUNCTION,0,implemented},
#include "ndis_exports.def"
#undef ND
    S("NTOSKRNL.EXE","MmGetSystemRoutineAddress",4),
    S("NTOSKRNL.EXE","ExAllocatePoolWithTag",12),
    S("NTOSKRNL.EXE","ExFreePool",4),
    S("NTOSKRNL.EXE","KeInitializeDpc",12),
    S("NTOSKRNL.EXE","KeInsertQueueDpc",12),
    S("NTOSKRNL.EXE","KeRemoveQueueDpc",4),
    S("NTOSKRNL.EXE","KeSetTimer",16),
    S("NTOSKRNL.EXE","KeCancelTimer",4),
    U("NTOSKRNL.EXE","KeInitializeEvent",12,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","KeSetEvent",12,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","KeResetEvent",4,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","KeWaitForSingleObject",20,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","KeWaitForMultipleObjects",32,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","KeInitializeSemaphore",12,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","KeReleaseSemaphore",16,NDIS_ABI_STDCALL),

    {"NTOSKRNL.EXE","KeTickCount",NDIS_NO_ORDINAL,0,NDIS_ABI_DATA,NDIS_DATA,0,true},
    C("NTOSKRNL.EXE","memcpy"), C("NTOSKRNL.EXE","memset"),
    F("HAL.DLL","KfRaiseIrql",0), F("HAL.DLL","KfLowerIrql",0),

    U("NTOSKRNL.EXE","InterlockedPopEntrySList",0,NDIS_ABI_FASTCALL),
    U("NTOSKRNL.EXE","InterlockedPushEntrySList",0,NDIS_ABI_FASTCALL),
    U("NTOSKRNL.EXE","IoFreeMdl",4,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","IofCompleteRequest",0,NDIS_ABI_FASTCALL),
    U("NTOSKRNL.EXE","KeBugCheckEx",20,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","KeInitializeSpinLock",4,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","KeQueryTimeIncrement",0,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","KefAcquireSpinLockAtDpcLevel",0,NDIS_ABI_FASTCALL),
    U("NTOSKRNL.EXE","KefReleaseSpinLockFromDpcLevel",0,NDIS_ABI_FASTCALL),
    U("NTOSKRNL.EXE","MmMapLockedPagesSpecifyCache",24,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","RtlAppendUnicodeStringToString",8,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","RtlCopyUnicodeString",8,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","RtlInitUnicodeString",8,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","WRITE_REGISTER_UCHAR",8,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","WRITE_REGISTER_ULONG",8,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","WRITE_REGISTER_USHORT",8,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","ZwClose",4,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","ZwOpenFile",24,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","_alldiv",16,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","_allmul",16,NDIS_ABI_STDCALL),
    U("NTOSKRNL.EXE","strncpy",0,NDIS_ABI_CDECL),
    U("HAL.DLL","KeGetCurrentIrql",0,NDIS_ABI_STDCALL),
    U("HAL.DLL","KeStallExecutionProcessor",4,NDIS_ABI_STDCALL),
    U("HAL.DLL","KfAcquireSpinLock",0,NDIS_ABI_FASTCALL),
    U("HAL.DLL","KfReleaseSpinLock",0,NDIS_ABI_FASTCALL),
    U("HAL.DLL","READ_PORT_UCHAR",4,NDIS_ABI_STDCALL),
    U("HAL.DLL","READ_PORT_ULONG",4,NDIS_ABI_STDCALL),
    U("HAL.DLL","READ_PORT_USHORT",4,NDIS_ABI_STDCALL),
    U("HAL.DLL","WRITE_PORT_UCHAR",8,NDIS_ABI_STDCALL),
    U("HAL.DLL","WRITE_PORT_ULONG",8,NDIS_ABI_STDCALL),
    U("HAL.DLL","WRITE_PORT_USHORT",8,NDIS_ABI_STDCALL),
};
#undef S
#undef F
#undef C
#undef U
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
