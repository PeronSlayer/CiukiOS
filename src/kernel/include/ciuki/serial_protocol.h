/* Bounded serial wire codecs. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CIUKI_SERIAL_PROTOCOL_H
#define CIUKI_SERIAL_PROTOCOL_H
#include <ciuki/abi.h>
#include <stdbool.h>
struct serial_pnp_id {
    uint16_t revision;
    char hid[8], serial[9], class_name[33], compatible[41], name[41];
};
int serial_pnp_parse(const uint8_t *, unsigned, struct serial_pnp_id *);
unsigned serial_pnp_mouse(const struct serial_pnp_id *);
struct serial_pnp_ops {
    int (*modem)(void *, unsigned);
    int (*setup)(void *);
    int (*dsr)(void *); /* 0/1, or negative errno */
    int (*receive)(void *, uint8_t *); /* one byte, or negative errno */
    void (*pause)(void *); /* one ms, thread context */
    uint64_t (*now)(void *); /* monotonic ms */
    void *context;
};
int serial_pnp_enumerate(const struct serial_pnp_ops *, struct serial_pnp_id *);
struct serial_mouse_decoder {
    uint64_t last;
    uint8_t bytes[5], count, protocol, buttons;
};
struct serial_mouse_packet { int dx, dy, wheel; unsigned buttons; };
bool serial_mouse_byte(struct serial_mouse_decoder *, uint8_t, bool error,
                       uint64_t ms, struct serial_mouse_packet *);
#endif
