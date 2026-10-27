/* Win32 copied-record transport. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_W32_TRANSPORT_H
#define CIUKI_W32_TRANSPORT_H
#include <ciuki/abi.h>
#include <ciuki/present_abi.h>
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
    W32_OBJ_ENDPOINT,
    W32_OBJ_FIND,
    W32_OBJ_SNAPSHOT,
    /* sec-k06: CREATE(source=process handle, access=8) opens a query token.
     * QUERY(name=credential VA, maximum=sizeof(ciuki_credential)) copies the
     * frozen credential; result=required bytes, value=privilege bit mask.
     * Tokens cannot be inherited or transferred to another process. */
    W32_OBJ_TOKEN
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
/* SELECT: message.window=-1 means thread messages, otherwise exact HWND or
 * zero=all. Optional buffer/bytes is an array of <=256 matching HWNDs.
 * message.message/wparam are inclusive range bounds (0/0=all).
 * token input: REMOVE=1, SENT_ONLY=2. Output token identifies a sent record.
 * STATUS returns changed QS bits in message.wparam and clears requested bits.
 * MARK sets synthetic wake bits for this or another same-process thread; the
 * mask is in message.wparam. QUIT is coalesced and bypasses retrieval filters. */
#define W32_MSG_SELECT 7u
#define W32_MSG_MARK 8u
#define W32_MSG_CANCEL 9u
#define W32_MSG_NOTIFY 10u
#define W32_MSG_QUIT 11u
#define W32_MSG_START 12u /* start synchronous send, return token without blocking */
/* INPUT_IDLE: token is a process handle. Query the initial GUI idle latch;
 * result is WAIT_OBJECT_0 or WAIT_TIMEOUT; invalid/non-GUI processes fail.
 * With tid=this thread and token=0, publish a yielding empty PeekMessage;
 * the kernel rechecks that this thread's entire input queue is empty. */
#define W32_MSG_INPUT_IDLE 13u
#define W32_SELECT_REMOVE 1u
#define W32_SELECT_SENT_ONLY 2u
/* wait.reserved bit zero selects new arrivals; zero retains the f5-04 ABI. */
#define W32_WAIT_NEW_INPUT 1u
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

#endif
