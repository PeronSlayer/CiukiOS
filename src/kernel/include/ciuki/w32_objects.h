/* Additive f5-04 transport and kernel object interfaces.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_W32_OBJECTS_H
#define CIUKI_W32_OBJECTS_H
#include <ciuki/w32_transport.h>
#include <ciuki/win32.h>
#include <ciuki/ledger.h>
#include <ciuki/w32_diagnostics.h>
struct w32_diagnostic_state;
struct w32_object;
struct w32_process;
struct w32_thread;
struct w32_wait;
struct w32_view;
struct px_node;
struct file_description;
struct w32_account {
    uint32_t objects, handles, sections, views, pages, cow_credits, messages, apcs, endpoints;
    uint32_t metadata_bytes, private_bytes;
};
struct w32_handle_entry {
    struct w32_object *object;
    uint32_t access;
    bool inherit;
};
struct w32_process {
    struct w32_process *next;
    struct process *owner;
    struct w32_handle_entry *chunks[64];
    struct w32_object *identity;
    struct w32_view *views;
    struct px_node *drive_cwd[26];
    uint32_t handles, private_bytes, apc_dispatcher;
    uint32_t endpoint[2], service, diagnostic_count, priority_class;
    uint32_t peak_handles, peak_threads, exit_reason;
    struct w32_diagnostic_state *diagnostics;
    bool closing, gui, input_idle;
    uint64_t launch_feedback_until; /* monotonic ms; zero after presentation/idle */
};
struct w32_apc_node {
    struct w32_apc_node *next;
    uint32_t routine, argument, low, high;
};
struct w32_thread {
    struct w32_object *identity, *queue;
    struct w32_apc_node *head, *tail;
    struct ciuki_w32_context *context, *apc_context;
    struct trap_frame *frame;
    uint32_t suspend, apcs;
    bool safe, ending, terminate_requested;
    uint32_t terminate_code;
    int32_t relative_priority;
};
struct w32_object {
    struct w32_object *next;
    uint32_t refs, type, id, exit_code;
    uint32_t section_limit, section_protect;
    uint32_t security_uid, security_session;
    uint16_t *name;
    bool manual, signalled, abandoned;
    union {
        struct {
            uint32_t owner, recursion;
        } mutex;
        struct {
            uint32_t count, maximum;
        } semaphore;
        struct {
            uint64_t due;
            uint32_t period, tid, routine, argument;
            bool armed;
        } timer;
        struct {
            void *head, *tail;
            uint32_t count, bits, tid, classes[8], changed, synthetic, quit_code;
            bool quit;
        } queue;
        struct {
            struct file_description *description;
            uint32_t access, share;
        } file;
        struct ciuki_credential token;
        void *section;
    } u;
};
_Static_assert(sizeof(struct w32_object) <= 512, "object budget");
void *w32_alloc(uint32_t bytes);
void w32_free(void *p, uint32_t bytes);
void w32_snapshot(struct w32_account *out);
extern struct w32_account w32_accounts;
extern struct w32_object *w32_objects;
extern struct kwait w32_changed;
uint64_t w32_now(void);
uint64_t w32_system_time(void);
void w32_changed_wake(void);
struct process *w32_process_for_memory(struct uaddr *u);
int w32_shared_map(struct process *p);
/* Physical frame of the page every Win32 process maps at CIUKI_W32_SHARED;
 * 0 while no process maps it (sections.c). Published by w32_shared_publish. */
extern uint32_t w32_shared_frame;
void w32_shared_publish(void);
int w32_process_init(struct process *p);
int w32_thread_objects_init(struct proc_thread *t);
void w32_process_cleanup(struct process *p);
void w32_thread_objects_cleanup(struct proc_thread *t);
void w32_thread_died(struct proc_thread *t);
void w32_unpark(struct proc_thread *t);     /* resume of a thread parked at user return */
void w32_services_desktop_died(uint32_t desktop_pid); /* retire the dead desktop's window server */
void w32_process_died(struct process *p);
struct w32_object *w32_object_new(uint32_t type);
void w32_retain(struct w32_object *o);
void w32_release(struct w32_object *o);
struct w32_object *w32_lookup(struct proc_thread *t, uint32_t handle, uint32_t type);
bool w32_handle_access(struct proc_thread *, uint32_t handle, uint32_t rights);
uint32_t w32_handle_install(
    struct process *p, struct w32_object *o, uint32_t access, bool inherit, uint32_t *handle);
