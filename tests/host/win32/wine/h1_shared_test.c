/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/w32_core.h>

/* Low, fixed native fixture addresses fit the copied-record wire pointers.
 * No PE code or network endpoint is executed in this sanitizer test. */
static unsigned char pages[640*1024];
static unsigned mappings, fail_map;
static void *map(void *opaque,uint32_t address,uint32_t bytes,int exact) {
    (void)opaque; (void)address; (void)exact;
    assert(!mappings && bytes<=sizeof(pages));
    if (fail_map) return 0;
    memset(pages,0,bytes); mappings++; return pages;
}
static void unmap(void *opaque,void *data,uint32_t bytes) {
    (void)opaque;
    assert(mappings==1 && data==pages && bytes<=sizeof(pages)); mappings--;
}
static struct {
    struct {
        void *(*map)(void *,uint32_t,uint32_t,int);
        void (*unmap)(void *,void *,uint32_t);
    } services;
} host={{map,unmap}};
#define w32_fill memset
#define w32_copy memcpy
#include "h1_shared.h"
#define CALL(r) h1_shared_call(&(r),sizeof(r))
static uint16_t name[]={'d','p','l','a','y',0};
static uint16_t section_name[]={'d','p','l','a','y','m',0};
static uint32_t wait_handle;
int main(void) {
    assert((uintptr_t)pages<=UINT32_MAX && (uintptr_t)&wait_handle<=UINT32_MAX);
    struct ciuki_w32_object_args sem={.header={1,sizeof(sem),W32_OBJECT,0},
        .action=W32_OBJ_CREATE,.type=W32_OBJ_SEMAPHORE,.maximum=1,.name=(uint32_t)(uintptr_t)name};
    assert(!CALL(sem) && !sem.error && sem.result);
    wait_handle=sem.result;
    struct ciuki_w32_wait_args wait={.header={1,sizeof(wait),W32_WAIT,0},
        .handles=(uint32_t)(uintptr_t)&wait_handle,.count=1};
    assert(!CALL(wait) && wait.result==W32_WAIT_TIMEOUT);
    wait.timeout=UINT32_MAX; assert(CALL(wait)==-38);
    sem.action=W32_OBJ_RELEASE; sem.handle=wait_handle; sem.value=1;
    assert(!CALL(sem) && !sem.error && !sem.result);
    assert(!CALL(sem) && sem.error==298); /* overflow never changes count */
    assert(!CALL(wait) && !wait.error && wait.result==W32_WAIT_OBJECT_0);
    sem.action=W32_OBJ_CLOSE; assert(!CALL(sem));
    assert(!h1_shared_live() && CALL(wait)==-38);
    sem.action=W32_OBJ_CREATE; sem.value=0;
    assert(!CALL(sem) && !sem.error && sem.result!=wait_handle);
    sem.handle=sem.result; sem.action=W32_OBJ_CLOSE; assert(!CALL(sem));

    struct ciuki_w32_section_args section={.header={1,sizeof(section),W32_SECTION,0},
        .kind=W32_SEC_PAGEFILE,.bytes=sizeof(pages),.protect=4u|0x08000000u,
        .name=(uint32_t)(uintptr_t)section_name};
    assert(!CALL(section) && !section.error && section.result);
    uint32_t section_handle=section.result;
    assert(!CALL(section) && section.error==183 && section.result==section_handle);
    struct ciuki_w32_view_args view={.header={1,sizeof(view),W32_VIEW,0},
        .action=W32_VIEW_MAP,.section=section_handle,.access=W32_FILE_MAP_WRITE};
    fail_map=1; view.address=0x50000000;
    assert(!CALL(view) && view.error==487 && !mappings);
    view.address=0; assert(!CALL(view) && view.error==8 && !mappings);
    fail_map=0; assert(!CALL(view) && !view.error && view.result==(uint32_t)(uintptr_t)pages);
    assert(mappings==1 && !pages[0] && !pages[sizeof(pages)-1]);
    pages[0]=7; view.address=view.result; view.action=W32_VIEW_FLUSH;
    assert(!CALL(view) && !view.error && pages[0]==7);
    sem.handle=section_handle; assert(!CALL(sem) && h1_shared_live()==1);
    assert(!CALL(sem) && h1_shared_live()==1); /* view retains storage after last close */
    assert(!CALL(view) && !view.error && pages[0]==7);
    view.action=W32_VIEW_UNMAP;
    assert(!CALL(view) && !view.error && !mappings && !h1_shared_live());
    assert(CALL(view)==-38 && CALL(sem)==-38); /* stale resources refused */
    section.kind=W32_SEC_FILE; assert(CALL(section)==-38);
    struct ciuki_w32_header unknown={1,sizeof(unknown),0xffffffffu,0};
    assert(CALL(unknown)==-38);
    uint32_t handles[8];
    sem.action=W32_OBJ_CREATE; sem.name=0;
    for (unsigned i=0;i<8;i++) {
        assert(!CALL(sem) && !sem.error); handles[i]=sem.result;
    }
    assert(!CALL(sem) && sem.error==8 && !sem.result && h1_shared_live()==8);
    sem.action=W32_OBJ_CLOSE;
    for (unsigned i=0;i<8;i++) { sem.handle=handles[i]; assert(!CALL(sem)); }
    assert(!h1_shared_live());
    puts("PASS H1 semaphore/shared data: waits, overflow, generations, named reuse, map failure, view lifetime, teardown and unsupported services");
    fflush(stdout);
    return 0;
}
