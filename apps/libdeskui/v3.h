/* SPDX-License-Identifier: MIT
 * User-mode personality bridge. Window/task ids are never native PIDs.
 * PONG is explicit: an I/O thread must not hide a stalled guest UI. */
#ifndef CIUKI_DESKUI_V3_H
#define CIUKI_DESKUI_V3_H
#include "../desktop/protocol.h"
struct du3_transport {
    int (*send)(int, const struct ciuki_message *, uint32_t);
    int (*recv)(int, struct ciuki_message *, uint32_t);
    int (*info)(int, struct ciuki_surface_info *);
    int (*close)(int);
};
struct du3_connection {
    int channel, disconnected;
    uint32_t request;
    struct du3_transport transport;
};
int du3_connect(struct du3_connection *, int channel, const struct du3_transport *);
/* Returns request token, or zero with errno. Caller retains its fd on send;
 * transport transfers a reference. Payload is the documented byte encoding. */
uint32_t du3_request(struct du3_connection *, unsigned opcode, uint32_t window,
                     uint32_t generation, const void *payload, unsigned bytes, int fd);
/* 1 event, 0 clean EOF, -1 errno (including EAGAIN); no implicit PONG. */
int du3_next(struct du3_connection *, struct desk_packet *);
int du3_ui_progress(struct du3_connection *, uint32_t window, uint32_t generation, uint32_t ping);
uint32_t du3_window_order(struct du3_connection *, uint32_t window,
    uint32_t generation, uint32_t flags, uint32_t after, uint32_t after_generation,
    uint32_t owner, uint32_t owner_generation);
/* G11B: serial 0 / answer 1 registers this native window; for a power
 * EVENT, answer 2 marks dequeue, 0 vetoes a query, 1 agrees/completes.
 * Explicit UI-thread replies; no acknowledgement is hidden in du3_next. */
uint32_t du3_power_reply(struct du3_connection *, uint32_t window,
    uint32_t generation, uint32_t serial, uint32_t answer);
#endif
