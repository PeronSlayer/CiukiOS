/* Immutable subjects and bounded security sessions; no user token setters.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SECURITY_H
#define CIUKI_SECURITY_H
#include <ciuki/process.h>
struct security_subject;
void security_init(struct process *supervisor);
int security_process_prepare(struct process *child, const struct process *parent);
void security_process_release(struct process *p);
void security_process_stopping(struct process *p);
const struct ciuki_credential *security_credential(const struct process *p);
bool security_system(const struct process *p);
bool security_admin(const struct process *p);
bool security_process_live(const struct process *p);
bool security_same(const struct process *a, const struct process *b);
int security_subject_get(struct security_subject *subject);
void security_subject_put(struct security_subject *subject);
bool security_subject_live(const struct security_subject *subject);
bool security_subject_matches(const struct security_subject *subject, const struct process *p);
const struct ciuki_credential *security_subject_credential(const struct security_subject *subject);
int security_query_process(struct process *caller, uint32_t pid, struct ciuki_credential *out);
int security_session_create(const struct ciuki_credential *credential,
                            const struct ciuki_security_profile *profile, uint32_t *id);
int security_session_query(struct process *caller, uint32_t id, uint32_t *state);
int security_session_end(uint32_t id, bool begin, uint32_t *state);
int security_session_attach(struct process *child, uint32_t id);
int security_session_profile(uint32_t id, struct ciuki_security_profile *out);
int security_syscall(struct trap_frame *tf);
/* k02 adapter: verifies a committed resolved owner binding; absent => EIO.
 * No caller can register an adapter through a syscall. */
int security_owner_verify(const struct ciuki_security_profile *profile);
/* k03 resolves, authorizes and freezes the executable in one operation under
 * the child's retained subject; a separate pathname precheck is insufficient.
 * Missing enforcement denies cross-credential launch. */
int security_exec_open(struct process *child, const char *path, struct ciuki_file *out);
/* Internal bootstrap only. No executable name in public spawn grants SYSTEM. */
int security_bootstrap_desktop(struct process *child);
int proc_spawn_credential(uint32_t spawn, uint32_t session, uint32_t outer, uint32_t *pid);
/* Retains the actual endpoint and subject across asynchronous service work. */
struct security_peer {
    struct proc_object *endpoint;
    struct security_subject *subject;
    uint32_t generation;
};
int channel_peer_get(struct process *p, int32_t fd, struct security_peer *out);
bool channel_peer_live(const struct security_peer *peer);
void channel_peer_put(struct security_peer *peer);
void channel_fd_closed(struct process *p, struct proc_object *object);
int channel_process_prepare(struct process *child);
void channel_process_publish(struct process *child);
void channel_process_abort(struct process *child);
#endif
