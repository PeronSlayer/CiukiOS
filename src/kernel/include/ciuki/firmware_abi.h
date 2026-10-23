/* f1-109: additive call-76 firmware inventory. SPDX-License-Identifier: MIT */
#ifndef CIUKI_FIRMWARE_ABI_H
#define CIUKI_FIRMWARE_ABI_H
#include <stdint.h>
#define CIUKI_FIRMWARE_VERSION 2u
#define CIUKI_FIRMWARE_PAGE_RECORDS 4u
#define CIUKI_FIRMWARE_TRUNCATED 1u
#define CIUKI_FIRMWARE_INCOMPLETE 2u
#define CIUKI_FIRMWARE_PENDING 4u
#define CIUKI_FIRMWARE_ACPI 1u
#define CIUKI_FIRMWARE_PNP_BIOS 2u
#define CIUKI_FIRMWARE_DISCOVERED 1u
#define CIUKI_FIRMWARE_UNSUPPORTED 2u
#define CIUKI_FIRMWARE_ROUTING_UNQUALIFIED 3u
#define CIUKI_FW_CAP_BUTTON 1u
#define CIUKI_FW_CAP_LID 2u
#define CIUKI_FW_CAP_AC 4u
#define CIUKI_FW_CAP_BATTERY 8u
#define CIUKI_FW_CAP_THERMAL 16u
#define CIUKI_FW_CAP_POWER 32u
#define CIUKI_FW_CAP_WAKE 64u
#define CIUKI_FW_CAP_EC 128u
#pragma pack(push, 4)
struct ciuki_firmware_resource {
    uint32_t type, flags; /* v1 types; IRQ flags: edge=1, exclusive=2, active-high=4 */
    uint64_t start, end;
};
struct ciuki_firmware_record {
    uint32_t id, parent, origin, state, capabilities, flags;
    uint32_t logical_owner, vm_lease, cid_count, resource_count;
    char path[96], hid[32], cid[4][32], reason[80];
    struct ciuki_firmware_resource resources[8];
};
struct ciuki_firmware_page {
    uint32_t version, size, cursor, generation, reserved;
    uint32_t count, total, next, flags, status;
    uint32_t allocated_bytes, peak_bytes, table_count, sci_enabled; /* 0 unknown, 1 off, 2 on */
    struct ciuki_firmware_record records[4];
};
#pragma pack(pop)
_Static_assert(sizeof(struct ciuki_firmware_resource) == 24, "firmware resource ABI");
_Static_assert(sizeof(struct ciuki_firmware_record) == 568, "firmware record ABI");
_Static_assert(sizeof(struct ciuki_firmware_page) == 2328, "firmware page ABI");
#endif
