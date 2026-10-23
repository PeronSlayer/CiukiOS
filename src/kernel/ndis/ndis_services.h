/* Process-local NDIS veneers and bounded adapter services. GPL-2.0-only */
#ifndef CIUKI_NDIS_SERVICES_H
#define CIUKI_NDIS_SERVICES_H
#include "miniport.h"
#include "user_memory.h"
enum ndis_service_operation {
#define ND(name,args,bytes,implemented) NDIS_OP_##name,
#include "ndis_exports.def"
#undef ND
    NDIS_SERVICE_COUNT
};
struct ndis_service_ops {
    void *context;
    unsigned (*pci)(void *,bool write,unsigned offset,void *,unsigned bytes);
    uint32_t (*port)(void *,unsigned base,unsigned bytes,bool release);
    uint32_t (*mmio)(void *,uint32_t physical,unsigned bytes,bool release);
    unsigned (*resources)(void *,void *out,unsigned capacity);
    void (*log)(void *,uint32_t code,const uint32_t *,unsigned count);
};
struct ndis_services {
    struct ndis_miniport *miniport;
    struct ndis_service_ops ops;
    uint32_t veneer_base;
    struct { uint32_t handle,maximum,protocol,used;bool live,packet; } pool[16];
    struct { uint32_t address,pool;bool live,packet; } object[64];
    struct { struct mm_pin pin;uint32_t address,bytes,physical;bool live,cached; } shared[16];
    struct { uint16_t name[64];uint8_t value[256];uint32_t type;uint16_t name_bytes,value_bytes; } parameter[32];
    unsigned parameter_count;
    uint32_t configuration[16];
    struct { uint32_t address,handle; } config_value[32];
    uint32_t resource_list,instance_string;
    bool wrapper_open;
};
int ndis_veneers(void *,uint32_t bytes,uint32_t base,struct ndis_user_export *,unsigned capacity,unsigned *count);
int ndis_services_init(struct ndis_services *,struct ndis_miniport *,const struct ndis_service_ops *,uint32_t veneer_base);
uint32_t ndis_service(struct ndis_services *,uint32_t operation,uint32_t arguments);
#endif
