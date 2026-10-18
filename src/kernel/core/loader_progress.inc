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
    static const char *const names[]={"menu","video","elf","handoff"};
    for (unsigned i=0;i<ARRAY_SIZE(names);i++) if (masks[0]&(1u<<i))
        klog("[loader] stage=%s completed=%u stalled=%u failed=%u",
             names[i],!!(masks[1]&(1u<<i)),!!(masks[2]&(1u<<i)),!!(masks[3]&(1u<<i)));
}
