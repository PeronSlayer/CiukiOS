/* SPDX-License-Identifier: MIT
 * Minesweeper engine. */
#include "mines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rnd(struct mines *m)
{
    uint32_t x = m->seed ? m->seed : 0x2545f491u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    m->seed = x; return x;
}
static int inside(const struct mines *m, int x, int y) { return x >= 0 && y >= 0 && x < m->w && y < m->h; }
void mines_best_reset(struct mines *m)
{
    for (int i = 0; i < 3; i++) { m->best.seconds[i] = MINES_TIME_MAX; snprintf(m->best.name[i], sizeof(m->best.name[i]), "Anonymous"); }
}
void mines_init(struct mines *m, uint32_t seed)
{
    memset(m, 0, sizeof(*m)); m->seed = seed; m->max_w = MINES_MAX_W; m->max_h = MINES_MAX_H;
    mines_best_reset(m);
    mines_set_level(m, MINES_BEGINNER, 9, 9, 10);
}
void mines_set_level(struct mines *m, enum mines_level level, int w, int h, int count)
{
    if (level == MINES_BEGINNER) { w = 9; h = 9; count = 10; }
    else if (level == MINES_INTERMEDIATE) { w = 16; h = 16; count = 40; }
    else if (level == MINES_EXPERT) { w = 30; h = 16; count = 99; }   /* needs 480x256 of board */
    else {   /* Custom: 9..30 wide, 9..24 high, 10 to (w-1)*(h-1) mines, as Windows clamps */
        if (w < MINES_MIN_W) w = MINES_MIN_W;
        if (w > m->max_w) w = m->max_w;
        if (h < MINES_MIN_H) h = MINES_MIN_H;
        if (h > m->max_h) h = m->max_h;
        if (count < 10) count = 10;
        if (count > (w - 1) * (h - 1)) count = (w - 1) * (h - 1);
    }
    m->level = level; m->w = w; m->h = h; m->mines = count;
    mines_new(m);
}
void mines_new(struct mines *m)
{
    memset(m->cell, 0, sizeof(m->cell));
    m->state = MINES_READY; m->flags = m->opened = m->seconds = 0;
}
static void place(struct mines *m, int safe_x, int safe_y)
{
    for (int placed = 0; placed < m->mines;) {
        int x = (int)(rnd(m) % (unsigned)m->w), y = (int)(rnd(m) % (unsigned)m->h);
        if ((x == safe_x && y == safe_y) || (m->cell[y][x] & MINES_CELL_MINE)) continue;
        m->cell[y][x] |= MINES_CELL_MINE; placed++;
    }
}
int mines_neighbours(const struct mines *m, int x, int y)
{
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        if ((dx || dy) && inside(m, x + dx, y + dy) && (m->cell[y + dy][x + dx] & MINES_CELL_MINE)) n++;
    return n;
}
int mines_flags_around(const struct mines *m, int x, int y)
{
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        if ((dx || dy) && inside(m, x + dx, y + dy) && (m->cell[y + dy][x + dx] & MINES_CELL_FLAG)) n++;
    return n;
}
int mines_remaining(const struct mines *m) { return m->mines - m->flags; }
static void lose(struct mines *m, int bx, int by)
{
    m->state = MINES_LOST;
    for (int y = 0; y < m->h; y++) for (int x = 0; x < m->w; x++) {
        uint8_t *c = &m->cell[y][x];
        if ((*c & MINES_CELL_MINE) && !(*c & MINES_CELL_FLAG)) *c |= MINES_CELL_OPEN;
        else if (!(*c & MINES_CELL_MINE) && (*c & MINES_CELL_FLAG)) *c |= MINES_CELL_WRONG;
    }
    m->cell[by][bx] |= MINES_CELL_BOOM;
}
static void win(struct mines *m)
{
    m->state = MINES_WON;
    for (int y = 0; y < m->h; y++) for (int x = 0; x < m->w; x++)
        if ((m->cell[y][x] & MINES_CELL_MINE) && !(m->cell[y][x] & MINES_CELL_FLAG)) m->cell[y][x] |= MINES_CELL_FLAG;
    m->flags = m->mines;
}
static int reveal(struct mines *m, int x, int y)    /* opens one closed, unflagged, safe cell; 1 when it has no neighbouring mine */
{
    uint8_t *c = &m->cell[y][x];
    if ((*c & (MINES_CELL_OPEN | MINES_CELL_FLAG | MINES_CELL_MINE))) return 0;
    *c |= MINES_CELL_OPEN; *c &= (uint8_t)~MINES_CELL_MARK; m->opened++;
    return mines_neighbours(m, x, y) == 0;
}
static void flood(struct mines *m, int x0, int y0)
{
    /* A cell is opened when it is queued, so each of the at most 720 cells enters the stack once. */
    uint16_t stack[MINES_MAX_W * MINES_MAX_H]; int top = 0;
    if (reveal(m, x0, y0)) stack[top++] = (uint16_t)(y0 * MINES_MAX_W + x0);
    while (top) {
        int at = stack[--top], x = at % MINES_MAX_W, y = at / MINES_MAX_W;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
            if ((dx || dy) && inside(m, x + dx, y + dy) && reveal(m, x + dx, y + dy)) stack[top++] = (uint16_t)((y + dy) * MINES_MAX_W + x + dx);
    }
}
void mines_open(struct mines *m, int x, int y)
{
    if (!inside(m, x, y) || m->state == MINES_WON || m->state == MINES_LOST) return;
    uint8_t *c = &m->cell[y][x];
    if (*c & (MINES_CELL_OPEN | MINES_CELL_FLAG)) return;
    if (m->state == MINES_READY) {
        place(m, x, y);          /* the mines are laid out on the first click, never under it */
        m->state = MINES_PLAYING;
    }
    if (*c & MINES_CELL_MINE) { lose(m, x, y); return; }
    flood(m, x, y);
    if (m->opened == m->w * m->h - m->mines) win(m);
}
void mines_flag(struct mines *m, int x, int y)
{
    if (!inside(m, x, y) || m->state == MINES_WON || m->state == MINES_LOST) return;
    uint8_t *c = &m->cell[y][x];
    if (*c & MINES_CELL_OPEN) return;
    if (*c & MINES_CELL_FLAG) { *c &= (uint8_t)~MINES_CELL_FLAG; m->flags--; if (m->marks_enabled) *c |= MINES_CELL_MARK; }
    else if (*c & MINES_CELL_MARK) *c &= (uint8_t)~MINES_CELL_MARK;
    else { *c |= MINES_CELL_FLAG; m->flags++; }
}
void mines_chord(struct mines *m, int x, int y)
{
    if (!inside(m, x, y) || m->state != MINES_PLAYING || !(m->cell[y][x] & MINES_CELL_OPEN)) return;
    if (mines_flags_around(m, x, y) != mines_neighbours(m, x, y)) return;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        if ((dx || dy) && inside(m, x + dx, y + dy)) mines_open(m, x + dx, y + dy);
}
void mines_tick(struct mines *m)
{
    if (m->state == MINES_PLAYING && m->seconds < MINES_TIME_MAX) m->seconds++;
}
int mines_qualifies(const struct mines *m)
{
    return m->state == MINES_WON && m->level != MINES_CUSTOM && m->seconds < m->best.seconds[m->level];
}
int mines_record(struct mines *m, const char *name)
{
    if (!mines_qualifies(m)) return -1;
    m->best.seconds[m->level] = m->seconds;
    snprintf(m->best.name[m->level], sizeof(m->best.name[m->level]), "%s", name && *name ? name : "Anonymous");
    return m->level;
}
/* One line per level: "<level> <seconds> <name>". */
int mines_best_save(const struct mines *m, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    static const char *const names[3] = {"beginner", "intermediate", "expert"};
    for (int i = 0; i < 3; i++) fprintf(f, "%s %d %s\n", names[i], m->best.seconds[i], m->best.name[i]);
    return fclose(f) ? -1 : 0;
}
int mines_best_load(struct mines *m, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    static const char *const names[3] = {"beginner", "intermediate", "expert"};
    char line[96];
    while (fgets(line, sizeof(line), f)) {
        char level[24], name[32] = "Anonymous"; int seconds;
        if (sscanf(line, "%23s %d %31[^\n]", level, &seconds, name) < 2) continue;
        for (int i = 0; i < 3; i++) if (!strcmp(level, names[i]) && seconds >= 0 && seconds <= MINES_TIME_MAX) {
            m->best.seconds[i] = seconds; snprintf(m->best.name[i], sizeof(m->best.name[i]), "%s", name);
        }
    }
    fclose(f);
    return 0;
}
