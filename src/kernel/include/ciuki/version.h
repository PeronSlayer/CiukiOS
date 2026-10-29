/* CiukiOS version string, e.g. "1.0.0-beta.1". scripts/build_kernel.py
 * passes it as -DCIUKI_VERSION from config/version.json (the single source);
 * host harnesses that do not define it get "unknown".
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VERSION_H
#define CIUKI_VERSION_H

#ifndef CIUKI_VERSION
#define CIUKI_VERSION "unknown"
#endif

#ifndef CIUKI_VERSION_FULL
#define CIUKI_VERSION_FULL CIUKI_VERSION
#endif

#endif
