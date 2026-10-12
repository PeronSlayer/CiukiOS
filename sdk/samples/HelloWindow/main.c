/* SPDX-License-Identifier: MIT */
#include <deskui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void clicked(struct du_ui *ui, const struct du_event *event, void *context)
{
    if (event->type == DU_EV_ACTIVATE && event->id == 2) {
        du_set_text(du_get(ui, 1), "Classic computing, one woof at a time.");
        ui->dirty = 1;
    }
    (void)context;
}
int main(int argc, char **argv)
{
    int channel = 3;
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "--channel-fd=", 13)) {
            char *end; long fd = strtol(argv[i] + 13, &end, 10);
            if (!argv[i][13] || *end || fd < 3 || fd >= 128) return 2;
            channel = (int)fd;
        }
    }
    struct du_ui *ui = calloc(1, sizeof(*ui));
    struct du_window *window = calloc(1, sizeof(*window));
    if (!ui || !window) { free(ui); free(window); return 1; }
    if (du_window_open(window, ui, channel, 360, 150, "Hello Ciuki")) {
        fprintf(stderr, "Hello window: desktop connection failed\n");
        free(ui); free(window); return 1;
    }
    ui->event = clicked;
    du_add(ui, DU_LABEL, 1, (struct du_rect){16, 20, 328, 40}, "Hello from CiukiOS!");
    du_add(ui, DU_BUTTON, 2, (struct du_rect){16, 80, 120, 28}, "Woof!");
    int result = du_run(window, NULL, NULL);
    du_window_destroy(window); free(ui); free(window);
    return result < 0 ? 1 : 0;
}
