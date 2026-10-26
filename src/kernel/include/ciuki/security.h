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
/* Internal role bootstrap. Only PID 1's fixed manifest uses this interface. */
uint32_t security_role(const struct process *p);
uint32_t security_process_generation(const struct process *p);
uint32_t security_role_generation(const struct process *p);
bool security_logon(const struct process *p);
const char *security_role_image(uint32_t role);
int security_role_prepare(struct process *parent, struct process *child, uint32_t role, const char *image);
struct process *security_service_server(uint32_t service, uint32_t session);
struct process *security_session_desktop(uint32_t session);
bool security_roles_active(void); /* sticky after first LOGON reservation */
int security_service_allowed(const struct process *client, uint32_t service);
int security_subject_acquire_endpoint(struct process *, int32_t, uint32_t, struct security_subject **);
/* Private service seam: 1 for the same live endpoint/process lease, 0 for
 * different live links, -EACCES for missing/non-link subjects, -ESRCH revoked.
 * Separate acquisitions may have different reference addresses. */
int security_subject_same_link(const struct security_subject *, const struct security_subject *);
/* action = (service << 16) | operation; scope = required peer session (0 = any).
 * This checks transport liveness and role routes; servers additionally check
 * account/object permissions under this retained credential. */
int security_subject_check(const struct security_subject *, uint32_t action, uint32_t scope);
uint32_t security_subject_role(const struct security_subject *);
uint32_t security_subject_pid(const struct security_subject *);
uint32_t security_subject_process_generation(const struct security_subject *);
uint32_t security_session_generation(uint32_t session);
int channel_endpoint_handoff(struct process *, int32_t, struct process *, int32_t *, bool);
int channel_security_link(struct process *, uint32_t operation, const struct ciuki_security_link *,
                          struct ciuki_security_link *, uint32_t *result);
int channel_kernel_receive(struct process *, int32_t, struct ciuki_message *);
int channel_control_bind(struct process *, int32_t);
int channel_control_install(struct process *, struct proc_object *);
void channel_security_revoke(struct process *);
void channel_security_poll(void);
uint32_t channel_peer_service(const struct security_peer *);
/* k07 installs the backend only after qualifying fences/maps/leases. Missing
 * adapter fails closed. prepare installs fresh session console/stdio and
 * purpose grants; it must never duplicate the caller's descriptions. All
 * prepare/trusted may yield. conceal/activate are nonblocking console
 * transitions, called with interrupts masked; conceal masks presentation/input
 * and starts fences, returning EBUSY while they drain. No device wait or
 * allocation belongs in these two callbacks. */
struct security_grant_ops {
    int (*prepare)(struct process *, uint32_t purpose, struct ciuki_security_launch *);
    int (*conceal)(uint32_t session, uint32_t grant_generation);
    int (*activate)(uint32_t session, uint32_t grant_generation);
    void (*release)(uint32_t session);
    int (*trusted)(struct process *); /* LOGON_UI private console/input/display */
};
void security_set_grant_ops(const struct security_grant_ops *ops);
int security_trusted_prepare(struct process *p);
int security_launch_resources(struct process *, struct ciuki_security_launch *);
int security_launch_environment(struct proc_strings *, const struct ciuki_security_launch *);
int proc_security_launch_prepare(struct process *parent, struct ciuki_security_launch *, struct process **);
void security_launch_preparation_done(uint32_t session);
int security_console_attention(uint32_t reason); /* kernel physical input/resume only */
void security_tick(void); /* bounded deadline wake; no I/O or waits */
void security_poll(void); /* PID 1 maintenance: deadlines and pair failures */
/* Committed per-profile TZ loader. Absent key => UTC0; absent adapter may use
 * UTC0, never the SYSTEM process environment. EIO/corruption fails launch. */
int security_profile_timezone(const struct security_subject *, const struct ciuki_security_profile *, char *, uint32_t);
#endif
