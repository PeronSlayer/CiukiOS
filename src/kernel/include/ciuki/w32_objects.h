/* Additive f5-04 transport and kernel object interfaces.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_W32_OBJECTS_H
#define CIUKI_W32_OBJECTS_H
#include <ciuki/abi.h>
#define W32_OBJECT 0x100u
#define W32_WAIT 0x101u
#define W32_APC 0x102u
#define W32_CONTEXT 0x103u
#define W32_MESSAGE 0x104u
#define W32_SECTION 0x105u
#define W32_VIEW 0x106u
#define W32_SERVICE 0x200u
#define W32_CREATE_SUSPENDED 4u
#define W32_HANDLE_MAX 4096u
#define W32_OBJECT_MAX 8192u
#define W32_MESSAGE_MAX 10000u
#define W32_WAIT_MAX 64u
#define W32_INFINITE 0xffffffffu
#define W32_WAIT_OBJECT_0 0u
#define W32_WAIT_ABANDONED_0 0x80u
#define W32_WAIT_IO_COMPLETION 0xc0u
#define W32_WAIT_TIMEOUT 258u
#define W32_WAIT_FAILED 0xffffffffu
#define W32_WAIT_PENDING 0x103u
#define W32_WAIT_SENT 0x10000u
#define W32_ERROR_INVALID_HANDLE 6u
#define W32_ERROR_ALREADY_EXISTS 183u
#define W32_ERROR_NOT_OWNER 288u
#define W32_ERROR_TOO_MANY_POSTS 298u
#define W32_ERROR_NOT_ENOUGH_QUOTA 1816u
#define W32_ERROR_SERVICE_NOT_ACTIVE 1062u
#define W32_CURRENT_PROCESS 0xffffffffu
#define W32_CURRENT_THREAD 0xfffffffeu
#define W32_DUP_CLOSE_SOURCE 1u
#define W32_DUP_SAME_ACCESS 2u
#define W32_QS_KEY 1u
#define W32_QS_MOUSEMOVE 2u
#define W32_QS_MOUSEBUTTON 4u
#define W32_QS_POSTMESSAGE 8u
#define W32_QS_TIMER 0x10u
#define W32_QS_PAINT 0x20u
#define W32_QS_SENDMESSAGE 0x40u
#define W32_QS_HOTKEY 0x80u
#define W32_QS_ALL 0xffu
#define W32_FILE_MAP_COPY 1u
#define W32_FILE_MAP_WRITE 2u
#define W32_FILE_MAP_READ 4u
#define W32_SEC_PAGEFILE 1u
#define W32_SEC_FILE 2u
#define W32_SEC_IMAGE 3u
#define W32_SERVICE_DESKTOP 1u
#define W32_SERVICE_W32SRV 2u

enum w32_object_type {
    W32_OBJ_PROCESS = 1,
    W32_OBJ_THREAD,
    W32_OBJ_EVENT,
    W32_OBJ_MUTEX,
    W32_OBJ_SEMAPHORE,
    W32_OBJ_TIMER,
    W32_OBJ_FILE,
    W32_OBJ_SECTION,
    W32_OBJ_VIEW,
    W32_OBJ_QUEUE,
    W32_OBJ_WINSTATION,
    W32_OBJ_ENDPOINT
};
enum w32_object_action {
    W32_OBJ_CREATE = 1,
    W32_OBJ_OPEN,
    W32_OBJ_CLOSE,
    W32_OBJ_DUPLICATE,
    W32_OBJ_SET,
    W32_OBJ_RESET,
    W32_OBJ_PULSE,
    W32_OBJ_RELEASE,
    W32_OBJ_QUERY,
    W32_OBJ_INHERIT,
    W32_OBJ_SET_INHERIT,
    W32_OBJ_TIMER_SET,
    W32_OBJ_TIMER_CANCEL
};
/* All pointers are user VAs, u64 quantities are split words. result/error are
 * outputs (Win32 result and last-error); the transport returns NTSTATUS. */
