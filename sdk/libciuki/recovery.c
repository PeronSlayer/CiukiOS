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
/* ---- sec-sysvc: SYSTEM services (ciuki/recovery.h) ---- */
int ciuki_boot_config_set(const char *text, size_t length)
{
    if (!text || length > CIUKI_BOOT_CONFIG_MAX) { errno = EINVAL; return -1; }
    struct ciuki_boot_config r;
    memset(&r, 0, sizeof(r));
    r.version = CIUKI_BOOT_CONFIG_VERSION; r.size = sizeof(r); r.action = CIUKI_BOOT_CONFIG_SET;
    r.length = (uint32_t)length; memcpy(r.text, text, length);
    return call(CIUKI_RECOVERY_BOOT_CONFIG, &r, sizeof(r));
}
static int log_name(struct ciuki_system_log *r, const char *name)
{
    size_t n = name ? strlen(name) : sizeof(r->name);
    if (n >= sizeof(r->name)) { errno = EINVAL; return -1; }
    memcpy(r->name, name, n + 1);
    return 0;
}
static void log_record(struct ciuki_system_log *r, uint32_t action, unsigned scope)
{
    memset(r, 0, sizeof(*r));
    r->version = CIUKI_SYSTEM_LOG_VERSION; r->size = sizeof(*r); r->action = action; r->scope = scope;
}
int ciuki_system_log_list(unsigned scope, unsigned index, char name[CIUKI_SYSTEM_LOG_NAME_MAX], uint32_t *total)
{
    if (!name || !total) { errno = EINVAL; return -1; }
    struct ciuki_system_log r;
    log_record(&r, CIUKI_SYSTEM_LOG_LIST, scope); r.index = index;
    if (call(CIUKI_RECOVERY_LOG, &r, sizeof(r))) return -1;
    memcpy(name, r.name, sizeof(r.name)); name[sizeof(r.name) - 1] = 0; *total = r.total;
    return 0;
}
int ciuki_system_log_read(unsigned scope, const char *name, uint32_t offset, void *buffer, size_t capacity,
                          size_t *got, uint32_t *total)
{
    if (!got || !total || (capacity && !buffer) || capacity > CIUKI_SYSTEM_LOG_DATA_MAX) { errno = EINVAL; return -1; }
    struct ciuki_system_log r;
    log_record(&r, CIUKI_SYSTEM_LOG_READ, scope);
    if (log_name(&r, name)) return -1;
    r.offset = offset; r.buffer = CU_PTR(buffer); r.bytes = (uint32_t)capacity;
    if (call(CIUKI_RECOVERY_LOG, &r, sizeof(r))) return -1;
    *got = r.bytes; *total = r.total;
    return 0;
}
int ciuki_system_log_write(unsigned scope, const char *name, int replace, const void *data, size_t bytes, uint32_t limit)
{
    if ((bytes && !data) || bytes > CIUKI_SYSTEM_LOG_DATA_MAX) { errno = EINVAL; return -1; }
    struct ciuki_system_log r;
    log_record(&r, replace ? CIUKI_SYSTEM_LOG_REPLACE : CIUKI_SYSTEM_LOG_APPEND, scope);
    if (log_name(&r, name)) return -1;
    r.buffer = CU_PTR(data); r.bytes = (uint32_t)bytes; r.limit = replace ? 0 : limit;
    return call(CIUKI_RECOVERY_LOG, &r, sizeof(r));
}
int ciuki_driver_pack(struct ciuki_driver_pack_job *job)
{
    if (!job) { errno = EINVAL; return -1; }
    return call(CIUKI_RECOVERY_DRIVER_PACK, job, sizeof(*job));
}
int ciuki_sfc_restore(const char *path, const char *source)
{
    struct ciuki_sfc_restore r;
    memset(&r, 0, sizeof(r));
    if (!path || !source || strlen(path) >= sizeof(r.path) || strlen(source) >= sizeof(r.source)) { errno = EINVAL; return -1; }
    r.version = CIUKI_SFC_RESTORE_VERSION; r.size = sizeof(r);
    memcpy(r.path, path, strlen(path) + 1); memcpy(r.source, source, strlen(source) + 1);
    return call(CIUKI_RECOVERY_SFC_RESTORE, &r, sizeof(r));
}
/* ---- sec-sysvc2: SYSTEM jobs and sealed boot settings (ciuki/recovery.h) ---- */
int ciuki_system_job(unsigned job, uint32_t *done)
{
    struct ciuki_system_job r;
    memset(&r, 0, sizeof(r));
    r.version = CIUKI_SYSTEM_JOB_VERSION; r.size = sizeof(r); r.job = job;
    int e = call(CIUKI_RECOVERY_JOB, &r, sizeof(r));
    if (done) *done = r.done;
    return e;
}
int ciuki_system_setting_set(unsigned setting, const char *text, size_t length)
{
    if ((length && !text) || length > CIUKI_SYSTEM_SETTING_MAX) { errno = EINVAL; return -1; }
    struct ciuki_system_setting r;
    memset(&r, 0, sizeof(r));
    r.version = CIUKI_SYSTEM_SETTING_VERSION; r.size = sizeof(r); r.action = CIUKI_SYSTEM_SETTING_SET;
    r.setting = setting; r.buffer = CU_PTR(text); r.length = (uint32_t)length;
    return call(CIUKI_RECOVERY_SETTING, &r, sizeof(r));
}
