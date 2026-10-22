/* f9-01: Setup-mode flag and the Setup memory refusal (cd-setup-plan.md
 * sections 3.2 and 3.3; directive f9-01 items 5 and 7). Normal boots only
 * parse the options once; nothing else here runs without setup=1.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/setup_mode.h>
#include <ciuki/storage.h>
#include <ciuki/ata.h>
#include <ciuki/task.h>

#ifndef CIUKI_BUILD_ID
#define CIUKI_BUILD_ID "unknown"
#endif
#ifndef CIUKI_BUILD_EPOCH
#define CIUKI_BUILD_EPOCH 0
#endif
#define SETUP_TEXT(x) #x
#define SETUP_STRING(x) SETUP_TEXT(x)

/* Read back from \SYSTEM\VMM.ELF on the boot image at the file offset of
 * this object (cdview.c, cdview_identify): equal bytes at the same virtual
 * address identify the same build. */
__attribute__((used, aligned(16)))
const char setup_build_identity[] = "CIUKI-BUILD-IDENTITY " CIUKI_BUILD_ID " "
    SETUP_STRING(CIUKI_BUILD_EPOCH) "\n";

static bool active;

uint32_t setup_build_identity_length(void) { return sizeof(setup_build_identity) - 1; }

void setup_mode_init(void)
{
    active = setup_mode_parse(g_boot.flags, g_boot.options, sizeof(g_boot.options));
}

bool setup_mode(void) { return active; }
bool storage_setup_requested(void) { return active; }

bool setup_memory_refused(void)
{
    if (!active) return false;
    uint64_t kib = setup_usable_kib(g_boot.e820, g_boot.e820_count);
    if (kib >= SETUP_MIN_USABLE_KIB) return false;
    /* Before any driver activation or process: the console shows the log
     * (quiet boots included) and the kernel stays idle. No panic path. */
    console_wake();
    klog("%s", SETUP_MEMORY_TEXT);
    klog("[setup] refused reason=memory usable_kib=%llu", (unsigned long long)kib);
    return true;
}

/* Storage steps of Setup mode, called by storage_init (fs/mount.c) before
 * the ATA disks are scanned, so C:/D: keep their letters (plan 3.3). A drive
 * that has just read the boot sectors is normally ready; a short bounded
 * wait covers the post-reset unit attentions and spin-up (SFF-8020i 10.6). */
#define SETUP_READY_ATTEMPTS 20u
#define SETUP_READY_MS 250u
void storage_setup_media(struct storage *s, struct ata_device *devices[STORAGE_DISKS])
{
    klog("[storage] setup_mode=1 writable=0");
    const struct cdview_identity kid = { (uint32_t)(uintptr_t)setup_build_identity,
        setup_build_identity, setup_build_identity_length() };
    for (unsigned slot = 0; slot < STORAGE_DISKS && !s->boot_cd.bound; slot++) {
        struct ata_device *d = devices[slot];
        if (!d || !d->atapi || optical_bind(s, slot, d)) continue;
        int e = -FS_EIO;
        for (unsigned attempt = 0; attempt < SETUP_READY_ATTEMPTS; attempt++) {
            e = atapi_ready(d);
            if (!e || d->media == ATAPI_NO_MEDIUM || d->block.quarantined) break;
            task_sleep_ms(SETUP_READY_MS);
        }
        if (e || (d->toc.valid && d->toc.first_audio)) {
            klog("[setup] candidate slot=%u result=%d reason=%s", slot, e, e ? "not_ready" : "audio_cd");
            continue;
        }
        if (storage_setup_identify(s, slot, &d->block, d->session_lba, g_boot.options,
                                   sizeof(g_boot.options), &kid) >= 0)
            s->optical_bindings[slot].attempted_generation = d->block.media_generation;
    }
    storage_setup_finish(s);
}

/* The zero-write assertion: write-class ATA commands per disk since boot. */
void storage_setup_records(struct ata_device *devices[STORAGE_DISKS])
{
    for (unsigned slot = 0; slot < STORAGE_DISKS; slot++)
        if (devices[slot] && !devices[slot]->atapi)
            klog("[setup] writes disk=%u count=%llu", slot, (unsigned long long)devices[slot]->write_commands);
}
