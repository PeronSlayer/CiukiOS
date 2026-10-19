/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <w32/input_bridge.h>
static struct {
  W32_INPUT_SINK callback;
  void *context;
} sinks[32];
static struct w32_input_snapshot state;
void W32API CiukiUserInputLock(void) { u32_lock(); }
void W32API CiukiUserInputUnlock(void) {
  int ignored = 0;
  u32_unlock(&ignored);
}
void W32API CiukiUserInputPump(void) {
  U32_GUARD;
  u32_pump();
}
BOOL W32API CiukiUserInputSubscribe(W32_INPUT_SINK callback, void *context,
                                    BOOL add) {
  U32_GUARD;
  if (!callback)
    return 0;
  for (UINT i = 0; i < 32; i++)
    if (sinks[i].callback == callback && sinks[i].context == context) {
      if (!add) {
        sinks[i].callback = 0;
        sinks[i].context = 0;
      }
      return 1;
    }
  if (!add)
    return 1;
  for (UINT i = 0; i < 32; i++)
    if (!sinks[i].callback) {
      sinks[i].context = context;
      sinks[i].callback = callback;
      return 1;
    }
  return 0;
}
void W32API CiukiUserInputSnapshot(struct w32_input_snapshot *out) {
  U32_GUARD;
  if (!out)
    return;
  *out = state;
  out->foreground = u32_foreground;
  out->cursor = u32_pointer;
}
void u32_input_notify(HWND hwnd, const struct ciuki_input_event *e, UINT kind) {
  if (e && !kind) {
    if (e->type == CIUKI_INPUT_KEY) {
      UINT dik = u32_dik(e->code);
      if (dik)
        state.keys[dik] = e->value ? 128 : 0;
    } else if (e->type == CIUKI_INPUT_BUTTON && e->code >= 1 && e->code <= 8)
      state.buttons[e->code - 1] = e->value ? 128 : 0;
  }
  for (UINT i = 0; i < 32; i++)
    if (sinks[i].callback)
      sinks[i].callback(sinks[i].context, hwnd, e, kind);
}
void u32_input_reset(void) {
  w32_fill(&state, 0, sizeof(state));
  u32_input_notify(u32_foreground, 0, 2);
}
void u32_input_focus(void) { u32_input_notify(u32_foreground, 0, 3); }
