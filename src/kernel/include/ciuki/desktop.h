/* F2 desktop objects. Public constants/layouts live only in abi.h.
 * All object/queue mutations use the non-preemptible UP kernel boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DESKTOP_H
#define CIUKI_DESKTOP_H
#include <ciuki/process.h>
#include <ciuki/fbdev.h>
#include <ciuki/input.h>

struct surface;
struct channel;
enum desktop_kind { DESKTOP_SURFACE, DESKTOP_CHANNEL, DESKTOP_DISPLAY, DESKTOP_INPUT };
struct desktop_description {
    struct proc_object object;
    enum desktop_kind kind;
    uint32_t references, flags, maximum, owner;
    void *display_fences;
    uint64_t display_sequence;
    union { struct surface *surface; struct { struct channel *channel; unsigned side; } endpoint; } u;
    /* sec-k07: grant domain (trusted logon UI or one session's desktop). */
    bool grant_trusted;
    uint32_t grant_session;
};
struct desktop_ledger { uint32_t descriptions, surfaces, pages, channels, messages, grants; };
extern struct desktop_ledger desktop_objects; /* internal object accounting */
void desktop_snapshot(struct desktop_ledger *out);
struct desktop_activity { uint64_t presents, input_events; };
void desktop_activity_snapshot(struct desktop_activity *out);
struct desktop_description *desktop_description_new(enum desktop_kind kind, uint32_t flags);
struct desktop_description *desktop_fd(const struct process *p, int32_t fd);
int desktop_fd_slot(const struct process *p, unsigned start);
int desktop_fd_install(struct process *p, struct desktop_description *d, uint32_t flags);
bool desktop_object_is_channel(const struct proc_object *o);
int desktop_close(struct process *p, int32_t fd);
int desktop_dup(struct process *p, int32_t fd, int32_t target, bool exact, uint32_t flags);
int desktop_fcntl(struct process *p, int32_t fd, uint32_t cmd, uint32_t arg);
/* Release hooks used exclusively by the description's last reference. */
void channel_release(struct desktop_description *d);
void grant_release(struct desktop_description *d);

int surface_create(struct process *p, uint32_t width, uint32_t height, uint32_t format);
int32_t surface_map(struct process *p, int32_t fd, uint32_t prot);
int surface_info(struct process *p, int32_t fd, struct ciuki_surface_info *out);
int surface_attachment(struct process *p, int32_t fd, struct desktop_description **out);
void surface_retain(struct surface *s);
void surface_release(struct surface *s);
int surface_write(struct surface *, uint32_t offset, const void *, uint32_t bytes);
int surface_read(struct surface *s, uint32_t offset, void *out, uint32_t bytes);
const struct ciuki_surface_info *surface_geometry(const struct surface *s);
struct swdib_target;
int surface_batch_target(struct desktop_description *, uint32_t, struct swdib_target *);
int surface_batch_reserve(struct surface *);
void surface_batch_unreserve(struct surface *);
int grant_batch_request(struct process *, int32_t, uint32_t, bool);
void grant_batch_close(struct desktop_description *);
int grant_batch_authorized(struct process *, struct desktop_description *);
void grant_batch_busy(bool);
bool grant_batch_ram(const struct process *, uint32_t, uint32_t);
void *desktop_buffer_alloc(uint32_t bytes, enum mm_owner);
void desktop_buffer_free(void *, uint32_t bytes, enum mm_owner);

int channel_pair(struct process *p, int32_t fds[2]);
int channel_poll_events(struct desktop_description *d);
bool channel_service_pending(struct process *, uint32_t service);
bool channel_connected(const struct process *a, const struct process *b);
int channel_send(struct process *p, int32_t fd, uint32_t message, uint32_t flags);
int channel_recv(struct process *p, int32_t fd, uint32_t message, uint32_t flags);

/* No public syscall can install a grant. Caller is the bootstrap controller. */
int grants_install(struct process *p, int32_t fds[2]);
/* --- sec-k07 (begin): secure console domains (proc/grants.c) ---
 * install_trusted: LOGON_UI's private pair; install_session: suspended
 * SESSION_LAUNCH desktop pair. conceal/activate/release are nonblocking and
 * IRQ-safe (called by the k01 console coordinator); conceal returns EBUSY
 * until every user pixel source is fenced and the neutral frame is painted
 * by grants_console_neutral (thread context, PID 1). */
struct grants_console_state {
    uint32_t trusted_owner, user_owner, user_session, active_session;
    bool trusted, neutral_pending, draining;
    uint64_t neutral_frames;
};
int grants_install_trusted(struct process *p, int32_t fds[2]);
int grants_install_session(struct process *p, uint32_t session, int32_t fds[2]);
int grants_console_conceal(uint32_t session);
int grants_console_activate(uint32_t session);
void grants_console_release(uint32_t session);
int grants_console_neutral(void);
bool grants_console_trusted(void);
void grants_console_snapshot(struct grants_console_state *out);
/* sec-user-06: trusted-gate latency of this boot (contract 4.3; self-test
 * security.lock_latency). A concealment that hid a visible session runs
 * from grants_console_conceal to the neutral black frame (neutral_ms) and
 * to the trusted UI's first presented frame (trusted_ms); a restoration
 * from grants_console_activate to the session's first frame (restore_ms).
 * Milliseconds of g_ticks; *_max over the boot, last_* the latest. */
struct grants_console_timing {
    uint32_t concealments, restorations;
    uint32_t last_neutral_ms, last_trusted_ms, last_restore_ms;
    uint32_t max_neutral_ms, max_trusted_ms, max_restore_ms;
};
void grants_console_timing(struct grants_console_timing *out);
/* --- sec-k07 (end) --- */
int grant_display_info(struct process *p, int32_t fd, struct ciuki_framebuffer_info *out);
int grant_present(struct process *p, int32_t display, int32_t surface, const struct ciuki_rect *rect);
int grant_accel(struct process *p, int32_t display, const struct ciuki_accel_request *request);
int grant_accel_info(struct process *, int32_t, const struct ciuki_accel_request *, struct ciuki_accel_info *);
int grant_input_read(struct process *p, int32_t fd, uint32_t events, uint32_t capacity);
int grant_input_devctl(struct process *, int32_t, uint32_t command, uint32_t address, uint32_t bytes);
int desktop_clip(const struct ciuki_surface_info *s, const struct fb_device *d,
                  const struct ciuki_rect *r, struct ciuki_rect *out);
void desktop_input_event(const struct input_event *in, uint32_t lost, uint32_t buttons,
                          struct ciuki_input_event *out);
int32_t desktop_syscall(struct trap_frame *tf);
/* f2-03 dispatch delegates recognized desktop fds to this helper. */
bool desktop_fd_syscall(struct trap_frame *tf);
#endif
