/* Minesweeper engine and window host tests (G27). SPDX-License-Identifier: MIT */
#include "mines.h"
#include "acc_test.h"
static uint32_t pixels[MINES_W * MINES_H];
static struct du_ui ui; static struct mines_app app;
static int relayout_w, relayout_h, relayout_calls;
static void relayout(void *host, int w, int h) { (void)host; relayout_w = w; relayout_h = h; relayout_calls++; }
static void click_button(int x, int y, int button)
{
    at_move(&ui, x, y);
    struct ciuki_input_event e = {.type = CIUKI_INPUT_BUTTON, .code = button, .value = 1}; du_input(&ui, &e);
    e.value = 0; du_input(&ui, &e);
}
static void cell_xy(int cx, int cy, int *x, int *y)
{
    struct du_rect b = mines_board_rect(&app.game);
    *x = b.x + cx * MINES_CELL + 8; *y = b.y + cy * MINES_CELL + 8;
}
static int mine_count(const struct mines *m)
{
    int n = 0;
    for (int y = 0; y < m->h; y++) for (int x = 0; x < m->w; x++) n += (m->cell[y][x] & MINES_CELL_MINE) != 0;
    return n;
}
static void open_all_safe(struct mines *m)
{
    for (int y = 0; y < m->h; y++) for (int x = 0; x < m->w; x++)
        if (!(m->cell[y][x] & MINES_CELL_MINE)) mines_open(m, x, y);
}
int main(int argc, char **argv)
{
    const char *folder = argc > 1 ? argv[1] : NULL;
    static struct mines m;
    mines_init(&m, 12345);
    assert(m.w == 9 && m.h == 9 && m.mines == 10 && m.level == MINES_BEGINNER && m.state == MINES_READY && mines_remaining(&m) == 10);
    mines_set_level(&m, MINES_INTERMEDIATE, 0, 0, 0); assert(m.w == 16 && m.h == 16 && m.mines == 40);
    mines_set_level(&m, MINES_EXPERT, 0, 0, 0); assert(m.w == 30 && m.h == 16 && m.mines == 99);
    mines_set_level(&m, MINES_CUSTOM, 5, 5, 3); assert(m.w == 9 && m.h == 9 && m.mines == 10);              /* clamped up */
    mines_set_level(&m, MINES_CUSTOM, 99, 99, 9999); assert(m.w == 30 && m.h == 24 && m.mines == 29 * 23);   /* clamped down */
    m.max_h = 19; mines_set_level(&m, MINES_CUSTOM, 20, 30, 50); assert(m.h == 19 && m.w == 20 && m.mines == 50);
    m.max_h = MINES_MAX_H;

    /* The first click is never a mine, on every level, for many boards; mines are laid out then. */
    for (uint32_t seed = 1; seed <= 400; seed++) {
        mines_init(&m, seed * 2654435761u);
        mines_set_level(&m, (enum mines_level)(seed % 3), 0, 0, 0);
        assert(mine_count(&m) == 0);
        int x = (int)(seed % (unsigned)m.w), y = (int)(seed / 3 % (unsigned)m.h);
        mines_open(&m, x, y);
        assert(m.state == MINES_PLAYING || m.state == MINES_WON);
        assert(!(m.cell[y][x] & MINES_CELL_MINE) && (m.cell[y][x] & MINES_CELL_OPEN));
        assert(mine_count(&m) == m.mines);
    }
    /* A zero opens its region (flood fill), numbers stop it, flags block it. */
    mines_init(&m, 99); mines_open(&m, 4, 4);
    assert(m.opened >= 1);
    int zero_region = 0;
    for (int y = 0; y < m.h; y++) for (int x = 0; x < m.w; x++)
        if ((m.cell[y][x] & MINES_CELL_OPEN) && mines_neighbours(&m, x, y) == 0) zero_region++;
    if (mines_neighbours(&m, 4, 4) == 0) {
        assert(m.opened > 1 && zero_region >= 1);
        for (int y = 0; y < m.h; y++) for (int x = 0; x < m.w; x++)      /* every neighbour of an opened zero is opened */
            if ((m.cell[y][x] & MINES_CELL_OPEN) && mines_neighbours(&m, x, y) == 0)
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
                    if (x + dx >= 0 && y + dy >= 0 && x + dx < m.w && y + dy < m.h) assert(m.cell[y + dy][x + dx] & MINES_CELL_OPEN);
    }
    /* Flags, marks and the counter. */
    mines_init(&m, 7); mines_flag(&m, 0, 0); assert((m.cell[0][0] & MINES_CELL_FLAG) && mines_remaining(&m) == 9 && m.flags == 1);
    mines_open(&m, 0, 0); assert(!(m.cell[0][0] & MINES_CELL_OPEN) && m.state == MINES_READY);          /* a flagged cell does not open */
    mines_flag(&m, 0, 0); assert(!(m.cell[0][0] & (MINES_CELL_FLAG | MINES_CELL_MARK)) && m.flags == 0);   /* marks off: flag, clear */
    m.marks_enabled = 1;
    mines_flag(&m, 1, 1); mines_flag(&m, 1, 1); assert((m.cell[1][1] & MINES_CELL_MARK) && !(m.cell[1][1] & MINES_CELL_FLAG) && m.flags == 0);
    mines_flag(&m, 1, 1); assert(!(m.cell[1][1] & (MINES_CELL_FLAG | MINES_CELL_MARK)));
    for (int i = 0; i < 12; i++) mines_flag(&m, i % 9, 8 - i / 9);
    assert(mines_remaining(&m) < 0 || m.flags == 12);                                                   /* more flags than mines: negative counter */
    mines_flag(&m, -1, 0); mines_flag(&m, 9, 0); mines_open(&m, 0, 9);                                  /* outside the board: ignored */

    /* Lose: the boom cell, the other mines shown, wrong flags marked. */
    mines_init(&m, 42); mines_open(&m, 4, 4);
    int bx = -1, by = -1, fx = -1, fy = -1;
    for (int y = 0; y < m.h; y++) for (int x = 0; x < m.w; x++) {
        uint8_t c = m.cell[y][x];
        if ((c & MINES_CELL_MINE) && bx < 0) { bx = x; by = y; }
        if (!(c & (MINES_CELL_MINE | MINES_CELL_OPEN)) && fx < 0) { fx = x; fy = y; }
    }
    mines_flag(&m, fx, fy);
    mines_open(&m, bx, by);
    assert(m.state == MINES_LOST && (m.cell[by][bx] & MINES_CELL_BOOM) && (m.cell[fy][fx] & MINES_CELL_WRONG));
    int shown = 0; for (int y = 0; y < m.h; y++) for (int x = 0; x < m.w; x++) shown += (m.cell[y][x] & MINES_CELL_MINE) && (m.cell[y][x] & MINES_CELL_OPEN);
    assert(shown == m.mines);
    int lost_seconds = m.seconds, lost_flags = m.flags;
    mines_open(&m, 0, 0); mines_flag(&m, 0, 0); mines_tick(&m);     /* a finished game ignores input */
    assert(m.state == MINES_LOST && m.seconds == lost_seconds && m.flags == lost_flags);

    /* Chording: opens the unflagged neighbours when the flags match; a wrong flag loses. */
    mines_init(&m, 5); mines_open(&m, 4, 4);
    int cx = -1, cy = -1;
    for (int y = 0; y < m.h && cx < 0; y++) for (int x = 0; x < m.w; x++)
        if ((m.cell[y][x] & MINES_CELL_OPEN) && mines_neighbours(&m, x, y) > 0) { cx = x; cy = y; break; }
    assert(cx >= 0);
    int need = mines_neighbours(&m, cx, cy), before = m.opened;
    mines_chord(&m, cx, cy); assert(m.opened == before);                                                /* no flags yet */
    int placed = 0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int x = cx + dx, y = cy + dy;
        if ((dx || dy) && x >= 0 && y >= 0 && x < m.w && y < m.h && (m.cell[y][x] & MINES_CELL_MINE) && placed < need) { mines_flag(&m, x, y); placed++; }
    }
    assert(mines_flags_around(&m, cx, cy) == need);
    mines_chord(&m, cx, cy); assert(m.state != MINES_LOST);
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int x = cx + dx, y = cy + dy;
        if ((dx || dy) && x >= 0 && y >= 0 && x < m.w && y < m.h && !(m.cell[y][x] & (MINES_CELL_MINE | MINES_CELL_FLAG))) assert(m.cell[y][x] & MINES_CELL_OPEN);
    }

    /* Win: every safe cell opened; the mines get flags; the timer stops; best times. */
    mines_init(&m, 77); mines_open(&m, 0, 0);
    for (int i = 0; i < 25; i++) mines_tick(&m);
    assert(m.seconds == 25);
    open_all_safe(&m);
    assert(m.state == MINES_WON && m.flags == m.mines && mines_remaining(&m) == 0);
    mines_tick(&m); assert(m.seconds == 25);
    assert(mines_qualifies(&m) && mines_record(&m, "Ciuki") == 0 && m.best.seconds[0] == 25 && !strcmp(m.best.name[0], "Ciuki"));
    assert(!mines_qualifies(&m) && mines_record(&m, "x") == -1);
    char path[256]; snprintf(path, sizeof(path), "%s/mines-best.cfg", folder ? folder : "/tmp");
    if (folder) {
        assert(!mines_best_save(&m, path));
        struct mines other; mines_init(&other, 1); assert(!mines_best_load(&other, path));
        assert(other.best.seconds[0] == 25 && !strcmp(other.best.name[0], "Ciuki") && other.best.seconds[1] == MINES_TIME_MAX && !strcmp(other.best.name[2], "Anonymous"));
        FILE *f = fopen(path, "w"); fputs("beginner 7 A B\nexpert 99999 Z\nbogus 3 q\nintermediate 41 Caro\n", f); fclose(f);
        assert(!mines_best_load(&other, path));
        assert(other.best.seconds[0] == 7 && !strcmp(other.best.name[0], "A B") && other.best.seconds[1] == 41 && other.best.seconds[2] == MINES_TIME_MAX);
        remove(path);
    }
    mines_set_level(&m, MINES_CUSTOM, 9, 9, 10); open_all_safe(&m);   /* custom boards have no best time */
    assert(!mines_qualifies(&m));
    mines_best_reset(&m); assert(m.best.seconds[2] == MINES_TIME_MAX);
    /* Timer limit. */
    mines_init(&m, 3); mines_open(&m, 0, 0); m.seconds = 998; mines_tick(&m); mines_tick(&m); mines_tick(&m);
    assert(m.state == MINES_PLAYING && m.seconds == MINES_TIME_MAX);

    /* The window: pointer input on the canvas, face button, menu, timer, chord with two buttons. */
    du_init(&ui, pixels, MINES_W, MINES_H);
    mines_init(&app.game, 2024);
    mines_attach(&app, &ui);
    du_render(&ui);
    assert(app.game.max_h <= MINES_MAX_H);
    at_save(folder, "mines-initial", &ui);
    int x, y;
    cell_xy(4, 4, &x, &y); click_button(x, y, 2);                                    /* right click flags */
    assert(app.game.cell[4][4] & MINES_CELL_FLAG && app.game.flags == 1 && app.game.state == MINES_READY);
    click_button(x, y, 2); assert(!(app.game.cell[4][4] & MINES_CELL_FLAG));
    click_button(x, y, 1);                                                           /* left click opens, starts the timer */
    assert(app.game.state == MINES_PLAYING && (app.game.cell[4][4] & MINES_CELL_OPEN));
    int running = 0;
    for (int i = 0; i < DU_TIMERS; i++) running += ui.timers[i].active && ui.timers[i].id == MN_TIMER;
    assert(running == 1);                                                            /* the first click starts the one-second timer */
    mines_event(&ui, &(struct du_event){.type = DU_EV_TICK, .id = MN_TIMER}, &app); assert(app.game.seconds == 1);
    du_render(&ui);
    at_save(folder, "mines-playing", &ui);
    int sec = app.game.seconds;
    struct du_rect face = mines_face_rect(&app.game);
    click_button(face.x + 5, face.y + 5, 1);                                         /* the face restarts */
    assert(app.game.state == MINES_READY && app.game.seconds == 0 && sec == 1 && mine_count(&app.game) == 0);
    running = 0;
    for (int i = 0; i < DU_TIMERS; i++) running += ui.timers[i].active && ui.timers[i].id == MN_TIMER;
    assert(running == 0);
    /* Menu: levels, marks. */
    struct du_event ev = {.type = DU_EV_ACTIVATE, .id = MN_EXPERT}; mines_event(&ui, &ev, &app);
    assert(app.game.level == MINES_EXPERT && (app.game_items[4].flags & DU_ITEM_CHECKED) && !(app.game_items[2].flags & DU_ITEM_CHECKED));
    du_render(&ui);
    at_save(folder, "mines-expert", &ui);
    /* The window wraps its board: a level that needs another size asks the host to reopen it (Windows resizes). */
    {
        int ww, wh; mines_window_size(&app.game, &ww, &wh);
        assert(ww == 30 * MINES_CELL + 24 && wh == 72 + 16 * MINES_CELL + 12);
        app.relayout = relayout;
        ev.id = MN_EXPERT; mines_event(&ui, &ev, &app);   /* the harness canvas is 504 x 440: Expert wraps to 504 x 340 */
        assert(relayout_calls == 1 && relayout_w == ww && relayout_h == wh);
        ev.id = MN_BEGINNER; mines_event(&ui, &ev, &app);
        assert(relayout_calls == 2 && relayout_w == 9 * MINES_CELL + 24 && relayout_h == 72 + 9 * MINES_CELL + 12 && app.game.level == MINES_BEGINNER);
        app.relayout = NULL;
        ev.id = MN_EXPERT; mines_event(&ui, &ev, &app);
        app.game.view_w = 9 * MINES_CELL + 24;                                           /* a beginner-sized window centres the board */
        mines_set_level(&app.game, MINES_BEGINNER, 0, 0, 0);
        struct du_rect nb = mines_board_rect(&app.game); assert(nb.x == 12 && nb.x + nb.w + 12 == app.game.view_w);
        app.game.view_w = MINES_W; ev.id = MN_EXPERT; mines_event(&ui, &ev, &app);
    }
    ev.id = MN_MARKS; mines_event(&ui, &ev, &app); assert(app.game.marks_enabled && (app.game_items[7].flags & DU_ITEM_CHECKED));
    ev.id = MN_BEGINNER; mines_event(&ui, &ev, &app);
    ev.id = MN_CUSTOM; mines_event(&ui, &ev, &app); assert(ui.dialog.kind && !strcmp(ui.dialog.items[2].text, "16 16 40")); ui.dialog.kind = 0;   /* the Custom field box, with the usual numbers */
    mines_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = MN_CUSTOM_BOX, .index = 0, .text = "12 20 30"}, &app);
    assert(app.game.level == MINES_CUSTOM && app.game.h == 12 && app.game.w == 20 && app.game.mines == 30 && (app.game_items[5].flags & DU_ITEM_CHECKED));
    mines_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = MN_CUSTOM_BOX, .index = 0, .text = "5 99 1"}, &app);        /* clamped: 9 x 30, at least 10 mines */
    assert(app.game.h == 9 && app.game.w == 30 && app.game.mines == 10);
    mines_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = MN_CUSTOM_BOX, .index = 0, .text = "abc"}, &app); assert(ui.dialog.kind && app.game.w == 30); ui.dialog.kind = 0;
    mines_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = MN_CUSTOM_BOX, .index = 1, .text = "10 10 10"}, &app); assert(app.game.w == 30);   /* Cancel */
    du_render(&ui);
    ev.id = MN_BEGINNER; mines_event(&ui, &ev, &app);
    /* Middle button chord on an opened number, through the window. */
    mines_init(&app.game, 5);
    cell_xy(4, 4, &x, &y); click_button(x, y, 1);
    for (int cy2 = 0; cy2 < 9; cy2++) for (int cx2 = 0; cx2 < 9; cx2++)
        if ((app.game.cell[cy2][cx2] & MINES_CELL_OPEN) && mines_neighbours(&app.game, cx2, cy2)) {
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int nx = cx2 + dx, ny = cy2 + dy;
                if ((dx || dy) && nx >= 0 && ny >= 0 && nx < 9 && ny < 9 && (app.game.cell[ny][nx] & MINES_CELL_MINE) && !(app.game.cell[ny][nx] & MINES_CELL_FLAG)) {
                    int px, py; cell_xy(nx, ny, &px, &py); click_button(px, py, 2);
                }
            }
            int before_open = app.game.opened; cell_xy(cx2, cy2, &x, &y);
            click_button(x, y, 3);
            assert(mines_flags_around(&app.game, cx2, cy2) == mines_neighbours(&app.game, cx2, cy2) && app.game.opened >= before_open);
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {   /* the middle button opened every safe neighbour */
                int nx = cx2 + dx, ny = cy2 + dy;
                if ((dx || dy) && nx >= 0 && ny >= 0 && nx < 9 && ny < 9 && !(app.game.cell[ny][nx] & (MINES_CELL_MINE | MINES_CELL_FLAG)))
                    assert(app.game.cell[ny][nx] & MINES_CELL_OPEN);
            }
            goto done_chord;
        }
done_chord:
    /* Lose through the window: the face shows it and the board is revealed. */
    mines_init(&app.game, 11); cell_xy(0, 0, &x, &y); click_button(x, y, 1);
    for (int by2 = 0; by2 < 9; by2++) for (int bx2 = 0; bx2 < 9; bx2++)
        if (app.game.state == MINES_PLAYING && (app.game.cell[by2][bx2] & MINES_CELL_MINE)) { cell_xy(bx2, by2, &x, &y); click_button(x, y, 1); }
    assert(app.game.state == MINES_LOST);
    du_render(&ui);
    at_save(folder, "mines-lost", &ui);
    puts("mines: PASS");
    return 0;
}
