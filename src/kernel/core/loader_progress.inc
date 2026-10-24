#include <ciuki/system_settings.h>
/* Private CLDRPRG1 trailer, copied before pmm_init can reclaim the loader.
 * Does not change the frozen boot_info or the native ABI. No disk I/O here.
 * SPDX-License-Identifier: GPL-2.0-only */
static void loader_progress_report(const void *bytes, size_t size)
{
    if (size!=32 || memcmp(bytes,"CLDRPRG1",8)) return;
    uint32_t masks[4];
    memcpy(masks,(const uint8_t *)bytes+8,sizeof(masks));
    if ((masks[0]|masks[1]|masks[2]|masks[3])&~15u ||
        ((masks[1]|masks[2]|masks[3])&~masks[0])) return;
    const uint8_t *profile = (const uint8_t *)bytes + 24;
    uint16_t config_size = (uint16_t)(profile[6] | (uint16_t)profile[7] << 8);
    if (system_boot_hw_copy && !memcmp(profile, "HP01", 4) && profile[5] >= 1 && profile[5] <= 8 && profile[4] < profile[5] &&
        config_size && config_size < 24576 && g_boot.loader_end >= g_boot.loader_start + 0x18000 + config_size)
        system_boot_hw_copy((const char *)bytes - 24544 + 0x18000, config_size, profile[4]);
    static const char *const names[]={"menu","video","elf","handoff"};
    for (unsigned i=0;i<ARRAY_SIZE(names);i++) if (masks[0]&(1u<<i))
        klog("[loader] stage=%s completed=%u stalled=%u failed=%u",
             names[i],!!(masks[1]&(1u<<i)),!!(masks[2]&(1u<<i)),!!(masks[3]&(1u<<i)));
}
