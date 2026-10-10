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
    union { struct surface *surface; struct { struct channel *channel; unsigned side; } endpoint; } u;
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
int surface_read(struct surface *s, uint32_t offset, void *out, uint32_t bytes);
const struct ciuki_surface_info *surface_geometry(const struct surface *s);

int channel_pair(struct process *p, int32_t fds[2]);
int channel_send(struct process *p, int32_t fd, uint32_t message, uint32_t flags);
int channel_recv(struct process *p, int32_t fd, uint32_t message, uint32_t flags);

/* No public syscall can install a grant. Caller is the bootstrap controller. */
int grants_install(struct process *p, int32_t fds[2]);
int grant_display_info(struct process *p, int32_t fd, struct ciuki_display_info *out);
int grant_present(struct process *p, int32_t display, int32_t surface, const struct ciuki_rect *rect);
int grant_input_read(struct process *p, int32_t fd, uint32_t events, uint32_t capacity);
int desktop_clip(const struct ciuki_surface_info *s, const struct fb_device *d,
                  const struct ciuki_rect *r, struct ciuki_rect *out);
void desktop_input_event(const struct input_event *in, uint32_t lost, uint32_t buttons,
                          struct ciuki_input_event *out);
int32_t desktop_syscall(struct trap_frame *tf);
/* f2-03 dispatch delegates recognized desktop fds to this helper. */
bool desktop_fd_syscall(struct trap_frame *tf);
#endif
