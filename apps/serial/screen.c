/* SPDX-License-Identifier: MIT
 * Serial Terminal screen model. */
#include "screen.h"
#include <string.h>

enum { S_NORMAL, S_ESC, S_CSI };
void screen_clear(struct screen *s)
{
    for (int y = 0; y < SCREEN_ROWS; y++) for (int x = 0; x < SCREEN_COLS; x++) s->cells[y][x] = (struct screen_cell){' ', 0};
    s->row = s->col = 0; s->wrap_pending = 0;
}
void screen_init(struct screen *s, int ansi)
{
    memset(s, 0, sizeof(*s)); s->ansi = ansi; screen_clear(s);
}
static void scroll(struct screen *s)
{
    unsigned slot = (s->history_first + s->history_count) % SCREEN_SCROLLBACK;
    if (s->history_count == SCREEN_SCROLLBACK) { slot = s->history_first; s->history_first = (s->history_first + 1) % SCREEN_SCROLLBACK; }
    else s->history_count++;
    memcpy(s->history[slot], s->cells[0], sizeof(s->cells[0]));
    memmove(s->cells[0], s->cells[1], sizeof(s->cells[0]) * (SCREEN_ROWS - 1));
    for (int x = 0; x < SCREEN_COLS; x++) s->cells[SCREEN_ROWS - 1][x] = (struct screen_cell){' ', 0};
}
static void line_feed(struct screen *s)
{
    if (s->row == SCREEN_ROWS - 1) scroll(s); else s->row++;
}
static void put(struct screen *s, uint8_t c)
{
    if (s->wrap_pending) { s->col = 0; line_feed(s); s->wrap_pending = 0; }
    s->cells[s->row][s->col] = (struct screen_cell){c, (uint8_t)s->attr};
    if (s->col == SCREEN_COLS - 1) s->wrap_pending = 1; else s->col++;
}
static int param(const struct screen *s, int index, int fallback)
{
    return index < s->nparams && s->params[index] > 0 ? s->params[index] : fallback;
}
static void erase(struct screen *s, int row, int from, int to)
{
    for (int x = from; x <= to; x++) s->cells[row][x] = (struct screen_cell){' ', 0};
}
static void csi(struct screen *s, uint8_t final)
{
    s->wrap_pending = 0;
    switch (final) {
    case 'A': s->row -= param(s, 0, 1); if (s->row < 0) s->row = 0; break;
    case 'B': s->row += param(s, 0, 1); if (s->row >= SCREEN_ROWS) s->row = SCREEN_ROWS - 1; break;
    case 'C': s->col += param(s, 0, 1); if (s->col >= SCREEN_COLS) s->col = SCREEN_COLS - 1; break;
    case 'D': s->col -= param(s, 0, 1); if (s->col < 0) s->col = 0; break;
    case 'H': case 'f': {
        int r = param(s, 0, 1) - 1, c = param(s, 1, 1) - 1;
        s->row = r < 0 ? 0 : r >= SCREEN_ROWS ? SCREEN_ROWS - 1 : r; s->col = c < 0 ? 0 : c >= SCREEN_COLS ? SCREEN_COLS - 1 : c;
        break;
    }
    case 'J': {
        int mode = s->nparams ? s->params[0] : 0;
        if (mode == 2) { for (int y = 0; y < SCREEN_ROWS; y++) erase(s, y, 0, SCREEN_COLS - 1); }
        else if (mode == 1) { for (int y = 0; y < s->row; y++) erase(s, y, 0, SCREEN_COLS - 1); erase(s, s->row, 0, s->col); }
        else { erase(s, s->row, s->col, SCREEN_COLS - 1); for (int y = s->row + 1; y < SCREEN_ROWS; y++) erase(s, y, 0, SCREEN_COLS - 1); }
        break;
    }
    case 'K': {
        int mode = s->nparams ? s->params[0] : 0;
        if (mode == 2) erase(s, s->row, 0, SCREEN_COLS - 1); else if (mode == 1) erase(s, s->row, 0, s->col); else erase(s, s->row, s->col, SCREEN_COLS - 1);
        break;
    }
    case 's': s->saved_row = s->row; s->saved_col = s->col; break;
    case 'u': s->row = s->saved_row; s->col = s->saved_col; break;
    case 'm':
        if (!s->nparams) s->attr = 0;
        for (int i = 0; i < s->nparams; i++) {
            int p = s->params[i];
            if (p == 0) s->attr = 0; else if (p == 1) s->attr |= SCREEN_BOLD; else if (p == 7) s->attr |= SCREEN_REVERSE;
            else if (p == 22) s->attr &= ~SCREEN_BOLD; else if (p == 27) s->attr &= ~SCREEN_REVERSE;
        }
        break;
    default: break;
    }
}
void screen_feed(struct screen *s, const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        uint8_t c = bytes[i];
        if (s->state == S_ESC) {
            if (c == '[') { s->state = S_CSI; s->nparams = 0; memset(s->params, 0, sizeof(s->params)); s->params[0] = -1; }
            else if (c == 'c') { s->state = S_NORMAL; s->attr = 0; screen_clear(s); }
            else s->state = S_NORMAL;
            continue;
        }
        if (s->state == S_CSI) {
            if (c >= '0' && c <= '9') {
                if (!s->nparams) s->nparams = 1;
                int *p = &s->params[s->nparams - 1];
                if (*p < 0) *p = 0;
                if (*p < 10000) *p = *p * 10 + (c - '0');
            } else if (c == ';') { if (s->nparams < 4) { if (!s->nparams) s->nparams = 1; s->nparams++; s->params[s->nparams - 1] = 0; } }
            else if (c >= 0x40 && c <= 0x7e) { if (s->nparams && s->params[0] < 0) s->params[0] = 0; csi(s, c); s->state = S_NORMAL; }
            else if (c < 0x20) { s->state = S_NORMAL; i--; }   /* an aborted sequence: the control byte then acts normally */
            continue;
        }
        switch (c) {
        case 0x1b: if (s->ansi) s->state = S_ESC; break;
        case '\r': s->col = 0; s->wrap_pending = 0; break;
        case '\n': line_feed(s); s->wrap_pending = 0; break;
        case '\b': if (s->col > 0) s->col--; s->wrap_pending = 0; break;
        case '\t': { int next = (s->col / 8 + 1) * 8; s->col = next >= SCREEN_COLS ? SCREEN_COLS - 1 : next; s->wrap_pending = 0; break; }
        case 0x07: s->bells++; break;
        case '\f': screen_clear(s); break;
        default:
            if (c >= 0x20 && c < 0x7f) put(s, c);
            else if (c >= 0xa0) put(s, c);
            else if (c >= 0x80) put(s, '?');
            break;
        }
    }
}
void screen_row_text(const struct screen *s, int row, char *out)
{
    int n = SCREEN_COLS;
    while (n > 0 && s->cells[row][n - 1].ch == ' ') n--;
    for (int x = 0; x < n; x++) out[x] = (char)s->cells[row][x].ch;
    out[n] = 0;
}
int screen_history_text(const struct screen *s, unsigned back, char *out)
{
    if (back >= s->history_count) return 0;
    unsigned slot = (s->history_first + s->history_count - 1 - back) % SCREEN_SCROLLBACK;
    int n = SCREEN_COLS;
    while (n > 0 && s->history[slot][n - 1].ch == ' ') n--;
    for (int x = 0; x < n; x++) out[x] = (char)s->history[slot][x].ch;
    out[n] = 0;
    return 1;
}