struct ciuki_w32_object_args {
    struct ciuki_w32_header header;
    uint32_t action, type, handle, target, source, access, flags, name;
    uint32_t value, maximum, result, error, extra, reserved;
};
struct ciuki_w32_wait_args {
    struct ciuki_w32_header header;
    uint32_t handles, count, all, alertable, timeout, wake_mask, signal, result, error, reserved;
};
struct ciuki_w32_apc_args {
    struct ciuki_w32_header header;
    uint32_t action, thread, routine, argument, result, error;
};
#define W32_APC_QUEUE 1u
#define W32_APC_REGISTER 2u
#define W32_APC_RETURN 3u
struct ciuki_w32_context_args {
    struct ciuki_w32_header header;
    uint32_t action, thread, context, flags, result, error;
};
#define W32_CTX_SUSPEND 1u
#define W32_CTX_RESUME 2u
#define W32_CTX_GET 3u
#define W32_CTX_SET 4u
struct ciuki_w32_message {
    uint32_t window, message, wparam, lparam, time;
    int32_t x, y;
};
struct ciuki_w32_message_args {
    struct ciuki_w32_header header;
    uint32_t action, tid, bits, token;
    struct ciuki_w32_message message;
    uint32_t buffer, bytes, result, error;
};
#define W32_MSG_POST 1u
#define W32_MSG_GET 2u
#define W32_MSG_SEND 3u
#define W32_MSG_REPLY 4u
#define W32_MSG_POLL_REPLY 5u
#define W32_MSG_STATUS 6u
struct ciuki_w32_section_args {
    struct ciuki_w32_header header;
    uint32_t kind, file, bytes, protect, name, flags, result, error;
};
struct ciuki_w32_view_args {
    struct ciuki_w32_header header;
    uint32_t action, section, address, bytes, offset_lo, offset_hi, access, result, error, reserved;
};
#define W32_VIEW_MAP 1u
#define W32_VIEW_UNMAP 2u
#define W32_VIEW_FLUSH 3u
struct ciuki_w32_parameters_v2 {
    struct ciuki_w32_parameters core;
    uint32_t desktop_endpoint, w32srv_endpoint;
};
_Static_assert(sizeof(struct ciuki_w32_parameters_v2) == 32, "parameters v2");
struct ciuki_w32_service_args {
    struct ciuki_w32_header header;
    uint32_t action, service, pid, endpoint, result, error;
};
#define W32_SVC_REGISTER 1u
#define W32_SVC_CONNECT 2u
#define W32_SVC_ACCEPT 3u
#define W32_SVC_PRIORITY 4u
#define W32_SVC_FOCUS 5u
#define W32_SVC_INPUT 6u
_Static_assert(sizeof(struct ciuki_w32_object_args) == 72, "object wire");
_Static_assert(sizeof(struct ciuki_w32_wait_args) == 56, "wait wire");
_Static_assert(sizeof(struct ciuki_w32_apc_args) == 40, "APC wire");
_Static_assert(sizeof(struct ciuki_w32_context_args) == 40, "context wire");
_Static_assert(sizeof(struct ciuki_w32_message) == 28, "MSG wire");
_Static_assert(sizeof(struct ciuki_w32_message_args) == 76, "message wire");
_Static_assert(sizeof(struct ciuki_w32_section_args) == 48, "section wire");
_Static_assert(sizeof(struct ciuki_w32_view_args) == 56, "view wire");
_Static_assert(sizeof(struct ciuki_w32_service_args) == 40, "service wire");

#include <ciuki/win32.h>
#include <ciuki/ledger.h>
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
    uint32_t access, generation;
    bool inherit;
};
struct w32_process {
    struct w32_process *next;
    struct process *owner;
    struct w32_handle_entry *chunks[64];
    struct w32_object *identity;
    struct w32_view *views;
    uint32_t handles, private_bytes, apc_dispatcher;
    uint32_t endpoint[2], service;
    bool closing;
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
    bool safe, ending;
};
struct w32_object {
    struct w32_object *next;
    uint32_t refs, type, id, exit_code;
    uint32_t section_limit, section_protect;
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
            uint32_t count, bits, tid, classes[8];
        } queue;
        struct {
            struct file_description *description;
            uint32_t access, share;
        } file;
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
int w32_process_init(struct process *p);
int w32_thread_objects_init(struct proc_thread *t);
void w32_process_cleanup(struct process *p);
void w32_thread_objects_cleanup(struct proc_thread *t);
void w32_thread_died(struct proc_thread *t);
void w32_process_died(struct process *p);
struct w32_object *w32_object_new(uint32_t type);
void w32_retain(struct w32_object *o);
void w32_release(struct w32_object *o);
struct w32_object *w32_lookup(struct proc_thread *t, uint32_t handle, uint32_t type);
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
void w32_service_delegate(struct process *parent, struct process *child, const char *path);
int w32_services_attach(struct process *p, uint32_t endpoints[2]);
void w32_services_cleanup(struct process *p);
int channel_connect(struct process *client, struct process *server, int32_t fds[2]);
#endif
