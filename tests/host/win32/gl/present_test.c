/* Completion identity and parser test; production protocol and callback.
 * SPDX-License-Identifier: MIT */
#include "../../../../apps/desktop/desktop.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct ciuki_message sent;
static unsigned count;
void desk_queue(struct desktop *d, int peer,
                const struct ciuki_message *message) {
  (void)d;
  assert(peer == 2);
  sent = *message;
  count++;
}
extern void desk_v3_present_complete(struct desktop *);
int main(void) {
  static struct desktop desktop;
  struct ciuki_surface_info surface = {24, 32, 32, 128, 1, 4096};
  struct ciuki_message request;
  struct desk_packet packet;
  desk_v3_message(&request, DESK_V3_PRESENT, 16, 123, 77, 9);
  desk_put32(request.data + 24, 32);
  desk_put32(request.data + 28, 32);
  assert(desk_parse(&request, 1, &surface, &packet));
  assert(packet.request == 123 && packet.opcode == DESK_V3_PRESENT);
  request.fd_count = 1;
  request.fds[0] = 4;
  assert(!desk_parse(&request, 1, &surface, &packet));
  request.fd_count = 0;
  request.fds[0] = 0;
  desk_put32(request.data + 24, 33);
  assert(!desk_parse(&request, 1, &surface, &packet));
  struct desk_client *c = &desktop.clients[5];
  c->window = 1;
  c->v3_peer = 3;
  c->v3_id = 77;
  c->v3_generation = 9;
  c->gl_present_request = 123;
  assert(!count);
  desk_v3_present_complete(&desktop);
  assert(count == 1 && !c->gl_present_request);
  assert(sent.data[1] == DESK_V3_RESULT && desk_get32(sent.data + 4) == 123 &&
         desk_get32(sent.data + 8) == 77 && desk_get32(sent.data + 12) == 9 &&
         desk_get32(sent.data + 16) == DESK_V3_PRESENT &&
         !desk_get32(sent.data + 20));
  desk_v3_present_complete(&desktop);
  assert(count == 1);
  puts("PASS v3 present bounds, attachment refusal, exact completion identity "
       "and no duplicate completion");
  return 0;
}
