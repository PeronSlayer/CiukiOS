#ifndef CIUKIOS_SESSION_SCHEDULER_H
#define CIUKIOS_SESSION_SCHEDULER_H

#include <stdint.h>

#define CVSCHED_MAGIC 0x43535643UL
#define CVSCHED_VERSION 0x0100u
#define CVSCHED_BYTES 256u
#define CVSCHED_STATE_BOUND       0x00000001UL
#define CVSCHED_STATE_SESSION     0x00000002UL
#define CVSCHED_STATE_JEMM        0x00000004UL
#define CVSCHED_STATE_DPMI_HOST    0x00000008UL
#define CVSCHED_STATE_DPMI_CLIENT  0x00000010UL
#define CVSCHED_STATE_CALLBACK     0x00000020UL
#define CVSCHED_STATE_FATAL        0x80000000UL
#define CVSCHED_FLAG_V86_IRQ       0x00000001UL
#define CVSCHED_FLAG_DPMI_IRQ      0x00000002UL
#define CVSCHED_FLAG_HDP324_ABI    0x00000004UL
#define CVSCHED_FLAG_PTE_GUARD     0x00000008UL

#pragma pack(push, 1)
typedef struct cvsched_descriptor {
    uint32_t magic;
    uint16_t version, bytes;
    uint32_t generation, state, flags, last_error;
    uint32_t v86_ticks, dpmi_ticks, services, reentries;
    uint32_t dpmi_handle, installs, removes;
    uint32_t client_entries, client_exits, fault_exits;
    uint32_t pte_checks, pte_repairs;
    uint32_t shadow_bytes, scheduler_bytes, adapter_bytes, callback_bytes;
    uint32_t mode_transitions, failures;
    uint32_t ptes[32];
    uint32_t request_generation, service_generation;
    uint32_t last_cr3, last_eip, last_flags, last_port;
    uint32_t reserved[2];
} cvsched_descriptor;
#pragma pack(pop)

typedef char cvsched_descriptor_size_must_match[
    sizeof(cvsched_descriptor) == CVSCHED_BYTES ? 1 : -1];

#endif
