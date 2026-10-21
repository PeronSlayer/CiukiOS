/* Shared authored guest result lifetime. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_W32_CORE_FIXTURE_H
#define CIUKI_W32_CORE_FIXTURE_H
#include <w32/base.h>
#include "controller.h"
extern void *W32API VirtualAlloc(void *,uint32_t,uint32_t,uint32_t);
extern void W32API Sleep(uint32_t);
extern void W32API ExitProcess(uint32_t);
static volatile struct w32_core_result *result;
static inline void fixture_begin(unsigned id)
{
    result=VirtualAlloc((void *)(uintptr_t)w32_fixtures[id].address,4096,0x3000,4);
    if ((uintptr_t)result!=w32_fixtures[id].address) ExitProcess(1);
    result->magic=W32_CORE_MAGIC;result->version=1;result->fixture=id;
}
static inline void fixture_check(unsigned row,int ok,unsigned line)
{
    __atomic_store_n(&result->row[row].checked,1,__ATOMIC_RELAXED);
    if(!ok){
        __atomic_add_fetch(&result->row[row].errors,1,__ATOMIC_RELAXED);
        uint32_t empty=0;
        __atomic_compare_exchange_n(&result->first_error,&empty,line,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED);
    }
}
#define CHECK(row,expr) fixture_check(row,!!(expr),__LINE__)
static inline void fixture_unsupported(unsigned row)
{
    result->unsupported|=1u<<row;result->row[row].errors++;
}
static inline void fixture_finish(void)
{
    result->done=1;
    for(unsigned i=0;i<30000 && !result->release;i++) Sleep(1);
    uint32_t errors=result->unsupported || !result->release;
    for(unsigned i=0;i<w32_core_schemas[result->fixture].rows;i++)
        errors|=result->row[i].errors || result->row[i].checked!=1;
    ExitProcess(errors ? 1 : 0);
}
#endif
