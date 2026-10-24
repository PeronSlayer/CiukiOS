/* SPDX-License-Identifier: MIT
 * XMODEM state machine. */
#include "xmodem.h"
#include <stdio.h>
#include <string.h>

uint16_t xmodem_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}
static uint8_t checksum8(const uint8_t *p, size_t n) { unsigned s = 0; while (n--) s += *p++; return (uint8_t)s; }
static void fail(struct xmodem *x, const char *why) { x->state = XM_FAILED; snprintf(x->message, sizeof(x->message), "%s", why); }
static size_t emit(uint8_t *out, size_t cap, size_t at, uint8_t byte) { if (at < cap) out[at++] = byte; return at; }

void xmodem_send_init(struct xmodem *x, const struct xmodem_io *io, int one_k)
{
    memset(x, 0, sizeof(*x)); x->io = *io; x->sending = 1; x->one_k = one_k; x->number = 1; x->state = XM_START;
    snprintf(x->message, sizeof(x->message), "Waiting for the receiver");
}
void xmodem_receive_init(struct xmodem *x, const struct xmodem_io *io, int crc)
{
    memset(x, 0, sizeof(*x)); x->io = *io; x->crc = crc; x->number = 1; x->state = XM_START;
    snprintf(x->message, sizeof(x->message), "Waiting for the sender");
}
int xmodem_finished(const struct xmodem *x) { return x->state == XM_DONE || x->state == XM_FAILED || x->state == XM_CANCELLED; }

