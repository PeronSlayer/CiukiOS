/* Independent literal oracle, also compiled for i686. SPDX-License-Identifier: MIT */
#include <ciuki/abi.h>
#include <ciuki/firmware_abi.h>
_Static_assert(CIUKI_SYS_DEVICES == 76, "v1 syscall preserved");
_Static_assert(CIUKI_DEVICES_VERSION == 2, "v2 version (display driver model)");
_Static_assert(sizeof(struct ciuki_devices_info) == 38536, "v2 bytes");
_Static_assert(CIUKI_FIRMWARE_VERSION == 2, "separate version");
_Static_assert(sizeof(struct ciuki_firmware_page) == 2328, "page size");
_Static_assert(_Alignof(struct ciuki_firmware_page) == 4, "page alignment");
_Static_assert(__builtin_offsetof(struct ciuki_firmware_page, records) == 56, "page records");
_Static_assert(__builtin_offsetof(struct ciuki_firmware_record, path) == 40, "path");
_Static_assert(__builtin_offsetof(struct ciuki_firmware_record, hid) == 136, "hid");
_Static_assert(__builtin_offsetof(struct ciuki_firmware_record, cid) == 168, "cid");
_Static_assert(__builtin_offsetof(struct ciuki_firmware_record, reason) == 296, "reason");
_Static_assert(__builtin_offsetof(struct ciuki_firmware_record, resources) == 376, "resources");
_Static_assert(sizeof(struct ciuki_firmware_record) == 568, "record");
int main(void) { return 0; }
