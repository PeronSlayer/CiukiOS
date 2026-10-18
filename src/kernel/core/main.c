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
#include <ciuki/version.h>
#include <ciuki/timing.h>
#include "loader_progress.inc"

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
    if (bi_phys < 0x500 || bi_phys > 0x1000000u - CIUKI_BOOT_INFO_V1_SIZE)
        early_fail("address");
    const struct ciuki_boot_info *src = P2V(bi_phys);
    if (src->size > 0x1000000u - bi_phys) early_fail("address_size");
    const char *why;
    if (bootinfo_validate(src, &why) != 0)
        early_fail(why);
    memset(&g_boot, 0, sizeof(g_boot));
    memcpy(&g_boot, src, src->size); /* v1 never reads the v2 extension */

    bool text = (g_boot.flags & CBI_F_TEXT_MODE) != 0;
    if (text)
        console_init_text();
    serial_boot_init();
    klog("Ciuki VMM F0 build %s - CiukiOS", CIUKI_BUILD_ID);
    /* A separate line: scripts/test/physical.py and its host fixtures match
     * the banner above, so its text stays as it is. */
    klog("CiukiOS %s", CIUKI_VERSION);
    /* Only the current real-mode loader has this optional private trailer.
     * Read it before reclaim; klog retains protected early-boot records even
     * when both firmware and kernel UART probes find no serial port. */
    if (g_boot.loader_start>=0x500 && g_boot.loader_start<=0x100000u-24576 &&
        g_boot.loader_end>=g_boot.loader_start+24576)
        loader_progress_report(P2V(g_boot.loader_start+24544),32);

    cpu_detect();
    gdt_init();
    idt_init();
    pic_init();
    pmm_init();
    vmm_init();
    kheap_init();
    df_tss_init(vmm_kernel_pd());
    /* A normal boot keeps the screen quiet (docs/boot.md); the decision is
     * logged so the serial capture shows which console behaviour applies. */
    console_set_quiet(!text && cbi_quiet_console(g_boot.flags));
    if (!text && !console_init_lfb()) {
        console_init_text();
        klog("[console] LFB unusable, text console");
    }
    if (console_quiet())
        klog("[console] quiet boot: log on serial and disk until the desktop starts");
    klog("[mm] usable RAM %u KiB, free DMA %u pages, free NORMAL %u pages",
         pmm_total_usable() * 4, pmm_zone_free(ZONE_DMA), pmm_zone_free(ZONE_NORMAL));
    registry_init();
    fpu_init();
    sched_init();
    pit_init();
    /* No task is running during the full-accuracy PIT measurement. */
    sti();
    timing_calibrate();
    cli();
    struct task *t = task_create_kernel("probes", probes_main, 0, P_INTERACTIVE);
    if (!t)
        panic("cannot create the probe task");
    task_start(t);
    sched_start();
}
