/* IPX encapsulation discriminators shared by bridge, DOS and native endpoints.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_IPX_WIRE_H
#define CIUKI_IPX_WIRE_H
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
enum ipx_framing { IPX_LLC, IPX_RAW, IPX_II, IPX_SNAP, IPX_FRAMINGS };
static inline unsigned ipx_frame_wire(unsigned setting)
{
    return setting==CIUKI_IPX_FRAME_RAW ? IPX_RAW : setting==CIUKI_IPX_FRAME_II ? IPX_II :
        setting==CIUKI_IPX_FRAME_SNAP ? IPX_SNAP : IPX_LLC;
}
#define IPX_FRAME_PEERS 32u
struct ipx_frame_cache { uint8_t node[IPX_FRAME_PEERS][6], framing[IPX_FRAME_PEERS]; unsigned count, next; };
static inline void ipx_frame_remember(struct ipx_frame_cache *c, const uint8_t node[6], unsigned f)
{
    if (node[0]&1) return;
    unsigned i=0; while (i<c->count && memcmp(c->node[i],node,6)) i++;
    if (i==c->count) {
        if (c->count<IPX_FRAME_PEERS) c->count++;
        else { i=c->next; c->next=(c->next+1)%IPX_FRAME_PEERS; }
        memcpy(c->node[i],node,6);
    }
    c->framing[i]=(uint8_t)f;
}
static inline unsigned ipx_frame_reply(const struct ipx_frame_cache *c, const uint8_t node[6], unsigned fallback)
{
    for (unsigned i=0;i<c->count;i++) if (!memcmp(c->node[i],node,6)) return c->framing[i];
    return fallback;
}
static inline unsigned ipx_be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static inline void ipx_put16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static inline bool ipx_all(const uint8_t *p)
{ return !memcmp(p, "\xff\xff\xff\xff\xff\xff", 6); }
/* Returns the IPX offset; padding is never part of the declared datagram. */
static inline unsigned ipx_decode(const uint8_t *p, unsigned n, unsigned *framing)
{
    if (!p || n < 44 || n > 1514) return 0;
    unsigned type = ipx_be16(p + 12), off = 14, end = n, f;
    if (type == 0x8137) f = IPX_II;
    else {
        if (type > 1500 || type < 30 || type > n - 14) return 0;
        end = 14 + type;
        if (p[14] == 255 && p[15] == 255) f = IPX_RAW;
        else if (type >= 33 && !memcmp(p + 14, "\xe0\xe0\x03", 3)) { f = IPX_LLC; off += 3; }
        else if (type >= 38 && !memcmp(p + 14, "\xaa\xaa\x03\0\0\0\x81\x37", 8)) { f = IPX_SNAP; off += 8; }
        else return 0;
    }
    if (off + 30 > end || p[off] != 255 || p[off + 1] != 255 ||
        ipx_be16(p + off + 2) < 30 || ipx_be16(p + off + 2) > end - off ||
        (type <= 1500 && ipx_be16(p + off + 2) != end - off) ||
        p[off + 4] > 15 || (p[6] & 1) ||
        !memcmp(p + 6, "\0\0\0\0\0\0", 6) ||
        memcmp(p, p + off + 10, 6) || memcmp(p + 6, p + off + 22, 6)) return 0;
    if (framing) *framing = f;
    return off;
}
static inline int ipx_encode(uint8_t out[1514], const uint8_t *p, unsigned n, unsigned f)
{
    unsigned off = ipx_decode(p, n, NULL); if (!off || f >= IPX_FRAMINGS) return -EINVAL;
    unsigned bytes = ipx_be16(p + off + 2), extra = f == IPX_LLC ? 3 : f == IPX_SNAP ? 8 : 0;
    if (bytes + extra > 1500) return -EMSGSIZE;
    memcpy(out, p, 12); ipx_put16(out + 12, f == IPX_II ? 0x8137 : bytes + extra);
    if (extra == 3) memcpy(out + 14, "\xe0\xe0\x03", 3);
    if (extra == 8) memcpy(out + 14, "\xaa\xaa\x03\0\0\0\x81\x37", 8);
    memcpy(out + 14 + extra, p + off, bytes);
    unsigned length = 14 + extra + bytes;
    if (length < 60) { memset(out + length, 0, 60 - length); length = 60; }
    return (int)length;
}
#endif