uint32_t w32_handle_close(struct process *p, uint32_t handle);
uint32_t w32_handles_inherit(struct process *child, const struct process *parent);
uint32_t w32_named_create(struct process *p, uint32_t type, const uint16_t *name, uint32_t access,
    bool inherit, struct w32_object **object, uint32_t *handle, bool *existing);
uint32_t w32_object_call(struct proc_thread *t, struct ciuki_w32_object_args *a);
uint32_t w32_wait_call(struct proc_thread *t, struct ciuki_w32_wait_args *a);
uint32_t w32_wait_try(
    struct proc_thread *t, const uint32_t *handles, unsigned count, bool all, uint32_t mask);
void w32_wait_pump(void);
uint32_t w32_apc_queue(
    struct proc_thread *t, uint32_t routine, uint32_t argument, uint32_t low, uint32_t high);
uint32_t w32_apc_call(
    struct proc_thread *t, struct ciuki_w32_apc_args *a, struct trap_frame *tf, bool *replaced);
bool w32_apc_deliver(struct proc_thread *t, struct trap_frame *tf);
uint32_t w32_context_call(struct proc_thread *t, struct ciuki_w32_context_args *a);
void w32_user_return(struct proc_thread *t, struct trap_frame *tf);
void w32_timer_poll(void);
uint32_t w32_message_call(struct proc_thread *t, struct ciuki_w32_message_args *a);
void w32_messages_thread_died(struct proc_thread *t);
void w32_queue_destroy(struct w32_object *o);
uint32_t w32_queue_bits(struct proc_thread *t);
uint32_t w32_queue_changed(struct proc_thread *t);
uint32_t w32_section_call(struct proc_thread *t, struct ciuki_w32_section_args *a);
uint32_t w32_view_call(struct proc_thread *t, struct ciuki_w32_view_args *a);
void w32_section_destroy(struct w32_object *o);
void w32_views_cleanup(struct process *p);
int w32_view_access(struct uaddr *u, uint32_t va, bool write);
bool w32_view_contains(struct uaddr *u, uint32_t va);
uint32_t w32_view_protect(struct uaddr *u, struct ciuki_w32_vm *a);
int w32_image_section_map(struct process *, const struct ciuki_file *, const struct w32_image *, bool);
int ua_w32_bind(struct uaddr *u, uint32_t va, uint32_t phys, uint32_t prot, bool shared);
int w32_file_sync(struct px_node *node);
void w32_file_changed(struct px_node *node, uint64_t off, const void *buffer, uint32_t bytes);
uint32_t w32_service_call(struct proc_thread *t, struct ciuki_w32_service_args *a);
bool w32_service_desktop_client(struct process *, uint32_t pid);
void w32_service_delegate(struct process *parent, struct process *child, const char *path);
int w32_services_attach(struct process *p, uint32_t endpoints[2]);
void w32_services_cleanup(struct process *p);
int channel_connect(struct process *client, struct process *server, int32_t fds[2]);
uint32_t w32_compat_call(struct proc_thread *, struct ciuki_w32_compat_args *);
uint32_t w32_report_call(struct proc_thread *, struct ciuki_w32_report_args *);
void w32_diagnostic_record(struct proc_thread *, const struct ciuki_w32_diagnostic *);
void w32_diagnostic_exit(struct process *);
void w32_diagnostic_cleanup(struct process *);
void w32_crash_capture(struct proc_thread *, const struct ciuki_w32_context *, uint32_t, const char *, uint32_t);
bool w32_io_emulate(struct proc_thread *, struct trap_frame *, const uint8_t *, unsigned);
uint32_t w32_initial_cw(struct process *);
void w32_log(const char *, ...);
void w32_refusal_queue(struct process *, uint32_t, const char *, uint32_t);
bool w32_refuse_image_name(struct process *, const char *);
int w32_diagnostic_image(struct process *,uint32_t,uint32_t);
#endif
