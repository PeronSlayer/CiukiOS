/* Small helpers shared by the System Information, System Monitor and Resource Meter windows.
 * Header-only and free of syscalls, so the host tests render the same code.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_SYSUI_H
#define CIUKI_SYSUI_H
#include "../libdeskui/deskui.h"
#include <stdio.h>
#include <string.h>

/* Set a widget's text and repaint it only when the text changed. */
static inline void sysui_text(struct du_ui *u, int id, const char *text)
{
    struct du_widget *w = du_get(u, id);
    if (w && strcmp(w->text, text)) { du_set_text(w, text); du_invalidate_widget(u, id); }
}
/* Information, warning or error box with an OK button. */
static inline void sysui_box(struct du_ui *u, int id, const char *title, const char *text, int icon)
{ du_message(u, id, title, text, icon, NULL, 0, 0, 0); }
/* Word-wraps `text` into lines of at most `cols` characters separated by '\n' (the sheet text draws no wrapping). */
static inline void sysui_wrap(char *out, size_t cap, const char *text, unsigned cols)
{
    size_t at = 0; unsigned col = 0;
    if (!cap) return;
    while (*text && at + 2 < cap) {
        const char *end = text; while (*end && *end != ' ') end++;
        unsigned n = (unsigned)(end - text);
        if (col && col + 1 + n > cols) { out[at++] = '\n'; col = 0; }
        else if (col) { out[at++] = ' '; col++; }
        for (unsigned i = 0; i < n && at + 2 < cap; i++) { out[at++] = text[i]; col++; }
        text = end; while (*text == ' ') text++;
    }
    out[at] = 0;
}
#endif
