/* Installed SDK surface: compile only. SPDX-License-Identifier: MIT */
#include <ciuki/devices.h>
int firmware_client(struct ciuki_firmware_page *p)
{
    *p = (struct ciuki_firmware_page){.version = CIUKI_FIRMWARE_VERSION, .size = sizeof(*p)};
    return ciuki_firmware_inventory(p);
}
