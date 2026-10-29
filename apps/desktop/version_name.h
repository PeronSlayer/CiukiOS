/* SPDX-License-Identifier: GPL-2.0-only
 * Presentation of the kernel's SemVer identity; no public ABI changes. */
#ifndef APPS_VERSION_NAME_H
#define APPS_VERSION_NAME_H
#include <stdio.h>
#include <string.h>
static inline void ciuki_version_name(char *out, size_t size, const char *version)
{
    const char *pre = strchr(version, '-');
    const char *stage = NULL, *number = NULL;
    if (pre) {
        if (!strncmp(pre, "-alpha.", 7)) { stage = "Alpha"; number = pre + 7; }
        else if (!strncmp(pre, "-beta.", 6)) { stage = "Beta"; number = pre + 6; }
        else if (!strncmp(pre, "-rc.", 4)) { stage = "Release Candidate"; number = pre + 4; }
    }
    if (stage) snprintf(out, size, "%.*s %s %s", (int)(pre - version), version, stage, number);
    else snprintf(out, size, "%s", version[0] ? version : "unknown");
}
#endif
