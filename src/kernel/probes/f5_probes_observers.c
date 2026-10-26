/* Runtime-only core fixture observers. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/probe.h>
#include <ciuki/kernel.h>
#include <ciuki/supervisor.h>
#include "../../probes/win32_core/controller.h"
extern int probes_w32_core_fixture(unsigned);
static int probe_kernel32(void) { return probes_w32_core_fixture(W32_FIXTURE_KERNEL32); }
CIUKI_F5_PROBE("w32-kernel32",probe_kernel32);
static int probe_fullscreen(void) { return probes_w32_core_fixture(W32_FIXTURE_FULLSCREEN); }
CIUKI_F5_PROBE("w32-fullscreen",probe_fullscreen);
static int probe_registry(void) { return probes_w32_core_fixture(W32_FIXTURE_REGISTRY); }
CIUKI_F5_PROBE("w32-registry",probe_registry);
static int probe_audio(void) { return probes_w32_core_fixture(W32_FIXTURE_AUDIO); }
CIUKI_F5_PROBE("w32-audio",probe_audio);
static int probe_gdi32(void) { return probes_w32_core_fixture(W32_FIXTURE_GDI32); }
CIUKI_F5_PROBE("w32-gdi32",probe_gdi32);
static int probe_crash(void) { return probes_w32_core_fixture(W32_FIXTURE_CRASH); }
CIUKI_F5_PROBE("w32-crash",probe_crash);
static int probe_user32(void) { return probes_w32_core_fixture(W32_FIXTURE_USER32); }
CIUKI_F5_PROBE("w32-user32",probe_user32);
static int probe_codecs(void) { return probes_w32_core_fixture(W32_FIXTURE_CODECS); }
CIUKI_F5_PROBE("w32-codecs",probe_codecs);
static int probe_ddraw(void) { return probes_w32_core_fixture(W32_FIXTURE_DDRAW); }
CIUKI_F5_PROBE("w32-ddraw",probe_ddraw);
static int probe_input(void) { return probes_w32_core_fixture(W32_FIXTURE_INPUT); }
CIUKI_F5_PROBE("w32-input",probe_input);
static int probe_q3_lease_eio(void)
{
    if (!supervisor_interactive_test(&g_boot)) {
        rec_emit("q3-lease-eio","FAIL","reason=qemu-only");
        return 1;
    }
    supervisor_bootstrap();
    rec_emit("q3-lease-eio","READY","fault=EIO desktop=interactive");
    /* The dispatcher's normal continuation overlays evidence pages. Keep the
     * ordinary desktop in charge throughout this interactive reproduction. */
    for (;;) task_sleep_ms(60000);
}
CIUKI_F5_PROBE("q3-lease-eio",probe_q3_lease_eio);
