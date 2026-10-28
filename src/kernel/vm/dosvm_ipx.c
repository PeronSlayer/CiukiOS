/* Original implementation of the legacy ECB API; no Novell binary/source.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "dosvm_ipx.h"
#include <ciuki/ipx_wire.h>
enum { ECB_FREE, ECB_LISTEN, ECB_SEND, ECB_TIMER, ECB_CALLBACK };
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)p[0] << 8 | p[1]; }
static uint16_t swap(uint16_t n) { return (uint16_t)(n << 8 | n >> 8); }
static void al(struct dosvm_ipx_regs *r, uint8_t n) { r->ax = (r->ax & 0xff00) | n; }
static uint32_t farptr(uint16_t seg, uint16_t off) { return ((uint32_t)seg << 4) + off; }
static int pin(struct dosvm_ipx *v, uint16_t seg, uint16_t off, unsigned n, bool write, uint32_t *ticket)
{
    if (!n || n > 0x10000u - off || n > 0x10fff0u - farptr(seg, off)) return -EFAULT;
    *ticket = 0;
    int e = v->ops.pin(v->ops.arg, farptr(seg, off), n, write, ticket);
    return e ? e : *ticket ? 0 : -EFAULT;
}
static int socket_index(const struct dosvm_ipx *v, uint16_t number)
{
    for (unsigned i = 0; i < DOSVM_IPX_SOCKETS; i++) if (v->socket[i].open && v->socket[i].number == number) return (int)i;
    return -1;
}
static void unpin_fragments(struct dosvm_ipx *v, struct dosvm_ipx_ecb *e)
{
    for (unsigned i = 0; i < e->fragments; i++) if (e->fragment[i].pin) {
        v->ops.unpin(v->ops.arg, e->fragment[i].pin); e->fragment[i].pin = 0;
    }
}
static void release(struct dosvm_ipx *v, struct dosvm_ipx_ecb *e)
{
    unpin_fragments(v, e);
    if (e->pin) v->ops.unpin(v->ops.arg, e->pin);
    if (e->esr_pin) v->ops.unpin(v->ops.arg, e->esr_pin);
    memset(e, 0, sizeof(*e));
}
static void complete(struct dosvm_ipx *v, struct dosvm_ipx_ecb *e, enum dosvm_ipx_completion code, bool callback)
{
    uint8_t status[2] = {0, (uint8_t)code};
    if (v->ops.write(v->ops.arg, e->linear + 8, status, 2)) { v->memory_errors++; callback = false; }
    unpin_fragments(v, e);
    if (callback && (e->esr_cs || e->esr_ip)) { e->state = ECB_CALLBACK; e->order = ++v->sequence; }
    else release(v, e);
}
int dosvm_ipx_init(struct dosvm_ipx *v, uint32_t generation, const uint8_t mac[6], const struct dosvm_ipx_ops *ops)
{
    if (!v || !generation || !mac || (mac[0] & 3) != 2 || !ops || !ops->pin || !ops->unpin ||
        !ops->read || !ops->write || !ops->send || !ops->upcall) return -EINVAL;
    memset(v, 0, sizeof(*v)); v->ops = *ops; v->generation = generation;
    memcpy(v->mac, mac, 6); v->next_dynamic = 0x4000; v->enabled = true; return 0;
}
static struct dosvm_ipx_ecb *snapshot(struct dosvm_ipx *v, struct dosvm_ipx_regs *r, bool timer, bool receive)
{
    uint32_t linear = farptr(r->es, r->si);
    struct dosvm_ipx_ecb *e = 0;
    for (unsigned i = 0; i < DOSVM_IPX_ECBS; i++) {
        if (v->ecb[i].state && v->ecb[i].linear == linear) { al(r, 0xff); return 0; }
        if (!v->ecb[i].state && !e) e = &v->ecb[i];
    }
    if (!e) { v->rejected++; al(r, 0xfe); return 0; }
    uint8_t raw[36 + 6 * DOSVM_IPX_FRAGMENTS];
    uint32_t ticket = 0;
    if (pin(v, r->es, r->si, 36, true, &ticket)) { al(r, 0xfd); return 0; }
    int error = v->ops.read(v->ops.arg, linear, raw, 36);
    v->ops.unpin(v->ops.arg, ticket);
    if (error) { v->memory_errors++; al(r, 0xfd); return 0; }
    unsigned fragments = timer ? 0 : le16(raw + 34);
    if ((!timer && (!fragments || fragments > DOSVM_IPX_FRAGMENTS)) ||
        pin(v, r->es, r->si, 36 + fragments * 6, true, &e->pin)) { al(r, 0xfd); return 0; }
    e->linear = linear; e->es = r->es; e->si = r->si; e->fragments = fragments;
    if (v->ops.read(v->ops.arg, linear, raw, 36 + fragments * 6) ||
        (!timer && le16(raw + 34) != fragments)) goto malformed;
    e->socket = be16(raw + 10); e->esr_ip = le16(raw + 4); e->esr_cs = le16(raw + 6);
    memcpy(e->immediate, raw + 28, 6);
    if (!timer && socket_index(v, e->socket) < 0) { release(v, e); al(r, 0xff); return 0; }
    if ((e->esr_cs || e->esr_ip) && pin(v, e->esr_cs, e->esr_ip, 1, false, &e->esr_pin)) goto malformed;
    for (unsigned i = 0; i < fragments; i++) {
        const uint8_t *f = raw + 36 + i * 6;
        struct dosvm_ipx_fragment *dst = &e->fragment[i];
        dst->length = le16(f + 4); dst->linear = farptr(le16(f + 2), le16(f));
        if (dst->length > 1500u - e->total ||
            (dst->length && pin(v, le16(f + 2), le16(f), dst->length, receive || i == 0, &dst->pin))) goto malformed;
        e->total += dst->length;
        /* Aliasing the ECB or another fragment would allow scatter writes to
         * corrupt control state. Reject the complete request before publication. */
        if (dst->length && dst->linear < linear + 36 + fragments * 6 && linear < dst->linear + dst->length) goto malformed;
        for (unsigned j = 0; j < i; j++) if (dst->length && e->fragment[j].length &&
            dst->linear < e->fragment[j].linear + e->fragment[j].length &&
            e->fragment[j].linear < dst->linear + dst->length) goto malformed;
    }
    if (!timer && (e->total < 30 || e->fragment[0].length < 30)) goto malformed;
    e->state = timer ? ECB_TIMER : receive ? ECB_LISTEN : ECB_SEND;
    e->order = ++v->sequence; e->event_al = timer ? 0 : 0xff;
    uint8_t status[2] = {timer ? 0xfd : receive ? 0xfe : 0xff, 0};
    if (v->ops.write(v->ops.arg, linear + 8, status, 2)) goto malformed;
    al(r, 0); return e;
