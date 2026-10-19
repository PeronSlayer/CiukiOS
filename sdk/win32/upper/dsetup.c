/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors.
 * F5 installation identity only. Never opens or copies a redistributable.
 * DirectSetup has no requested-version argument; later-package admission is
 * the installer's separate preflight responsibility. */
#include "w32/base.h"

/* Period DirectSetup component/driver/UI/test flags. Unknown components,
 * including Managed DirectX (0x4000), are not covered by the F5 identity. */
static LONG setup_result(ULONG flags) {
    return flags & ~0x30033a7fu ? -1 : 0; /* BADWINDOWSVERSION or SUCCESS. */
}

LONG W32API DirectXSetupA(void *window, const char *root, ULONG flags) {
    (void)window;
    (void)root;
    return setup_result(flags); /* Already installed, no restart. */
}

LONG W32API DirectXSetupW(void *window, const WCHAR *root, ULONG flags) {
    (void)window;
    (void)root;
    return setup_result(flags);
}

LONG W32API DirectXSetupGetVersion(ULONG *version, ULONG *revision) {
    if (version)
        *version = 0x00040007u;
    if (revision)
        *revision = 716;
    return 1;
}

LONG W32API CiukiSetupDllMain(void *module, ULONG reason, void *reserved) {
    (void)module;
    (void)reason;
    (void)reserved;
    return 1;
}
