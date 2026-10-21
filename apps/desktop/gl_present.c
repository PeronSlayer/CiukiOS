/* Deferred v3 software-GL present completion. No graphics API or renderer in
 * the compositor: only request identity and completed-frame acknowledgement.
 * SPDX-License-Identifier: MIT */
#include "desktop.h"
void desk_v3_present_complete(struct desktop *d) {
  for (int i = 0; i < DESK_CLIENTS; i++) {
    struct desk_client *c = &d->clients[i];
    if (!c->gl_present_request || !c->window || !c->v3_peer)
      continue;
    struct ciuki_message reply;
    desk_v3_message(&reply, DESK_V3_RESULT, 8, c->gl_present_request, c->v3_id,
                    c->v3_generation);
    desk_put32(reply.data + 16, DESK_V3_PRESENT);
    desk_put32(reply.data + 20, 0);
    desk_queue(d, (int)c->v3_peer - 1, &reply);
    c->gl_present_request = 0;
  }
}
