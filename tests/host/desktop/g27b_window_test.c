/* SPDX-License-Identifier: MIT
 * Focused G27B rows; shares the production desktop oracle without linking
 * the unrelated disk UI scenarios of the complete window-model suite. */
#define WM_NO_MAIN
#include "window_model_test.c"
#include "../../../sdk/sysroot-overlay/include/ciuki/devices.h"
int ciuki_devices(struct ciuki_devices_info *p) { (void)p;errno=ENOSYS;return -1; }
int main(void)
{
    (void)wm_fuzz;(void)wm_rows;(void)wm_dump;(void)report;
    (void)command_redraw;(void)covered_scenario;(void)restack_scenario;(void)internal_scenario;(void)fuzz;(void)wm_shell;
    signal(SIGABRT,report);
    setvbuf(stdout,NULL,_IONBF,0);
    for(unsigned m=0;m<WM_MODES;m++) {
        row_g27b_statistics(m);row_g27b_viewer_save(m);row_g27b_terminal(m);row_g27b_serial(m);
        printf("PASS G27B window rows: %s\n",wm_names[m]);
    }
    printf("PASS G27B fx-15: %u frames equal full redraw\n",wm_frames);
}
