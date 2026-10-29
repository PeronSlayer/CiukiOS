/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Single-process H1 services for Wine's DPLAYX_ConstructData: semaphores and
 * pagefile sections, with one whole-section view. No network or IPC service.
 * Unsupported operations remain ENOSYS; this is not the production kernel.
 * host.services owns every mapping, including after its last handle closes.
 */
#ifndef CIUKI_H1_SHARED_H
#define CIUKI_H1_SHARED_H
#include <ciuki/w32_core.h>

static struct h1_shared_object {
    uint32_t type, handle, refs, value, maximum, bytes, viewed;
    uint16_t name[261];
    void *data;
} h1_shared_objects[8];
static uint32_t h1_shared_generation;

static struct h1_shared_object *h1_shared_handle(uint32_t handle) {
    for (unsigned i=0;i<8;i++)
        if (h1_shared_objects[i].refs && h1_shared_objects[i].handle==handle)
            return &h1_shared_objects[i];
    return 0;
}
static void h1_shared_collect(struct h1_shared_object *o) {
    if (o->refs || o->viewed) return;
    if (o->data) host.services.unmap(&host,o->data,o->bytes);
    w32_fill(o,0,sizeof(*o));
}
static struct h1_shared_object *h1_shared_create(uint32_t type,uint32_t name,
                                                uint32_t *error) {
    const uint16_t *text=(const uint16_t *)(uintptr_t)name;
    unsigned length=0;
    if (text) {
        while (length<261 && text[length]) length++;
        if (length==261) { *error=87; return 0; }
        for (unsigned i=0;i<8;i++) {
            struct h1_shared_object *o=&h1_shared_objects[i];
            if (!o->type) continue;
            unsigned n=0;
            while (n<length && o->name[n]==text[n]) n++;
            if (n!=length || o->name[n]) continue;
            if (o->type!=type) { *error=6; return 0; }
            o->refs++; *error=183; return o;
        }
    }
    for (unsigned i=0;i<8;i++) {
        struct h1_shared_object *o=&h1_shared_objects[i];
        if (o->type) continue;
        o->type=type; o->refs=1;
        o->handle=0x51000000u+(++h1_shared_generation)*4u;
        if (text) w32_copy(o->name,text,(length+1)*2);
        return o;
    }
    *error=8; return 0;
}
static int32_t h1_shared_call(void *record,uint32_t size) {
    struct ciuki_w32_header *h=record;
    if (h->operation==W32_OBJECT && size==sizeof(struct ciuki_w32_object_args)) {
        struct ciuki_w32_object_args *a=record;
        a->error=0; a->result=0;
        if (a->action==W32_OBJ_CREATE && a->type==W32_OBJ_SEMAPHORE) {
            if (!a->maximum || a->maximum>0x7fffffffu || a->value>a->maximum) {
                a->error=87; return 0;
            }
            struct h1_shared_object *o=h1_shared_create(a->type,a->name,&a->error);
            if (!o) return 0;
            if (!a->error) { o->value=a->value; o->maximum=a->maximum; }
            a->result=o->handle; return 0;
        }
        struct h1_shared_object *o=h1_shared_handle(a->handle);
        if (!o) return -38;
        if (a->action==W32_OBJ_CLOSE) {
            o->refs--; h1_shared_collect(o); a->result=1; return 0;
        }
        if (a->action==W32_OBJ_RELEASE && o->type==W32_OBJ_SEMAPHORE) {
            if (!a->value || a->value>o->maximum-o->value) a->error=298;
            else { a->result=o->value; o->value+=a->value; }
            return 0;
        }
    } else if (h->operation==W32_WAIT && size==sizeof(struct ciuki_w32_wait_args)) {
        struct ciuki_w32_wait_args *a=record;
        if (a->count!=1 || !a->handles || a->alertable || a->wake_mask || a->signal)
            return -38;
        struct h1_shared_object *o=h1_shared_handle(*(const uint32_t *)(uintptr_t)a->handles);
        if (!o || o->type!=W32_OBJ_SEMAPHORE) return -38;
        a->error=0;
        if (o->value) { o->value--; a->result=W32_WAIT_OBJECT_0; return 0; }
        /* No scheduler: never pretend to complete a blocking wait. */
        if (a->timeout) return -38;
        a->result=W32_WAIT_TIMEOUT; return 0;
    } else if (h->operation==W32_SECTION && size==sizeof(struct ciuki_w32_section_args)) {
        struct ciuki_w32_section_args *a=record;
        if (a->kind!=W32_SEC_PAGEFILE || a->protect!=(4u|0x08000000u)) return -38;
        a->error=0; a->result=0;
        if (!a->bytes || a->bytes>1024u*1024u) { a->error=87; return 0; }
        struct h1_shared_object *o=h1_shared_create(W32_OBJ_SECTION,a->name,&a->error);
        if (o) { if (!a->error) o->bytes=a->bytes; a->result=o->handle; }
        return 0;
    } else if (h->operation==W32_VIEW && size==sizeof(struct ciuki_w32_view_args)) {
        struct ciuki_w32_view_args *a=record;
        a->error=0; a->result=0;
        if (a->action==W32_VIEW_MAP) {
            struct h1_shared_object *o=h1_shared_handle(a->section);
            if (!o || o->type!=W32_OBJ_SECTION) { a->error=6; return 0; }
            if (o->viewed || a->offset_lo || a->offset_hi ||
                (a->bytes && a->bytes!=o->bytes) || a->access!=W32_FILE_MAP_WRITE)
                return -38;
            if (o->data && a->address && a->address!=(uint32_t)(uintptr_t)o->data) {
                a->error=487; return 0;
            }
            if (!o->data) o->data=host.services.map(&host,a->address,o->bytes,!!a->address);
            if (!o->data) { a->error=a->address ? 487 : 8; return 0; }
            o->viewed=1; a->result=(uint32_t)(uintptr_t)o->data; return 0;
        }
        for (unsigned i=0;i<8;i++) {
            struct h1_shared_object *o=&h1_shared_objects[i];
            if (!o->viewed || a->address!=(uint32_t)(uintptr_t)o->data) continue;
            if (a->action==W32_VIEW_FLUSH) { a->result=1; return 0; }
            if (a->action==W32_VIEW_UNMAP) {
                o->viewed=0; h1_shared_collect(o); a->result=1; return 0;
            }
        }
    }
    return -38;
}
static unsigned h1_shared_live(void) {
    unsigned live=0;
    for (unsigned i=0;i<8;i++) live+=!!h1_shared_objects[i].type;
    return live;
}
#endif
