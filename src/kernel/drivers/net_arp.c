/* A bounded wire-format probe helper, not a replacement for lwIP.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/net.h>
unsigned net_arp_request(uint8_t frame[60], const uint8_t mac[6])
{
    memset(frame, 0, 60); memset(frame, 0xff, 6); memcpy(frame + 6, mac, 6);
    frame[12] = 8; frame[13] = 6; frame[15] = 1; frame[16] = 8;
    frame[18] = 6; frame[19] = 4; frame[21] = 1;
    memcpy(frame + 22, mac, 6);
    const uint8_t source[4] = {10,0,2,15}, target[4] = {10,0,2,2};
    memcpy(frame + 28, source, 4); memcpy(frame + 38, target, 4); return 60;
}
bool net_arp_reply(const uint8_t *frame, unsigned length, const uint8_t local[6], uint8_t peer[6])
{
    const uint8_t header[10] = {8,6,0,1,8,0,6,4,0,2};
    const uint8_t sender[4] = {10,0,2,2}, target[4] = {10,0,2,15};
    if (!frame || !local || !peer || length < 42 || length > NET_FRAME_MAX ||
        memcmp(frame, local, 6) || memcmp(frame + 12, header, 10) ||
        memcmp(frame + 6, frame + 22, 6) || (frame[22] & 1) ||
        memcmp(frame + 28, sender, 4) || memcmp(frame + 32, local, 6) ||
        memcmp(frame + 38, target, 4)) return false;
    unsigned bits = 0; for (unsigned i = 0; i < 6; i++) bits |= frame[22 + i];
    if (!bits) return false;
    memcpy(peer, frame + 22, 6); return true;
}