/* ---- sender ---- */
static size_t build_block(struct xmodem *x, uint8_t *out, size_t cap)
{
    uint8_t data[1024]; int want = x->one_k ? 1024 : 128;
    int n = x->io.read(x->io.ctx, data, (size_t)want);
    if (n < 0) { fail(x, "The file could not be read"); return 0; }
    if (n == 0) {                                   /* end of file: EOT */
        x->eof = 1; x->state = XM_END; x->retries = 0; x->quiet_ms = 0;
        x->last[0] = XMODEM_EOT; x->last_length = 1;
        snprintf(x->message, sizeof(x->message), "Finishing");
        return emit(out, cap, 0, XMODEM_EOT);
    }
    int big = n > 128;
    size_t size = big ? 1024 : 128;
    memset(data + n, XMODEM_PAD, size - (size_t)n);
    size_t at = 0;
    x->last[at++] = big ? XMODEM_STX : XMODEM_SOH;
    x->last[at++] = (uint8_t)x->number; x->last[at++] = (uint8_t)~x->number;
    memcpy(x->last + at, data, size); at += size;
    if (x->crc) { uint16_t c = xmodem_crc16(data, size); x->last[at++] = (uint8_t)(c >> 8); x->last[at++] = (uint8_t)c; }
    else x->last[at++] = checksum8(data, size);
    x->last_length = at;
    x->bytes += (uint64_t)n;
    size_t sent = at > cap ? cap : at;
    memcpy(out, x->last, sent);
    return sent;
}
static size_t repeat(struct xmodem *x, uint8_t *out, size_t cap)
{
    if (++x->retries > XMODEM_RETRIES) { fail(x, "Too many errors: the transfer was abandoned"); return 0; }
    x->quiet_ms = 0;
    size_t n = x->last_length > cap ? cap : x->last_length;
    memcpy(out, x->last, n);
    return n;
}
static size_t sender_byte(struct xmodem *x, uint8_t c, uint8_t *out, size_t cap)
{
    if (c == XMODEM_CAN) { if (++x->cans >= 2) { x->cans = 0; x->state = XM_CANCELLED; snprintf(x->message, sizeof(x->message), "Cancelled by the receiver"); } return 0; }
    x->cans = 0;
    x->quiet_ms = 0;
    if (x->state == XM_START) {
        if (c != 'C' && c != XMODEM_NAK) return 0;
        x->crc = c == 'C'; x->state = XM_RUN; x->retries = 0;
        snprintf(x->message, sizeof(x->message), "Sending (%s)", x->crc ? (x->one_k ? "1K CRC" : "CRC") : "checksum");
        return build_block(x, out, cap);
    }
    if (c == XMODEM_ACK) {
        x->retries = 0;
        if (x->state == XM_END) { x->state = XM_DONE; snprintf(x->message, sizeof(x->message), "Sent %u blocks", x->blocks); return 0; }
        x->number = (x->number + 1) & 255; x->blocks++;
        return build_block(x, out, cap);
    }
    if (c == XMODEM_NAK) return repeat(x, out, cap);
    return 0;
}
/* ---- receiver ---- */
static void flush_held(struct xmodem *x, int final)
{
    if (!x->holding) return;
    size_t n = x->held_length;
    if (final) while (n && x->held[n - 1] == XMODEM_PAD) n--;
    if (n && x->io.write(x->io.ctx, x->held, n)) fail(x, "The file could not be written");
    x->bytes += n; x->holding = 0;
}
static size_t receiver_frame(struct xmodem *x, uint8_t *out, size_t cap)
{
    size_t data = x->frame[0] == XMODEM_STX ? 1024 : 128, at = 0;
    const uint8_t *p = x->frame + 3;
    int good = (uint8_t)(x->frame[1] + x->frame[2]) == 0xff;
    if (good) {
        if (x->crc) { uint16_t want = xmodem_crc16(p, data); good = p[data] == (uint8_t)(want >> 8) && p[data + 1] == (uint8_t)want; }
        else good = p[data] == checksum8(p, data);
    }
    if (!good) { if (++x->retries > XMODEM_RETRIES) { fail(x, "Too many errors: the transfer was abandoned"); return 0; } return emit(out, cap, 0, XMODEM_NAK); }
    unsigned number = x->frame[1];
    if (number == ((x->number - 1) & 255) && x->blocks) { x->retries = 0; return emit(out, cap, 0, XMODEM_ACK); }   /* a repeat of the last block: its ACK was lost */
    if (number != x->number) {                                 /* out of sequence: cancel */
        at = emit(out, cap, at, XMODEM_CAN); at = emit(out, cap, at, XMODEM_CAN);
        fail(x, "Blocks arrived out of order"); return at;
    }
    flush_held(x, 0);
    if (x->state == XM_FAILED) return emit(out, cap, 0, XMODEM_CAN);
    memcpy(x->held, p, data); x->held_length = data; x->holding = 1;
    x->number = (x->number + 1) & 255; x->blocks++; x->retries = 0;
    snprintf(x->message, sizeof(x->message), "Receiving block %u", x->blocks);
    return emit(out, cap, 0, XMODEM_ACK);
}
static size_t receiver_byte(struct xmodem *x, uint8_t c, uint8_t *out, size_t cap)
{
    x->quiet_ms = 0;
    if (x->frame_length == 0) {
        if (c == XMODEM_CAN) { if (++x->cans >= 2) { x->cans = 0; x->state = XM_CANCELLED; snprintf(x->message, sizeof(x->message), "Cancelled by the sender"); } return 0; }
        x->cans = 0;
        if (c == XMODEM_EOT) {
            flush_held(x, 1);
            if (x->state != XM_FAILED) { x->state = XM_DONE; snprintf(x->message, sizeof(x->message), "Received %u blocks, %llu bytes", x->blocks, (unsigned long long)x->bytes); }
            return emit(out, cap, 0, XMODEM_ACK);
        }
        if (c != XMODEM_SOH && c != XMODEM_STX) return 0;      /* line noise between frames */
        x->state = XM_RUN; x->frame[x->frame_length++] = c;
        return 0;
    }
    x->frame[x->frame_length++] = c;
    size_t total = 3 + (x->frame[0] == XMODEM_STX ? 1024 : 128) + (x->crc ? 2 : 1);
    if (x->frame_length < total) return 0;
    x->frame_length = 0;
    return receiver_frame(x, out, cap);
}
static void queue(struct xmodem *x, const uint8_t *bytes, size_t n)
{
    if (n > sizeof(x->pending) - x->pending_length) n = sizeof(x->pending) - x->pending_length;
    memcpy(x->pending + x->pending_length, bytes, n); x->pending_length += n;
}
size_t xmodem_input(struct xmodem *x, const uint8_t *in, size_t length)
{
    uint8_t out[XMODEM_OUT]; size_t used = 0;
    if (x->state == XM_DONE && !x->sending) {     /* a receiver that finished still answers a repeated EOT (its ACK may have been lost) */
        for (size_t i = 0; i < length; i++) if (in[i] == XMODEM_EOT && x->pending_length + 1 <= sizeof(x->pending)) x->pending[x->pending_length++] = XMODEM_ACK;
        return length;
    }
    for (; used < length && !xmodem_finished(x); used++) {
        if (x->pending_length + XMODEM_OUT > sizeof(x->pending)) break;
        size_t n = x->sending ? sender_byte(x, in[used], out, sizeof(out)) : receiver_byte(x, in[used], out, sizeof(out));
        queue(x, out, n);
    }
    return xmodem_finished(x) ? length : used;       /* a finished transfer ignores the rest */
}
void xmodem_tick(struct xmodem *x, unsigned elapsed_ms)
{
    uint8_t out[XMODEM_OUT]; size_t n = 0;
    if (xmodem_finished(x) || x->pending_length + XMODEM_OUT > sizeof(x->pending)) return;
    x->quiet_ms += elapsed_ms;
    if (x->sending) {
        if (x->state == XM_START) { if (x->quiet_ms >= XMODEM_SENDER_WAIT_MS) fail(x, "The receiver did not answer"); return; }
        if (x->quiet_ms >= XMODEM_WAIT_MS) n = repeat(x, out, sizeof(out));
    } else if (x->state == XM_START && x->frame_length == 0) {
        if (x->quiet_ms < XMODEM_START_MS) return;
        x->quiet_ms = 0;
        if (++x->start_attempts > 3 * XMODEM_RETRIES) { fail(x, "The sender did not answer"); return; }
        if (x->crc && x->start_attempts > 3) x->crc = 0;       /* an old sender: fall back to the checksum, as HyperTerminal does */
        n = emit(out, sizeof(out), 0, x->crc ? 'C' : XMODEM_NAK);
    } else if (x->quiet_ms >= XMODEM_RECEIVER_WAIT_MS) {
        x->quiet_ms = 0; x->frame_length = 0;
        if (++x->retries > XMODEM_RETRIES) { fail(x, "Too many errors: the transfer was abandoned"); return; }
        n = emit(out, sizeof(out), 0, XMODEM_NAK);
    }
    queue(x, out, n);
}
void xmodem_cancel(struct xmodem *x)
{
    static const uint8_t cancel[2] = {XMODEM_CAN, XMODEM_CAN};
    if (xmodem_finished(x)) return;
    x->state = XM_CANCELLED; snprintf(x->message, sizeof(x->message), "Cancelled");
    x->pending_length = 0; queue(x, cancel, 2);
}
size_t xmodem_take(struct xmodem *x, uint8_t *out, size_t cap)
{
    size_t n = x->pending_length < cap ? x->pending_length : cap;
    memcpy(out, x->pending, n);
    memmove(x->pending, x->pending + n, x->pending_length - n); x->pending_length -= n;
    return n;
}
