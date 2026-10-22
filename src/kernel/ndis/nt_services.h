/* Process-local NT/HAL veneer and validated scalar service. GPL-2.0-only */
#ifndef CIUKI_NDIS_NT_SERVICES_H
#define CIUKI_NDIS_NT_SERVICES_H
#include "nt_runtime.h"
struct ndis_user_export;
#define NDIS_NT_EXPORTS 32u
struct ndis_nt_file { uint16_t name[256]; uint16_t bytes; bool present; };
struct ndis_nt_irp {
    uint32_t address,status_address,generation,maximum_information;
    void *context;
    void (*complete)(void *,int32_t,uint32_t);
    bool live;
};
struct ndis_nt_services {
    struct ndis_nt_runtime *runtime;
    void *context;
    uint32_t (*port)(void *,uint32_t port,unsigned width,bool write,uint32_t value,bool *ok);
    bool (*mmio)(void *,uint32_t address,unsigned width,uint32_t value);
    int (*stall)(void *,uint32_t us);
    struct ndis_nt_file file[NDIS_NT_OBJECTS]; /* installer-owned immutable names */
    struct { uint32_t value,share;unsigned file;bool live; } handle[NDIS_NT_OBJECTS];
    struct ndis_nt_irp irp[NDIS_NT_OBJECTS];
    uint32_t next_handle;
};
/* Emit exact x86 calling-convention veneers, never kernel function addresses.
 * Page is sealed by the allocator after emission. */
int ndis_nt_veneers(void *page,uint32_t bytes,uint32_t base,uint32_t tick_address,
                     struct ndis_user_export *,unsigned capacity,unsigned *count);
uint64_t ndis_nt_service(struct ndis_nt_services *,uint32_t operation,uint32_t arguments);
/* Upper-edge producer only: already resident IRP/IoStatus view with no
 * lower-device stack or completion-routine chain. Arbitrary driver IRPs refuse. */
int ndis_nt_irp_register(struct ndis_nt_services *,uint32_t address,uint32_t status_address,
                         uint32_t maximum_information,void *,void (*complete)(void *,int32_t,uint32_t));
void ndis_nt_services_stop(struct ndis_nt_services *);
#endif
