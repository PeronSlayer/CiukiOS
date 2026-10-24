/* G14: system call 99 wrappers (ciuki/recovery.h).
 * SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/recovery.h>
#include <errno.h>
#include <string.h>

static int call(uint32_t op, void *record, uint32_t size)
{
    int e = ciuki_error(CU_CALL(RECOVERY, op, CU_PTR(record), size, 0, 0, 0));
    if (e) { errno = e; return -1; }
    return 0;
}
static int status_call(uint32_t op, struct ciuki_recovery_status *s)
{
    s->version = CIUKI_RECOVERY_VERSION; s->size = sizeof(*s);
    return call(op, s, sizeof(*s));
}
int ciuki_recovery_status(struct ciuki_recovery_status *status)
{
    if (!status) { errno = EINVAL; return -1; }
    memset(status, 0, sizeof(*status));
    return status_call(CIUKI_RECOVERY_STATUS, status);
}
int ciuki_recovery_configure(uint8_t disabled_classes, uint8_t requests)
{
    struct ciuki_recovery_status s;
    memset(&s, 0, sizeof(s));
    s.disabled = disabled_classes; s.request = requests;
    return status_call(CIUKI_RECOVERY_SET, &s);
}
int ciuki_recovery_enable(uint8_t classes)
{
    struct ciuki_recovery_status s;
    memset(&s, 0, sizeof(s));
    s.skipped = classes;
    return status_call(CIUKI_RECOVERY_ENABLE, &s);
}
static int scandisk_call(uint32_t op, unsigned drive, uint32_t flags, struct ciuki_scandisk *out)
{
    if (!out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof(*out));
    out->version = CIUKI_SCANDISK_VERSION; out->size = sizeof(*out); out->drive = drive; out->flags = flags;
    return call(op, out, sizeof(*out));
}
int ciuki_scandisk(unsigned drive, uint32_t flags, struct ciuki_scandisk *out)
{
    return scandisk_call(CIUKI_RECOVERY_SCANDISK, drive, flags, out);
}
int ciuki_scandisk_report(unsigned drive, struct ciuki_scandisk *out)
{
    return scandisk_call(CIUKI_RECOVERY_REPORT, drive, 0, out);
}
