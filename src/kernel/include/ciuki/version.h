/* CiukiOS version string, e.g. "0.9.0 pre-alpha". scripts/build_kernel.py
 * passes it as -DCIUKI_VERSION from config/version.json (the single source);
 * host harnesses that do not define it get "unknown".
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VERSION_H
#define CIUKI_VERSION_H

#ifndef CIUKI_VERSION
#define CIUKI_VERSION "unknown"
#endif

#endif
