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
        (m->data[0] != DESK_VERSION && m->data[0] != DESK_SHELL_VERSION) ||
        m->data[3] || (m->data[2] &&
        (m->data[0] != DESK_SHELL_VERSION || m->data[2] != DESK_RESIZE_MINOR))) return 0;
    for (unsigned i = m->length; i < 256; i++) if (m->data[i]) return 0;
    for (unsigned i = m->fd_count; i < 4; i++) if (m->fds[i]) return 0;
    for (unsigned i = 0; i < m->fd_count; i++) if (m->fds[i] < 0) return 0;
    memset(p, 0, sizeof(*p));
    p->opcode = m->data[1];
    int resize = m->data[2] == DESK_RESIZE_MINOR;
    unsigned fds = client && p->opcode == DESK_CREATE_WINDOW ? 1u : 0u;
    if (client && resize && p->opcode == DESK_DAMAGE && m->length == 24 && desk_get32(m->data + 20)) fds = 1;
    if (m->fd_count != fds) return 0;
    unsigned n = 4;
    switch (p->opcode) {
    case DESK_HELLO: case DESK_CLOSE:
        if (!client) return 0;
        break;
    case DESK_CLOSED:
        if (client) return 0;
        break;
    case DESK_CLOSE_REQUEST:
        if (client || m->data[0] != DESK_SHELL_VERSION) return 0;
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
        if (!t || t > DESK_TITLE_MAX || m->length != 16 + t + (resize ? 28u : 0u) ||
            p->width != s->width || p->height != s->height) return 0;
        for (unsigned i = 0; i < t; i++) {
            if (m->data[16+i] < 32 || m->data[16+i] > 126) return 0;
            p->title[i] = (char)m->data[16+i];
        }
        n = 16 + t;
        if (resize) {
            p->flags = desk_get32(m->data + n);
            p->min_width = desk_get32(m->data + n + 4); p->min_height = desk_get32(m->data + n + 8);
            p->max_width = desk_get32(m->data + n + 12); p->max_height = desk_get32(m->data + n + 16);
            p->step_width = desk_get32(m->data + n + 20); p->step_height = desk_get32(m->data + n + 24);
            if (p->flags & ~DESK_RESIZABLE) return 0;
            if (p->flags) {
                if (!p->min_width || !p->min_height || p->min_width > p->width || p->min_height > p->height ||
                    p->max_width < p->width || p->max_height < p->height ||
                    p->max_width > CIUKI_SURFACE_DIMENSION_MAX || p->max_height > CIUKI_SURFACE_DIMENSION_MAX ||
                    !p->step_width || !p->step_height || p->step_width > p->max_width || p->step_height > p->max_height ||
                    (p->width - p->min_width) % p->step_width || (p->height - p->min_height) % p->step_height) return 0;
            } else if (p->min_width || p->min_height || p->max_width || p->max_height || p->step_width || p->step_height) return 0;
            n += 28;
        }
        break;
    }
    case DESK_SET_TITLE: {
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length < 9) return 0;
        unsigned t = desk_get32(m->data + 4);
        if (!t || t > DESK_TITLE_MAX || m->length != 8 + t) return 0;
        for (unsigned i = 0; i < t; i++) {
            if (m->data[8 + i] < 32 || m->data[8 + i] > 126) return 0;
            p->title[i] = (char)m->data[8 + i];
        }
        n = 8 + t;
        break;
    }
    case DESK_DAMAGE:
        if (!client || m->length != (resize ? 24u : 20u) || !desk_surface_valid(s)) return 0;
        p->x = (int32_t)desk_get32(m->data + 4);
        p->y = (int32_t)desk_get32(m->data + 8);
        p->width = desk_get32(m->data + 12);
        p->height = desk_get32(m->data + 16);
        if (p->x < 0 || p->y < 0 || (uint32_t)p->x > s->width ||
            (uint32_t)p->y > s->height || p->width > s->width - (uint32_t)p->x ||
            p->height > s->height - (uint32_t)p->y) return 0;
        p->serial = resize ? desk_get32(m->data + 20) : 0;
        if (p->serial && (p->x || p->y || p->width != s->width || p->height != s->height)) return 0;
        n = resize ? 24 : 20;
        break;
    case DESK_MOVE:
        if (!client || m->length != 12) return 0;
        p->x = (int32_t)desk_get32(m->data + 4);
        p->y = (int32_t)desk_get32(m->data + 8);
        if (p->x < -2048 || p->x > 2048 || p->y < -2048 || p->y > 2048) return 0;
        n = 12;
        break;
    case DESK_CONFIGURE:
        if (client || m->length != (resize ? 20u : 12u)) return 0;
        p->width = desk_get32(m->data + 4);
        p->height = desk_get32(m->data + 8);
        if (!p->width || !p->height || p->width > 2048 || p->height > 2048) return 0;
        if (resize) {
            p->serial = desk_get32(m->data + 12); p->flags = desk_get32(m->data + 16);
            if ((p->flags & ~DESK_CONFIGURE_COMMITTED) || (!p->serial && !p->flags)) return 0;
        }
        n = resize ? 20 : 12;
        break;
    case DESK_FOCUS:
        if (client || m->length != 8) return 0;
        p->focused = desk_get32(m->data + 4);
        if (p->focused > 1) return 0;
        n = 8;
        break;
    case DESK_WINDOWS_QUERY:
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length != 8) return 0;
        p->serial = desk_get32(m->data + 4);
        n = 8;
        break;
    case DESK_WINDOW_INFO: {
        if (client || m->data[0] != DESK_SHELL_VERSION || m->length < 32) return 0;
        p->serial = desk_get32(m->data + 4);
        p->index = desk_get32(m->data + 8);
        p->count = desk_get32(m->data + 12);
        p->window_id = desk_get32(m->data + 16);
        p->pid = desk_get32(m->data + 20);
        p->flags = desk_get32(m->data + 24);
        unsigned t = desk_get32(m->data + 28);
        if (p->count > 16 || t > DESK_TITLE_MAX || m->length != 32 + t ||
            (p->flags & ~DESK_WINDOW_FLAGS)) return 0;
        if (!p->count ? p->index || p->window_id || p->pid || p->flags || t :
            p->index >= p->count || !p->window_id) return 0;
        for (unsigned i = 0; i < t; i++) {
            if (m->data[32+i] < 32 || m->data[32+i] > 126) return 0;
            p->title[i] = (char)m->data[32+i];
        }
        n = 32 + t;
        break;
    }
    case DESK_END_TASK:
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length != 16) return 0;
        p->serial = desk_get32(m->data + 4);
        p->window_id = desk_get32(m->data + 8);
        p->pid = desk_get32(m->data + 12);
        if (!p->window_id && !p->pid) return 0;
        n = 16;
        break;
    case DESK_SWITCH_TO:
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length != 12) return 0;
        p->serial = desk_get32(m->data + 4);
        p->window_id = desk_get32(m->data + 8);
        if (!p->window_id) return 0;
        n = 12;
        break;
    case DESK_LAUNCH: {
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length < 13) return 0;
        p->serial = desk_get32(m->data + 4);
        unsigned t = desk_get32(m->data + 8);
        if (!t || t > DESK_TITLE_MAX || m->length < 12 + t) return 0;
        for (unsigned i = 0; i < t; i++) {
            if (m->data[12+i] < 33 || m->data[12+i] > 126) return 0;
            p->title[i] = (char)m->data[12+i];
        }
        n = 12 + t;
        if (m->length != n) {   /* optional argument: u32 length, then that many bytes (no control characters) */
            if (m->length < n + 5) return 0;
            unsigned a = desk_get32(m->data + n);
            if (!a || a > DESK_ARG_MAX || m->length != n + 4 + a) return 0;
            for (unsigned i = 0; i < a; i++) {
                unsigned char ch = m->data[n+4+i];
                if (ch < 32 || ch == 127) return 0;
                p->argument[i] = (char)ch;
            }
            n += 4 + a;
        }
        break;
    }
    case DESK_TASK_RESULT:
        if (client || m->data[0] != DESK_SHELL_VERSION || m->length != 16) return 0;
        p->serial = desk_get32(m->data + 4);
        p->request = desk_get32(m->data + 8);
        p->result = (int32_t)desk_get32(m->data + 12);
        if ((p->request != DESK_WINDOWS_QUERY && p->request != DESK_END_TASK &&
             p->request != DESK_SWITCH_TO && p->request != DESK_LAUNCH && p->request != DESK_SETTINGS && p->request != DESK_MIXER_SET) ||
            p->result < -4095) return 0;
        n = 16;
        break;
    case DESK_SETTINGS:
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length != 24) return 0;
        p->serial = desk_get32(m->data + 4);
        p->request = desk_get32(m->data + 8);
        for (unsigned i = 0; i < 3; i++) p->values[i] = desk_get32(m->data + 12 + i * 4);
        if (p->request == DESK_SETTINGS_RELOAD) {
            if (p->values[0] || p->values[1] || p->values[2]) return 0;
        } else if (p->request == DESK_SETTINGS_DISPLAY) {
            if (p->values[0] > UINT16_MAX || p->values[1] || p->values[2]) return 0;
        } else if (p->request != DESK_SETTINGS_MASTER || p->values[0] > 100 || p->values[1] > 100 || p->values[2] > 1) return 0;
        n = 24;
        break;
    case DESK_MIXER_SET:
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length != 20) return 0;
        p->serial = desk_get32(m->data + 4);
        p->width = desk_get32(m->data + 8);    /* left */
        p->height = desk_get32(m->data + 12);  /* right */
        p->flags = desk_get32(m->data + 16);   /* mute */
        if (p->width > 100 || p->height > 100 || p->flags > 1) return 0;
        n = 20;
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
