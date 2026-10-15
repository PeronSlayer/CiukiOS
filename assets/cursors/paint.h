/* SPDX-License-Identifier: MIT
 * Original CiukiOS monochrome cursor drawings, 2026-10-15.
 * '.' transparent, '#' dark, 'o' light. Rows occupy a 32 by 32 raster.
 * The same mask, image and hotspot feed software and hardware rendering. */
#ifndef CIUKI_PAINT_CURSORS_H
#define CIUKI_PAINT_CURSORS_H
#include <stdint.h>
#include <string.h>
#include "../../apps/desktop/protocol.h"
struct desk_tool_cursor { uint32_t mask[32], image[32]; int hot_x, hot_y; };
static inline struct desk_tool_cursor desk_tool_cursor(unsigned shape)
{
    static const char *const arrow[] = {
        "#", "##", "#o#", "#oo#", "#ooo#", "#oooo#", "#ooooo#", "#oooooo#",
        "#ooooooo#", "#oooooooo#", "#ooooo#####", "#oo#oo#", "#o#.#oo#", "##..#oo#",
        "#....#oo#", ".....#oo#", "......##"};
    static const char *const pencil[] = {
        ".............###", "............#ooo#", "...........#ooo#o#", "..........#ooo#o#",
        ".........#ooo#o#", "........#ooo#o#", ".......#ooo#o#", "......#ooo#o#",
        ".....#ooo#o#", "....#ooo#o#", "...#ooo#o#", "..#ooo#o#", ".#ooo#o#", ".#oo#o#",
        ".#o#o#", ".#oo#", "#oo#", "###"};
    static const char *const brush[] = {
        "..............###", ".............#ooo#", "............#ooo#", "...........#ooo#",
        "..........#ooo#", ".........#ooo#", "........#ooo#", ".......#ooo#", "......#####",
        ".....#oooo#", "....#oooo#", "...#oooo#", "..#oooo#", ".#oooo#", "#oooo#", "#####"};
    static const char *const bucket[] = {
        ".......#####", "......#ooooo#", ".....#oo###oo#", ".....#o#...#o#", "......#.....#o#",
        ".....###...#o#", "....#ooo#.#o#", "...#ooooo#o#", "..#ooooooo#", ".#ooooooooo#",
        "#ooooooooooo#", ".#ooooooooo#.#", "..#ooooooo#.#o#", "...#ooooo#..#o#", "....#ooo#....#",
        ".....###.....#", "............#o#", ".............#"};
    static const char *const picker[] = {
        "............####", "...........#oooo#", "..........#oooooo#", ".........#oooooo#",
        "........#oooooo#", ".......########", "........#ooo#", ".......#ooo#", "......#ooo#",
        ".....#ooo#", "....#ooo#", "...#ooo#", "..#ooo#", ".#ooo#", ".#oo#", "#oo#", "###"};
    static const char *const magnify[] = {
        "....#######", "..##ooooooo##", ".#ooooooooooo#", ".#ooooo#ooooo#", "#oooooo#oooooo#",
        "#oooo#####oooo#", "#oooooo#oooooo#", ".#ooooo#ooooo#", ".#ooooooooooo#", "..##ooooooo##",
        "....#######o#", "...........#o#", "............#o#", ".............#o#", "..............#o#",
        "...............#o#", "................#o#", ".................#"};
    static const char *const eraser[] = {"#########", "#ooooooo#", "#o#####o#", "#o#...#o#", "#o#...#o#",
        "#o#...#o#", "#o#####o#", "#ooooooo#", "#########"};
    static const char *const text[] = {"##.##", ".#o#", "..#", "..#", "..#", "..#", "..#", "..#", "..#",
        "..#", "..#", "..#", "..#", "..#", "..#", ".#o#", "##.##"};
    static const char *const cross[] = {"......###", "......#o#", "......#o#", "......#o#", "......#o#", "......#o#",
        "#######o#######", "#ooooooooooooo#", "#######o#######", "......#o#", "......#o#", "......#o#",
        "......#o#", "......#o#", "......###"};
    const char *const *rows = arrow; unsigned count = sizeof(arrow)/sizeof(*arrow);
    struct desk_tool_cursor r = {{0},{0},0,0};
#define TOOL_ROWS(name, x, y) do { rows=name; count=sizeof(name)/sizeof(*name); r.hot_x=x; r.hot_y=y; } while (0)
    switch (shape) {
    case DESK_CURSOR_PENCIL: TOOL_ROWS(pencil,0,17); break;
    case DESK_CURSOR_BRUSH: TOOL_ROWS(brush,0,15); break;
    case DESK_CURSOR_FILL: TOOL_ROWS(bucket,13,17); break;
    case DESK_CURSOR_PICK: TOOL_ROWS(picker,0,16); break;
    case DESK_CURSOR_ZOOM: TOOL_ROWS(magnify,7,5); break;
    case DESK_CURSOR_ERASER: TOOL_ROWS(eraser,4,4); break;
    case DESK_CURSOR_TEXT: TOOL_ROWS(text,2,8); break;
    case DESK_CURSOR_CROSSHAIR: case DESK_CURSOR_PRECISION: TOOL_ROWS(cross,7,7); break;
    default: break;
    }
#undef TOOL_ROWS
    for (unsigned y=0; y<count; ++y) for (unsigned x=0; rows[y][x] && x<32; ++x) {
        if (rows[y][x]!='.') r.mask[y] |= 0x80000000u>>x;
        if (rows[y][x]=='o') r.image[y] |= 0x80000000u>>x;
    }
    return r;
}
#endif
