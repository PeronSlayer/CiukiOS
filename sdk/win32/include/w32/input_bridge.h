/* Private builtin-to-USER interface. No kernel or desktop wire ABI change.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_W32_INPUT_BRIDGE_H
#define CIUKI_W32_INPUT_BRIDGE_H
#include <w32/user32.h>
/* 0 ordinary input, 1 raw relative input, 2 reset, 3 activation. */
typedef void(W32API *W32_INPUT_SINK)(void *, HWND,
                                     const struct ciuki_input_event *, UINT);
struct w32_input_snapshot {
  uint8_t keys[256], buttons[8];
  HWND foreground;
  POINT cursor;
};
BOOL W32API CiukiUserInputSubscribe(W32_INPUT_SINK, void *, BOOL);
void W32API CiukiUserInputSnapshot(struct w32_input_snapshot *);
void W32API CiukiUserInputPump(void);
void W32API CiukiUserInputLock(void);
void W32API CiukiUserInputUnlock(void);
/* flags: 1 relative, 2 exclusive; zero releases. Ordinary capture/clip
 * retained. */
BOOL W32API CiukiUserInputPointer(HWND, UINT);
#endif
