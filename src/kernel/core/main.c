/* Ciuki VMM F0 entry after paging is on (docs/design/boot-memory.md,
 * "Early kernel sequence").
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/arch.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include <ciuki/probe.h>
#include <ciuki/registry.h>

struct ciuki_boot_info g_boot;

#ifndef CIUKI_BUILD_ID
#define CIUKI_BUILD_ID "unknown"
#endif

static __attribute__((noreturn)) void early_fail(const char *why)
{
    /* Before validation nothing from the handoff is trusted: default COM1
     * and the VGA text buffer. */
    serial_init(0x3F8, 3);
    console_init_text();
    panic("invalid loader handoff: %s", why);
}

void kmain(uint32_t magic, uint32_t bi_phys)
{
    if (magic != CIUKI_BOOT_MAGIC_EAX)
        early_fail("magic");
    if (bi_phys < 0x500 || bi_phys > 0x1000000u - CIUKI_BOOT_INFO_SIZE)
        early_fail("address");
    const struct ciuki_boot_info *src = P2V(bi_phys);
    const char *why;
    if (bootinfo_validate(src, &why) != 0)
        early_fail(why);
    memcpy(&g_boot, src, sizeof(g_boot));

    serial_init((uint16_t)g_boot.uart_base, (uint16_t)g_boot.uart_divisor);
    bool text = (g_boot.flags & CBI_F_TEXT_MODE) != 0;
    if (text)
        console_init_text();
    klog("Ciuki VMM F0 build %s - CiukiOS", CIUKI_BUILD_ID);

    cpu_detect();
    gdt_init();
    idt_init();
    pic_init();
    pmm_init();
    vmm_init();
    kheap_init();
    df_tss_init(vmm_kernel_pd());
    if (!text && !console_init_lfb()) {
        console_init_text();
        klog("[console] LFB unusable, text console");
    }
    klog("[mm] usable RAM %u KiB, free DMA %u pages, free NORMAL %u pages",
         pmm_total_usable() * 4, pmm_zone_free(ZONE_DMA), pmm_zone_free(ZONE_NORMAL));
    registry_init();
    fpu_init();
    sched_init();
    pit_init();
    struct task *t = task_create_kernel("probes", probes_main, 0, P_INTERACTIVE);
    if (!t)
        panic("cannot create the probe task");
    task_start(t);
    sched_start();
}
