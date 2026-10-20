/* f5-10C2 w32-dialogs fixture: the environment the guest entry and the host
 * harness supply. pump drains the thread queue through IsDialogMessage;
 * settle additionally waits for placement and painting (guest desktop).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_W32_DIALOGS_EXERCISE_H
#define CIUKI_W32_DIALOGS_EXERCISE_H
#include <w32/user32.h>
#include "controller.h"
struct w32_dialogs_env {
  HINSTANCE instance;
  void (*pump)(HWND dialog, int settle);
};
void w32_dialogs_run(const struct w32_dialogs_env *, struct w32_dialogs_result *);
#endif