malformed:
    v->rejected++;
    if (e->pin) complete(v, e, DOSVM_IPX_MALFORMED, false); else release(v, e);
    al(r, 0xfd); return 0;
}
static void close_socket(struct dosvm_ipx *v, uint16_t number)
{
    int index = socket_index(v, number); if (index < 0) return;
    for (unsigned i = 0; i < DOSVM_IPX_ECBS; i++) if (v->ecb[i].state && v->ecb[i].socket == number)
        complete(v, &v->ecb[i], DOSVM_IPX_CANCELLED, false);
    v->socket[index].open = false;
}
static int memory_buffer(struct dosvm_ipx *v, uint16_t seg, uint16_t off, void *data, unsigned n, bool write)
{
    uint32_t ticket = 0; int e = pin(v, seg, off, n, write, &ticket); if (e) return e;
    e = write ? v->ops.write(v->ops.arg, farptr(seg, off), data, n) : v->ops.read(v->ops.arg, farptr(seg, off), data, n);
    v->ops.unpin(v->ops.arg, ticket); return e;
}
int dosvm_ipx_interrupt(struct dosvm_ipx *v, unsigned vector, struct dosvm_ipx_regs *r)
{
    if (!v || !v->enabled || !r) return 0;
    if (vector == 0x2f) {
        if (r->ax != 0x7a00) return 0;
        al(r, 0xff); r->es = 0xf000; r->di = DOSVM_IPX_ENTRY; return 1;
    }
    if (vector != 0x7a) return 0;
    switch (r->bx) {
    case 0: {
        uint16_t number = swap(r->dx); unsigned index = DOSVM_IPX_SOCKETS;
        if (number && socket_index(v, number) >= 0) { al(r, 0xff); break; }
        if ((uint8_t)r->ax != 0 && (uint8_t)r->ax != 0xff) { al(r, 0xff); break; }
        for (unsigned i = 0; i < DOSVM_IPX_SOCKETS; i++) if (!v->socket[i].open) { index = i; break; }
        if (index == DOSVM_IPX_SOCKETS) { al(r, 0xfe); break; }
        if (!number) for (unsigned i = 0; i <= DOSVM_IPX_SOCKETS; i++) {
            number = v->next_dynamic++;
            if (v->next_dynamic >= 0x8000) v->next_dynamic = 0x4000;
            if (socket_index(v, number) < 0) break;
            number = 0;
        }
        if (!number) { al(r, 0xfe); break; }
        v->socket[index] = (struct dosvm_ipx_socket){number, v->psp, (uint8_t)r->ax, true};
        r->dx = swap(number); al(r, 0); break;
    }
    case 1: close_socket(v, swap(r->dx)); break;
    case 2: {
        uint8_t target[12];
        if (memory_buffer(v, r->es, r->si, target, 12, false) || memcmp(target, "\0\0\0\0", 4)) al(r, 0xfa);
        else { r->cx = 1; al(r, memory_buffer(v, r->es, r->di, target + 4, 6, true) ? 0xfa : 0); }
        break;
    }
    case 3: case 4: (void)snapshot(v, r, false, r->bx == 4); break;
    case 5: case 7: {
        uint16_t delay = r->ax;
        struct dosvm_ipx_ecb *e = snapshot(v, r, true, false);
        if (e) e->due = v->ticks + (delay ? delay : 1); break;
    }
    case 6: {
        uint32_t linear = farptr(r->es, r->si); al(r, 0xff);
        for (unsigned i = 0; i < DOSVM_IPX_ECBS; i++) if (v->ecb[i].state && v->ecb[i].linear == linear) {
            if (v->ecb[i].state == ECB_CALLBACK) { al(r, 0xf9); break; }
            complete(v, &v->ecb[i], DOSVM_IPX_CANCELLED, false); al(r, 0); break;
        }
        break;
    }
    case 8: r->ax = (uint16_t)v->ticks; break;
    case 9: {
        uint8_t address[10]; memcpy(address,v->network,4); memcpy(address + 4, v->mac, 6);
        al(r, memory_buffer(v, r->es, r->si, address, 10, true) ? 0xfd : 0); break;
    }
    case 10:
        dosvm_ipx_service(v, v->ticks);
        if (v->ops.relinquish) v->ops.relinquish(v->ops.arg);
        break;
    default: al(r, 0xff); break;
    }
    return 1;
}
static void send_packet(struct dosvm_ipx *v, struct dosvm_ipx_ecb *e)
{
    uint8_t *p = v->frame; unsigned at = 14;
    for (unsigned i = 0; i < e->fragments; i++) {
        struct dosvm_ipx_fragment *f = &e->fragment[i];
        if (f->length && v->ops.read(v->ops.arg, f->linear, p + at, f->length)) {
            v->memory_errors++; complete(v, e, DOSVM_IPX_MALFORMED, true); return;
        }
        at += f->length;
    }
    if (memcmp(e->immediate, p + 24, 6) || (memcmp(p + 20, "\0\0\0\0", 4) && memcmp(p+20,v->network,4)) ||
        ((p[24] & 1) && memcmp(p + 24, "\xff\xff\xff\xff\xff\xff", 6))) {
        complete(v, e, DOSVM_IPX_UNDELIVERABLE, true); return;
    }
    memcpy(p, e->immediate, 6); memcpy(p + 6, v->mac, 6); p[12] = 0x81; p[13] = 0x37;
    p[14] = p[15] = 0xff; p[16] = e->total >> 8; p[17] = (uint8_t)e->total; p[18] = 0;
    memcpy(p + 32, v->network, 4); memcpy(p + 36, v->mac, 6); p[42] = e->socket >> 8; p[43] = (uint8_t)e->socket;
    /* The API fills the caller's first IPX header as well as its wire copy. */
    if (v->ops.write(v->ops.arg, e->fragment[0].linear, p + 14, 30)) {
        v->memory_errors++; complete(v, e, DOSVM_IPX_MALFORMED, true); return;
    }
    int error = v->ops.send(v->ops.arg, p, at);
    complete(v, e, !error ? DOSVM_IPX_SUCCESS : error == -19 ? DOSVM_IPX_UNDELIVERABLE : DOSVM_IPX_HARDWARE, true);
}
void dosvm_ipx_service(struct dosvm_ipx *v, uint64_t ticks)
{
    if (!v || !v->enabled || v->servicing) return;
    v->servicing = true; if (ticks > v->ticks) v->ticks = ticks;
    for (unsigned i = 0; i < DOSVM_IPX_ECBS; i++) {
        struct dosvm_ipx_ecb *e = &v->ecb[i];
        if (e->state == ECB_SEND) send_packet(v, e);
        else if (e->state == ECB_TIMER && v->ticks >= e->due) complete(v, e, DOSVM_IPX_SUCCESS, true);
    }
    v->servicing = false;
}
int dosvm_ipx_receive(struct dosvm_ipx *v, const uint8_t *p, unsigned n)
{
    if (!v || !v->enabled || !p) return -EINVAL;
    unsigned off=ipx_decode(p,n,NULL);
    if(!off || (memcmp(p,v->mac,6) && !ipx_all(p)))return -EINVAL;
    uint8_t canonical[1514];
    if(off!=14) { int bytes=ipx_encode(canonical,p,n,IPX_II); if(bytes<0)return bytes; p=canonical; n=(unsigned)bytes; }
    unsigned length=be16(p+16);
    if(!memcmp(v->network,"\0\0\0\0",4) && memcmp(p+32,"\0\0\0\0",4))memcpy(v->network,p+32,4);
    if(memcmp(p+20,"\0\0\0\0",4) && memcmp(p+20,v->network,4))return -EINVAL;
    uint16_t socket = be16(p + 30); struct dosvm_ipx_ecb *e = 0;
    for (unsigned i = 0; i < DOSVM_IPX_ECBS; i++) if (v->ecb[i].state == ECB_LISTEN && v->ecb[i].socket == socket &&
        (!e || v->ecb[i].order < e->order)) e = &v->ecb[i];
    if (!e) { v->rx_dropped++; return 0; }
    if (length > e->total) { complete(v, e, DOSVM_IPX_MALFORMED, true); return -ENOSPC; }
    unsigned at = 14, left = length;
    for (unsigned i = 0; left && i < e->fragments; i++) {
        unsigned bytes = e->fragment[i].length < left ? e->fragment[i].length : left;
        if (bytes && v->ops.write(v->ops.arg, e->fragment[i].linear, p + at, bytes)) {
            v->memory_errors++; complete(v, e, DOSVM_IPX_MALFORMED, true); return -EFAULT;
        }
        left -= bytes; at += bytes;
    }
    if (v->ops.write(v->ops.arg, e->linear + 28, p + 6, 6)) {
        v->memory_errors++; complete(v, e, DOSVM_IPX_MALFORMED, true); return -EFAULT;
    }
    complete(v, e, DOSVM_IPX_SUCCESS, true); return 1;
}
int dosvm_ipx_dispatch(struct dosvm_ipx *v)
{
    if (!v || !v->enabled) return 0;
    struct dosvm_ipx_ecb *e = 0;
    for (unsigned i = 0; i < DOSVM_IPX_ECBS; i++) if (v->ecb[i].state == ECB_CALLBACK &&
        (!e || v->ecb[i].order < e->order)) e = &v->ecb[i];
    if (!e) return 0;
    struct dosvm_ipx_upcall call = {e->esr_cs, e->esr_ip, e->es, e->si, e->event_al, v->generation};
    int result = v->ops.upcall(v->ops.arg, &call);
    if (result) { if (result < 0) v->callbacks_dropped++; release(v, e); }
    return result;
}
void dosvm_ipx_process_exit(struct dosvm_ipx *v, uint16_t psp)
{
    if (!v || !v->enabled) return;
    for (unsigned i = 0; i < DOSVM_IPX_SOCKETS; i++)
        if (v->socket[i].open && v->socket[i].lifetime == 0 && v->socket[i].owner == psp)
            close_socket(v, v->socket[i].number);
}
void dosvm_ipx_disconnect(struct dosvm_ipx *v)
{
    if (!v || !v->enabled) return;
    for (unsigned i = 0; i < DOSVM_IPX_ECBS; i++) if (v->ecb[i].state)
        complete(v, &v->ecb[i], DOSVM_IPX_UNDELIVERABLE, false);
}
void dosvm_ipx_destroy(struct dosvm_ipx *v)
{
    if (!v || !v->enabled) return;
    for (unsigned i = 0; i < DOSVM_IPX_ECBS; i++) if (v->ecb[i].pin)
        complete(v, &v->ecb[i], DOSVM_IPX_CANCELLED, false);
    memset(v->socket, 0, sizeof(v->socket)); v->enabled = false;
    if (!++v->generation) ++v->generation;
}
