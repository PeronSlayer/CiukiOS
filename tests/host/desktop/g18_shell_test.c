/* SPDX-License-Identifier: MIT
 * Reuse the production compositor's five-presenter oracle and input fixture. */
#define WM_NO_MAIN 1
#include "window_model_test.c"
#include "g18_shortcut_test.h"
int main(void)
{
    (void)wm_rows; (void)wm_fuzz; (void)report;
    (void)command_redraw; (void)covered_scenario; (void)restack_scenario; (void)internal_scenario; (void)fuzz;
    for (unsigned m=0;m<WM_MODES;m++) { row_g18_shortcuts(m); row_g18_tray(m); }
    puts("PASS G18 shell: nested installed folders, duplicate folder names, shortcut fields, hotkey capture, show states, icon edits, target repair and repaint on five presenters");
    return 0;
}
