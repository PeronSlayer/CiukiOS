/* Storage policy helpers; namespace lock held for physical volume identity.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SECURITY_STORAGE_H
#define CIUKI_SECURITY_STORAGE_H
#include <ciuki/security_device.h>
#include <ciuki/storage.h>

static inline bool security_storage_system_drive(const struct storage *s, unsigned drive)
{
    if (!s || drive >= 26) return true;
    if (drive == 2) return true;
    const struct storage_volume *v = &s->volumes[drive];
    const struct storage_volume *system = &s->volumes[2];
    return v->present && !s->setup_mode &&
        ((!v->removable && v->disk == 0) ||
         (v->part.parent && (v->part.parent == s->raw[0].dev ||
          (system->present && v->part.parent == system->part.parent))));
}
/* A SYSTEM worker needs the retained client's authority as well as its own.
 * A user worker cannot enter the protected volume engine directly; the
 * typed kernel broker below uses the protected SYSTEM controller. */
static inline bool security_storage_admin(const struct security_subject *subject)
{
    const struct ciuki_credential *c = security_subject_credential(subject);
    if (!security_subject_live(subject) || !c) return false;
    if (c->uid == CIUKI_UID_SYSTEM && !c->session_id) return true;
    for (unsigned i=0; i<c->group_count; i++)
        if (c->groups[i] == CIUKI_GID_ADMINISTRATORS) return true;
    return false;
}
static inline int security_storage_maintenance(struct process *worker,
    const struct security_subject *subject, struct storage *s, unsigned drive)
{
    if (!s || drive>=26) return -EINVAL;
    if (!security_process_live(worker) || !security_storage_admin(subject)) return -EPERM;
    return security_storage_system_drive(s,drive) && !security_device_system(worker) ? -EPERM : 0;
}
/* Native media/recovery calls are typed kernel services, not a raw-I/O
 * grant. The protected SYSTEM controller admits their retained client before
 * the existing kernel storage engine runs. This never changes the caller's
 * credential or hands out a SYSTEM descriptor. */
static inline int security_storage_broker(const struct security_subject *subject,
    struct storage *s, unsigned drive)
{
    return security_storage_maintenance(proc_supervisor(),subject,s,drive);
}
/* Internal retained-client entry points for SYSTEM maintenance brokers.
 * The broker retains the subject across the entire call and owns the worker
 * output buffers. No client ABI exposes a subject pointer or SYSTEM launch. */
int media_devctl_subject(struct process *, const struct security_subject *, int32_t,
                         uint32_t, uint32_t, uint32_t);
int32_t recovery_syscall_subject(struct process *, const struct security_subject *, struct trap_frame *);
#endif
