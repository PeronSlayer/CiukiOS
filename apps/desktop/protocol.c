/* SPDX-License-Identifier: MIT */
#include "protocol.h"
#include <limits.h>
#include <string.h>
uint32_t desk_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
void desk_put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8));
}
void desk_message(struct ciuki_message *m, unsigned op, unsigned n)
{
    memset(m, 0, sizeof(*m));
    m->length = n;
    m->data[0] = DESK_VERSION;
    m->data[1] = (uint8_t)op;
}
void desk_event_message(struct ciuki_message *m, unsigned op,
                        const struct ciuki_input_event *e)
{
    desk_message(m, op, 44);
    desk_put32(m->data + 4, e->sequence);
    desk_put32(m->data + 8, e->source);
    desk_put32(m->data + 12, e->generation);
    desk_put32(m->data + 16, e->type);
    desk_put32(m->data + 20, (uint32_t)e->monotonic_ns);
    desk_put32(m->data + 24, (uint32_t)(e->monotonic_ns >> 32));
    desk_put32(m->data + 28, (uint32_t)e->code);
    desk_put32(m->data + 32, (uint32_t)e->value);
    desk_put32(m->data + 36, (uint32_t)e->value2);
    desk_put32(m->data + 40, e->lost_count);
}
int desk_surface_valid(const struct ciuki_surface_info *s)
{
    if (!s || s->size != sizeof(*s) || !s->width || !s->height ||
        s->width > CIUKI_SURFACE_DIMENSION_MAX ||
        s->height > CIUKI_SURFACE_DIMENSION_MAX ||
        s->format != CIUKI_SURFACE_XRGB8888 || s->stride != s->width * 4)
        return 0;
    uint32_t n = (s->stride * s->height + 4095u) & ~4095u;
    return n <= CIUKI_SURFACE_MAX && s->allocation_bytes == n;
}
static int valid_event(unsigned op, const struct ciuki_input_event *e)
{
    if (op == DESK_KEY) {
        if (e->type == CIUKI_INPUT_KEY)
            return e->code >= 0 && e->code <= 0x200 &&
                   e->value >= 0 && e->value <= 2 && !e->value2 && !e->lost_count;
        if (e->type == CIUKI_INPUT_TEXT)
            return e->code >= 0 && e->code <= 0x10ffff &&
                   !(e->code >= 0xd800 && e->code <= 0xdfff) &&
                   !e->value && !e->value2 && !e->lost_count;
        if (e->type == CIUKI_INPUT_RESYNC)
            return !e->code && e->value >= 0 && e->value <= 7 && !e->value2;
        return 0;
    }
    if (op == DESK_MOTION)
        return e->type == CIUKI_INPUT_MOTION && !e->code && !e->lost_count;
    return op == DESK_BUTTON && e->type == CIUKI_INPUT_BUTTON &&
           e->code >= 1 && e->code <= 3 && e->value >= 0 &&
           e->value <= 1 && !e->value2 && !e->lost_count;
}
int desk_parse(const struct ciuki_message *m, int client,
               const struct ciuki_surface_info *s, struct desk_packet *p)
{
    if (m->length < 4 || m->length > 256 || m->fd_count > 4 || m->reserved ||
        m->data[0] != DESK_VERSION || m->data[2] || m->data[3]) return 0;
    for (unsigned i = m->length; i < 256; i++) if (m->data[i]) return 0;
    for (unsigned i = m->fd_count; i < 4; i++) if (m->fds[i]) return 0;
    for (unsigned i = 0; i < m->fd_count; i++) if (m->fds[i] < 0) return 0;
    memset(p, 0, sizeof(*p));
    p->opcode = m->data[1];
    if (m->fd_count != (unsigned)(client && p->opcode == DESK_CREATE_WINDOW)) return 0;
    unsigned n = 4;
    switch (p->opcode) {
    case DESK_HELLO: case DESK_CLOSE:
        if (!client) return 0;
        break;
    case DESK_CLOSED:
        if (client) return 0;
        break;
    case DESK_PING: case DESK_PONG:
        n = 8;
        if (m->length != n) return 0;
        p->serial = desk_get32(m->data + 4);
        break;
    case DESK_CREATE_WINDOW: {
        if (!client || m->length < 17 || !desk_surface_valid(s)) return 0;
        p->width = desk_get32(m->data + 4);
        p->height = desk_get32(m->data + 8);
        unsigned t = desk_get32(m->data + 12);
        if (!t || t > DESK_TITLE_MAX || m->length != 16 + t ||
            p->width != s->width || p->height != s->height) return 0;
        for (unsigned i = 0; i < t; i++) {
            if (m->data[16+i] < 32 || m->data[16+i] > 126) return 0;
            p->title[i] = (char)m->data[16+i];
        }
        n = 16 + t;
        break;
    }
    case DESK_DAMAGE:
        if (!client || m->length != 20 || !desk_surface_valid(s)) return 0;
        p->x = (int32_t)desk_get32(m->data + 4);
        p->y = (int32_t)desk_get32(m->data + 8);
        p->width = desk_get32(m->data + 12);
        p->height = desk_get32(m->data + 16);
        if (p->x < 0 || p->y < 0 || (uint32_t)p->x > s->width ||
            (uint32_t)p->y > s->height || p->width > s->width - (uint32_t)p->x ||
            p->height > s->height - (uint32_t)p->y) return 0;
        n = 20;
        break;
    case DESK_MOVE:
        if (!client || m->length != 12) return 0;
        p->x = (int32_t)desk_get32(m->data + 4);
        p->y = (int32_t)desk_get32(m->data + 8);
        if (p->x < -2048 || p->x > 2048 || p->y < -2048 || p->y > 2048) return 0;
        n = 12;
        break;
    case DESK_CONFIGURE:
        if (client || m->length != 12) return 0;
        p->width = desk_get32(m->data + 4);
        p->height = desk_get32(m->data + 8);
        if (!p->width || !p->height || p->width > 2048 || p->height > 2048) return 0;
        n = 12;
        break;
    case DESK_FOCUS:
        if (client || m->length != 8) return 0;
        p->focused = desk_get32(m->data + 4);
        if (p->focused > 1) return 0;
        n = 8;
        break;
    case DESK_KEY: case DESK_MOTION: case DESK_BUTTON:
        if (client || m->length != 44) return 0;
        p->event.sequence = desk_get32(m->data + 4);
        p->event.source = desk_get32(m->data + 8);
        p->event.generation = desk_get32(m->data + 12);
        p->event.type = desk_get32(m->data + 16);
        p->event.monotonic_ns = desk_get32(m->data + 20) |
                                 (uint64_t)desk_get32(m->data + 24) << 32;
        p->event.code = (int32_t)desk_get32(m->data + 28);
        p->event.value = (int32_t)desk_get32(m->data + 32);
        p->event.value2 = (int32_t)desk_get32(m->data + 36);
        p->event.lost_count = desk_get32(m->data + 40);
        if (!valid_event(p->opcode, &p->event)) return 0;
        n = 44;
        break;
    default: return 0;
    }
    return m->length == n;
}
