/* SPDX-License-Identifier: MIT
 * Framing of the headless hardware report (hwreport --rescue --text-fd=N),
 * shared by the sender (apps/hwreport) and the receiver (Setup's rescue page).
 * Header only, no dependencies beyond <stdint.h>/<string.h>.
 *
 * Every frame is the data of one channel message (at most 256 bytes):
 *   u32 magic 'CHRL' (bytes 43 48 52 4C), u16 version (1), u16 type, then
 *   BEGIN (1): u32 total length of the text that follows
 *   DATA  (2): u32 offset (must equal the bytes received so far), 1..244 text bytes
 *   END   (3): i32 status (0 complete; negative errno: partial or failed),
 *              u32 total length, u32 CRC-32 of all text bytes
 * All integers are little-endian. A transfer is BEGIN, DATA*, END, and
 * nothing after END. Total length is bounded by HR_LINK_MAX. */
#ifndef CIUKI_HWREPORT_TEXTFRAME_H
#define CIUKI_HWREPORT_TEXTFRAME_H
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HR_FRAME_MAGIC 0x4C524843u
#define HR_FRAME_VERSION 1u
#define HR_FRAME_BEGIN 1u
#define HR_FRAME_DATA 2u
#define HR_FRAME_END 3u
#define HR_FRAME_MAX 256u                         /* CIUKI_MESSAGE_PAYLOAD_MAX */
#define HR_FRAME_DATA_HEADER 12u
#define HR_FRAME_CHUNK (HR_FRAME_MAX - HR_FRAME_DATA_HEADER)
/* Hard bound on the transferred text: Setup's text viewer holds 256 KiB
 * (ST_TEXT_BYTES); the transfer stays 16 KiB below it. A larger report is
 * cut at a line boundary and sent with status -EFBIG. */
#define HR_LINK_MAX (240u * 1024u)

static inline void hr_put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void hr_put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline uint32_t hr_get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t hr_get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static inline uint32_t hr_crc32(uint32_t crc, const uint8_t *data, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (unsigned k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline size_t hr_frame_prefix(uint8_t *out, unsigned type)
{ hr_put32(out, HR_FRAME_MAGIC); hr_put16(out + 4, HR_FRAME_VERSION); hr_put16(out + 6, type); return 8; }
static inline size_t hr_frame_begin(uint8_t *out, uint32_t total)
{ size_t n = hr_frame_prefix(out, HR_FRAME_BEGIN); hr_put32(out + n, total); return n + 4; }
/* 1 <= bytes <= HR_FRAME_CHUNK */
static inline size_t hr_frame_data(uint8_t *out, uint32_t offset, const void *text, size_t bytes)
{ size_t n = hr_frame_prefix(out, HR_FRAME_DATA); hr_put32(out + n, offset); memcpy(out + n + 4, text, bytes); return n + 4 + bytes; }
static inline size_t hr_frame_end(uint8_t *out, int32_t status, uint32_t total, uint32_t crc)
{ size_t n = hr_frame_prefix(out, HR_FRAME_END); hr_put32(out + n, (uint32_t)status); hr_put32(out + n + 4, total); hr_put32(out + n + 8, crc); return n + 12; }

/* Receiver state machine. The caller supplies the destination buffer. */
struct hr_frame_rx {
    char *buffer; size_t capacity;      /* capacity is clamped to HR_LINK_MAX */
    int state;                          /* 0 expect BEGIN, 1 expect DATA/END, 2 complete */
    uint32_t total, received, crc;
    int32_t status;                     /* from END */
};
static inline void hr_frame_rx_init(struct hr_frame_rx *rx, char *buffer, size_t capacity)
{
    memset(rx, 0, sizeof(*rx));
    rx->buffer = buffer; rx->capacity = capacity > HR_LINK_MAX ? HR_LINK_MAX : capacity;
}
/* Returns 0 (more frames expected), 1 (transfer complete), or a negative
 * errno: -EPROTO bad magic/version/type/order/length/offset, -EMSGSIZE a
 * declared or actual size over the bound, -EBADMSG checksum mismatch. */
static inline int hr_frame_rx_feed(struct hr_frame_rx *rx, const uint8_t *data, size_t n)
{
    if (rx->state == 2 || n < 8 || n > HR_FRAME_MAX || hr_get32(data) != HR_FRAME_MAGIC ||
        hr_get16(data + 4) != HR_FRAME_VERSION) return -EPROTO;
    unsigned type = hr_get16(data + 6);
    if (type == HR_FRAME_BEGIN) {
        if (rx->state != 0 || n != 12) return -EPROTO;
        rx->total = hr_get32(data + 8);
        if (rx->total > rx->capacity) return -EMSGSIZE;
        rx->state = 1; return 0;
    }
    if (rx->state != 1) return -EPROTO;
    if (type == HR_FRAME_DATA) {
        if (n <= HR_FRAME_DATA_HEADER || hr_get32(data + 8) != rx->received) return -EPROTO;
        size_t bytes = n - HR_FRAME_DATA_HEADER;
        if (bytes > rx->total - rx->received) return -EMSGSIZE;
        memcpy(rx->buffer + rx->received, data + HR_FRAME_DATA_HEADER, bytes);
        rx->received += (uint32_t)bytes; return 0;
    }
    if (type == HR_FRAME_END) {
        if (n != 20 || hr_get32(data + 12) != rx->total || rx->received != rx->total) return -EPROTO;
        if (hr_get32(data + 16) != hr_crc32(0, (const uint8_t *)rx->buffer, rx->received)) return -EBADMSG;
        rx->status = (int32_t)hr_get32(data + 8); rx->state = 2; return 1;
    }
    return -EPROTO;
}
#endif
