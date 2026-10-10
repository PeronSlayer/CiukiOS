/* SPDX-License-Identifier: MIT
 * F2 desktop protocol v1. Byte encoding, independent of C alignment/padding.
 * Install as ciuki/desktop.h when the lead extends the SDK overlay. */
#ifndef CIUKI_APP_DESKTOP_PROTOCOL_H
#define CIUKI_APP_DESKTOP_PROTOCOL_H
#include <ciuki/abi.h>
#include <stdint.h>
#define DESK_VERSION 1u
#define DESK_TITLE_MAX 63u
enum desk_opcode {
    DESK_HELLO = 1, DESK_CREATE_WINDOW, DESK_DAMAGE, DESK_MOVE, DESK_CLOSE,
    DESK_PING, DESK_CONFIGURE, DESK_KEY, DESK_MOTION, DESK_BUTTON,
    DESK_FOCUS, DESK_PONG, DESK_CLOSED
};
struct desk_packet {
    unsigned opcode;
    int32_t x, y;
    uint32_t width, height, serial, focused;
    char title[DESK_TITLE_MAX + 1];
    struct ciuki_input_event event;
};
uint32_t desk_get32(const uint8_t *p);
void desk_put32(uint8_t *p, uint32_t v);
void desk_message(struct ciuki_message *m, unsigned opcode, unsigned length);
void desk_event_message(struct ciuki_message *m, unsigned opcode,
                        const struct ciuki_input_event *e);
/* from_client selects allowed direction. surface required only for CREATE. */
int desk_parse(const struct ciuki_message *m, int from_client,
               const struct ciuki_surface_info *surface, struct desk_packet *out);
int desk_surface_valid(const struct ciuki_surface_info *s);
#endif
