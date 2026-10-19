/* Decode the production v3 wire; this file isolates native ABI constants.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../../../apps/desktop/protocol.h"
#include <w32/ddraw.h>
int host_replay(const uint8_t *bytes, DWORD n, struct dd_lease *out) {
  if (n > 256)
    return 0;
  struct ciuki_message message = {0};
  message.length = n;
  w32_copy(message.data, bytes, n);
  struct desk_packet packet;
  if (!desk_parse(&message, 0, 0, &packet) ||
      packet.opcode != DESK_V3_LEASE_EVENT)
    return 0;
  out->active = packet.flags == 1;
  out->token = packet.lease.token;
  out->generation = packet.lease.generation;
  out->width = packet.width;
  out->height = packet.height;
  out->bpp = packet.bpp;
  out->pitch = packet.lease.surface.pitch;
  return 1;
}
