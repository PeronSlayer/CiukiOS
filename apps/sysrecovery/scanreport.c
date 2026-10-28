/* SPDX-License-Identifier: MIT
 * G14 ScanDisk report text (docs/design/startup-recovery.md 2.4): the
 * summary and statistics of Windows 98's "ScanDisk Results" window, the
 * details of what was found, and SCANDISK.LOG. */
#define _POSIX_C_SOURCE 200809L
/* System headers first: abi.h then leaves the C library's macros alone. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "recovery.h"

/* sec-sysvc2: the text is shared with the kernel's startup-report job
 * (ciuki/scandisk_text.h), so both write the same SCANDISK.LOG entries. */
#define SCANDISK_TEXT_VFORMAT vsnprintf
#include "../../src/kernel/include/ciuki/scandisk_text.h"
size_t scandisk_text(const struct ciuki_scandisk *r, char *out, size_t cap) { return scandisk_report_text(r, out, cap); }
int scandisk_log(const char *root, const struct ciuki_scandisk *r, int64_t now)
{
    time_t t = (time_t)now; struct tm tm; char when[32];
    if (localtime_r(&t, &tm)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm); else snprintf(when, sizeof(when), "?");
    enum { TEXT = 2048, ENTRY = 2304 };
    char *text = malloc(TEXT + ENTRY);
    if (!text) return -ENOMEM;
    char *entry = text + TEXT;
    size_t n = scandisk_log_entry(r, when, text, TEXT, entry, ENTRY);
    /* sec-sysvc: a system log, appended through the SYSTEM log service. */
    int e = sr_log_append(root, "SCANDISK.LOG", entry, n);
    free(text);
    return e;
}
