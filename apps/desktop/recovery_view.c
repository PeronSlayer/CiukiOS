/* SPDX-License-Identifier: MIT
 * G14 desktop drawing and state of the startup recovery that needs no
 * system call (recovery.c has the rest): Safe mode's corner labels and the
 * Terminal's WIN at a Command prompt only start. */
#include "desktop.h"
#include "shell.h"
#include <errno.h>

/* The Terminal's WIN: the normal desktop from here on (icons, no relaunch). */
int desk_recovery_win(struct desktop *d)
{
    if (!d->terminal_shell) return -EALREADY;
    d->terminal_shell = 0;
    d->sh.background_valid = 0;
    desk_damage_add(&d->damage, (struct desk_box){0, 0, (int)d->width, (int)d->height}, d->width, d->height);
    return 0;
}
/* Windows 98 marks Safe mode in the four corners of the desktop. */
void desk_recovery_paint(struct desktop *d, struct desk_box clip)
{
    if (!d->safe_mode) return;
    const char *label = "Safe mode";
    int w = desk_text_measure(desk_theme->font_large, label), h = 24, m = 8;
    int top = DESK_BAR + m, bottom = (int)d->height - DESK_BAR - m - h;
    int xs[2] = { m, (int)d->width - m - w }, ys[2] = { top, bottom };
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) {
        struct desk_box box = { xs[a], ys[b], w, h }, cut;
        if (!desk_intersect(box, clip, &cut)) continue;
        desk_text(d, clip, xs[a], ys[b], label, 2, desk_theme->desktop_label);
    }
}
