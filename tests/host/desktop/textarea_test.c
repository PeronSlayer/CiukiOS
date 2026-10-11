/* SPDX-License-Identifier: MIT
 * f2-32: libdeskui text area model, layout, pointer, keyboard and rendering.
 * With an argument, also writes the rendered surfaces (<arg>-<name>.xrgb). */
#include "textarea.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct du_textarea ta;
static void load(const char *s, size_t cap)
{
    du_ta_free(&ta);
    assert(!du_ta_init(&ta, cap));
    assert(!du_ta_set_text(&ta, s, strlen(s)));
}
static void same(const char *expect) { assert(ta.len == strlen(expect) && !memcmp(ta.text, expect, ta.len) && !ta.text[ta.len]); }
static void sel(size_t a, size_t b) { size_t x, y; du_ta_selection(&ta, &x, &y); assert(x == a && y == b); }
static void at(size_t cursor) { assert(ta.cursor == cursor && ta.anchor == cursor); }

static void test_editing(void)
{
    load("", DU_TA_LIMIT);
    assert(!ta.modified && !du_ta_can_undo(&ta));
    du_ta_insert(&ta, "Hello", 5, DU_TA_EDIT_TYPE); same("Hello"); at(5); assert(ta.modified && ta.lf == 0);
    du_ta_insert(&ta, "\nWorld", 6, DU_TA_EDIT_TYPE); same("Hello\nWorld"); assert(ta.lf == 1);
    unsigned line, col; du_ta_line_col(&ta, ta.cursor, &line, &col); assert(line == 2 && col == 6);
    du_ta_line_col(&ta, 0, &line, &col); assert(line == 1 && col == 1);
    du_ta_line_col(&ta, 6, &line, &col); assert(line == 2 && col == 1);
    assert(du_ta_lines(&ta) == 2);
    /* Backspace and Delete are character based (UTF-8 aware). */
    load("a\xc3\xa9z", DU_TA_LIMIT); du_ta_move(&ta, DU_TA_DOC_END, 0); at(4);
    du_ta_backspace(&ta); same("a\xc3\xa9"); du_ta_backspace(&ta); same("a"); du_ta_backspace(&ta); same(""); du_ta_backspace(&ta); same("");
    load("\xe7\x8a\xac" "x\xff" "y", DU_TA_LIMIT);
    du_ta_delete(&ta); same("x\xff" "y"); du_ta_move(&ta, DU_TA_RIGHT, 0); du_ta_delete(&ta); same("xy");
    du_ta_move(&ta, DU_TA_DOC_END, 0); du_ta_delete(&ta); same("xy");
    /* A truncated sequence is single invalid bytes and is never read past the end. */
    load("a\xe7\x8a", DU_TA_LIMIT); du_ta_move(&ta, DU_TA_DOC_END, 0); du_ta_backspace(&ta); same("a\xe7");
    /* Selection replacement and deletion. */
    load("one two three", DU_TA_LIMIT);
    du_ta_select(&ta, 4, 7); assert(du_ta_has_selection(&ta)); sel(4, 7);
    du_ta_insert(&ta, "2", 1, DU_TA_EDIT_TYPE); same("one 2 three"); at(5);
    du_ta_select_all(&ta); sel(0, 11); du_ta_delete_selection(&ta); same(""); at(0);
    /* Typed text arrives through du_ta_text, including non-ASCII scalars. */
    load("", DU_TA_LIMIT);
    du_ta_text(&ta, 'A'); du_ta_text(&ta, 0xe9); du_ta_text(&ta, 0x72ac); du_ta_text(&ta, 0x1f600);
    assert(!du_ta_text(&ta, 7) && !du_ta_text(&ta, 127));
    same("A\xc3\xa9\xe7\x8a\xac\xf0\x9f\x98\x80");
    du_ta_move(&ta, DU_TA_LEFT, 0); at(6); du_ta_move(&ta, DU_TA_LEFT, 0); at(3); du_ta_move(&ta, DU_TA_LEFT, 0); at(1);
    du_ta_move(&ta, DU_TA_RIGHT, 0); at(3);
    /* Selections snap to character boundaries. */
    du_ta_select(&ta, 2, 7); sel(1, 6);
}

static void test_undo(void)
{
    load("", DU_TA_LIMIT);
    for (const char *s = "abc"; *s; s++) du_ta_insert(&ta, s, 1, DU_TA_EDIT_TYPE);
    assert(du_ta_can_undo(&ta));
    assert(!du_ta_undo(&ta)); same(""); at(0);
    assert(!du_ta_undo(&ta)); same("abc"); at(3);             /* undo again redoes */
    assert(!du_ta_undo(&ta)); same("");
    assert(!du_ta_undo(&ta)); same("abc");
    /* Moving the caret ends a typing group, even when it returns to the same place. */
    du_ta_insert(&ta, "d", 1, DU_TA_EDIT_TYPE); du_ta_move(&ta, DU_TA_LEFT, 0); du_ta_move(&ta, DU_TA_RIGHT, 0);
    du_ta_insert(&ta, "e", 1, DU_TA_EDIT_TYPE); same("abcde");
    du_ta_undo(&ta); same("abcd");
    /* Backspace runs form one step. */
    load("hello world", DU_TA_LIMIT); du_ta_move(&ta, DU_TA_DOC_END, 0);
    for (int i = 0; i < 5; i++) du_ta_backspace(&ta);
    same("hello "); du_ta_undo(&ta); same("hello world"); at(11);
    /* Forward deletes too. */
    load("abcdef", DU_TA_LIMIT);
    du_ta_delete(&ta); du_ta_delete(&ta); du_ta_delete(&ta); same("def"); du_ta_undo(&ta); same("abcdef");
    /* Typing over a selection continues as one group. */
    load("abc def", DU_TA_LIMIT); du_ta_select(&ta, 0, 3);
    du_ta_insert(&ta, "X", 1, DU_TA_EDIT_TYPE); du_ta_insert(&ta, "Y", 1, DU_TA_EDIT_TYPE); same("XY def");
    du_ta_undo(&ta); same("abc def"); sel(0, 3); du_ta_undo(&ta); same("XY def");
    /* Cut-like deletes are their own step and restore the selection. */
    load("abc def", DU_TA_LIMIT); du_ta_select(&ta, 0, 4); du_ta_delete_selection(&ta); same("def");
    du_ta_undo(&ta); same("abc def"); sel(0, 4);
    load("x", DU_TA_LIMIT); assert(du_ta_undo(&ta) == -1);
    ta.readonly = 1; assert(du_ta_insert(&ta, "y", 1, DU_TA_EDIT_TYPE) == -1); same("x"); assert(!ta.limit_hit);
}

static void test_limit(void)
{
    load("1234", 8);
    assert(!du_ta_insert(&ta, "5678", 4, DU_TA_EDIT_OTHER)); same("56781234");
    assert(ta.len == 8 && !ta.limit_hit);
    assert(du_ta_insert(&ta, "9", 1, DU_TA_EDIT_TYPE) == -1 && ta.limit_hit); same("56781234");
    ta.limit_hit = 0;
    du_ta_select(&ta, 0, 4); assert(!du_ta_insert(&ta, "AB", 2, DU_TA_EDIT_TYPE)); same("AB1234");
    assert(du_ta_set_text(&ta, "123456789", 9) == -1);
    assert(du_ta_replace_all(&ta, "1", 1, "ONE", 3, 1) == 1); same("ABONE234");
    assert(du_ta_replace_all(&ta, "ONE", 3, "xxxx", 4, 1) == -1 && ta.limit_hit); same("ABONE234");
    du_ta_free(&ta);
    assert(du_ta_init(&ta, 0) == -1);
    /* The default limit is the 0.8 CiukNote text limit. */
    assert(DU_TA_LIMIT == 61440u);
}

static void test_movement(void)
{
    load("alpha beta\n\n  gamma", DU_TA_LIMIT); du_ta_set_view(&ta, 40, 10);
    du_ta_move(&ta, DU_TA_DOC_END, 0); at(19);
    du_ta_move(&ta, DU_TA_HOME, 0); at(12);
    du_ta_move(&ta, DU_TA_END, 0); at(19);
    du_ta_move(&ta, DU_TA_UP, 0); at(11);                         /* the empty line */
    du_ta_move(&ta, DU_TA_UP, 0); at(7);                          /* sticky column 7 */
    du_ta_move(&ta, DU_TA_DOWN, 0); at(11); du_ta_move(&ta, DU_TA_DOWN, 0); at(19);
    du_ta_move(&ta, DU_TA_DOWN, 0); at(19);                       /* last line: no move */
    du_ta_move(&ta, DU_TA_DOC_START, 1); sel(0, 19); assert(ta.cursor == 0);
    du_ta_move(&ta, DU_TA_RIGHT, 0); at(19);                      /* collapses to the selection end */
    du_ta_move(&ta, DU_TA_DOC_START, 0);
    du_ta_move(&ta, DU_TA_WORD_RIGHT, 0); at(6);
    du_ta_move(&ta, DU_TA_WORD_RIGHT, 0); at(14);
    du_ta_move(&ta, DU_TA_WORD_LEFT, 0); at(6);
    du_ta_move(&ta, DU_TA_END, 1); sel(6, 10);
    du_ta_move(&ta, DU_TA_LEFT, 0); at(6);                        /* collapses to the selection start */
    du_ta_move(&ta, DU_TA_RIGHT, 1); du_ta_move(&ta, DU_TA_RIGHT, 1); sel(6, 8);
    /* Paging uses the viewport. */
    char many[400]; many[0] = 0;
    for (int i = 0; i < 40; i++) strcat(many, "line\n");
    load(many, DU_TA_LIMIT); du_ta_set_view(&ta, 20, 8);
    du_ta_move(&ta, DU_TA_PAGE_DOWN, 0); assert(ta.cursor == 8 * 5 && ta.top == 8);
    du_ta_move(&ta, DU_TA_PAGE_UP, 0); assert(ta.cursor == 0 && ta.top == 0);
    du_ta_move(&ta, DU_TA_DOC_END, 0); assert(ta.top == du_ta_visual_lines(&ta) - 8);
}

static void check_lines(const char *text, unsigned cols, const unsigned *starts, unsigned n)
{
    load(text, DU_TA_LIMIT); du_ta_set_view(&ta, cols, 10); du_ta_set_wrap(&ta, 1);
    assert(du_ta_visual_lines(&ta) == n);
    for (unsigned i = 0; i < n; i++) assert(ta.vl[i] == starts[i]);
}
static void test_wrap(void)
{
    static const unsigned a[] = {0, 6, 12}, b[] = {0, 4, 8}, c[] = {0, 3, 8}, d[] = {0, 6}, e[] = {0, 2, 3};
    check_lines("hello world again", 8, a, 3);   /* breaks after the last blank that fits */
    check_lines("abcdefghijk", 4, b, 3);         /* hard breaks */
    check_lines("ab cdefghijkl", 5, c, 3);       /* a long word moves down, then hard-breaks */
    check_lines("abc  \nd", 4, d, 2);            /* blanks hang past the edge */
    check_lines("a\n\nb", 80, e, 3);
    /* Tabs advance to the next multiple of 8, counted from the visual line start. */
    unsigned col;
    load("a\tb", DU_TA_LIMIT); du_ta_set_view(&ta, 40, 5);
    du_ta_visual_line_of(&ta, 2, &col); assert(col == 8);
    du_ta_visual_line_of(&ta, 3, &col); assert(col == 9);
    load("\t\tx", DU_TA_LIMIT); du_ta_visual_line_of(&ta, 2, &col); assert(col == 16);
    /* A trailing newline yields an empty last line; maxcol follows the widest line. */
    load("abc\n", DU_TA_LIMIT); assert(du_ta_visual_lines(&ta) == 2 && ta.maxcol == 3);
    load("short\na longer line\nx", DU_TA_LIMIT); du_ta_visual_lines(&ta); assert(ta.maxcol == 13);
    /* Wrapped caret movement: End uses affinity, Home goes to the visual line start. */
    load("hello world again", DU_TA_LIMIT); du_ta_set_view(&ta, 8, 5); du_ta_set_wrap(&ta, 1);
    du_ta_move(&ta, DU_TA_END, 0); at(6); assert(ta.affinity);
    unsigned vi = du_ta_visual_line_of(&ta, 6, &col); assert(vi == 1 && col == 0);
    du_ta_move(&ta, DU_TA_RIGHT, 0); at(7); assert(!ta.affinity);
    du_ta_move(&ta, DU_TA_HOME, 0); at(6);
    du_ta_move(&ta, DU_TA_UP, 0); at(0);
    du_ta_move(&ta, DU_TA_DOWN, 0); at(6);
    /* Changing the width re-wraps; turning wrap off restores one visual line per line. */
    du_ta_set_view(&ta, 20, 5); assert(du_ta_visual_lines(&ta) == 1);
    du_ta_set_wrap(&ta, 0); du_ta_set_view(&ta, 4, 5); assert(du_ta_visual_lines(&ta) == 1 && ta.maxcol == 17);
    /* Non-ASCII characters occupy single cells. */
    load("\xe7\x8a\xac\xe7\x8a\xac\xe7\x8a\xac", DU_TA_LIMIT); du_ta_set_view(&ta, 2, 5); du_ta_set_wrap(&ta, 1);
    assert(du_ta_visual_lines(&ta) == 2 && ta.vl[1] == 6);
}

static void test_goto_and_scroll(void)
{
    load("l1\nl2\nl3\nl4", DU_TA_LIMIT); du_ta_set_view(&ta, 10, 2);
    assert(du_ta_goto_line(&ta, 3) == 0); at(6);
    assert(du_ta_goto_line(&ta, 4) == 0); at(9); assert(ta.top == 2);
    assert(du_ta_goto_line(&ta, 5) == -1 && du_ta_goto_line(&ta, 0) == -1); at(9);
    assert(du_ta_goto_line(&ta, 1) == 0); at(0); assert(ta.top == 0);
    /* Horizontal reveal applies only without wrapping. */
    load("0123456789ABCDEF", DU_TA_LIMIT); du_ta_set_view(&ta, 6, 2);
    du_ta_move(&ta, DU_TA_DOC_END, 0); assert(ta.left == 11);
    du_ta_move(&ta, DU_TA_DOC_START, 0); assert(ta.left == 0);
    du_ta_set_wrap(&ta, 1); du_ta_move(&ta, DU_TA_DOC_END, 0); assert(ta.left == 0);
}

static void test_search(void)
{
    load("The cat and the CAT sat; the End", DU_TA_LIMIT);
    assert(du_ta_find(&ta, "the", 3, 0, 0, 0) == 0);
    assert(du_ta_find(&ta, "the", 3, 1, 0, 0) == 12);
    assert(du_ta_find(&ta, "the", 3, 13, 0, 0) == 25);
    assert(du_ta_find(&ta, "the", 3, 0, 1, 0) == 12);           /* match case */
    assert(du_ta_find(&ta, "The", 3, 0, 1, 0) == 0);
    assert(du_ta_find(&ta, "CAT", 3, 0, 1, 0) == 16);
    assert(du_ta_find(&ta, "cat", 3, 0, 0, 0) == 4);
    assert(du_ta_find(&ta, "cat", 3, 32, 0, 1) == 16);          /* backward: ends at or before `from` */
    assert(du_ta_find(&ta, "cat", 3, 16, 0, 1) == 4);
    assert(du_ta_find(&ta, "cat", 3, 6, 0, 1) == -1);
    assert(du_ta_find(&ta, "", 0, 0, 0, 0) == -1 && du_ta_find(&ta, "zzz", 3, 0, 0, 0) == -1);
    assert(du_ta_find(&ta, "End", 3, 29, 1, 0) == 29 && du_ta_find(&ta, "End", 3, 30, 1, 0) == -1);
    /* Find Next walks the matches; Up walks back. */
    assert(!du_ta_find_select(&ta, "the", 3, 0, 0)); sel(0, 3); assert(ta.cursor == 3);
    assert(!du_ta_find_select(&ta, "the", 3, 0, 0)); sel(12, 15);
    assert(du_ta_selection_equals(&ta, "THE", 3, 0) && !du_ta_selection_equals(&ta, "THE", 3, 1));
    assert(!du_ta_find_select(&ta, "the", 3, 0, 1)); sel(0, 3); assert(ta.cursor == 0);
    assert(du_ta_find_select(&ta, "the", 3, 0, 1) == -1); sel(0, 3);
    /* Replace all: counted on the original text, one undo step. */
    assert(du_ta_replace_all(&ta, "the", 3, "a", 1, 0) == 3); same("a cat and a CAT sat; a End"); at(22);
    assert(ta.lf == 0 && ta.modified);
    du_ta_undo(&ta); same("The cat and the CAT sat; the End");
    assert(du_ta_replace_all(&ta, "zzz", 3, "x", 1, 0) == 0 && du_ta_replace_all(&ta, "", 0, "x", 1, 0) == 0);
    assert(du_ta_replace_all(&ta, "a", 1, "aa", 2, 1) == 3); same("The caat aand the CAT saat; the End");
    load("a\nb\nc", DU_TA_LIMIT); assert(du_ta_replace_all(&ta, "\n", 1, "\n\n", 2, 1) == 2); same("a\n\nb\n\nc"); assert(ta.lf == 4);
    /* Needles never match inside a multi-byte character. */
    load("\xc3\xa9", DU_TA_LIMIT); assert(du_ta_find(&ta, "\xa9", 1, 0, 1, 0) == -1);
    load("\xc3\x89", DU_TA_LIMIT); assert(du_ta_find(&ta, "\xc3\xa9", 2, 0, 0, 0) == -1);   /* ASCII-only case folding */
}

static const struct du_rect BOX = {0, 0, 200, 120};   /* text 180x100 at (2,2): 29 columns, 7 rows (6x13 cells) */
static void test_keys(void)
{
    load("one two", DU_TA_LIMIT); du_ta_fit(&ta, BOX); assert(ta.cols == 29 && ta.rows == 7);
    assert(du_ta_key(&ta, 0x14d, 0) && ta.cursor == 1);
    assert(du_ta_key(&ta, 0x14d, DU_CTRL) && ta.cursor == 4);
    assert(du_ta_key(&ta, 0x14b, DU_CTRL) && ta.cursor == 0);
    assert(du_ta_key(&ta, 0x14f, 0) && ta.cursor == 7);
    assert(du_ta_key(&ta, 0x147, 0) && ta.cursor == 0);
    assert(du_ta_key(&ta, 0x14f, DU_SHIFT) && ta.anchor == 0 && ta.cursor == 7);
    assert(du_ta_key(&ta, 0x14b, 0) && ta.cursor == 0 && ta.anchor == 0);
    assert(du_ta_key(&ta, 0x14f, DU_CTRL) && ta.cursor == 7 && du_ta_key(&ta, 0x147, DU_CTRL) && ta.cursor == 0);
    assert(du_ta_key(&ta, 0x1c, 0)); same("\none two");
    assert(du_ta_key(&ta, 0x0f, 0)); same("\n\tone two");
    assert(du_ta_key(&ta, 0x0e, 0)); same("\none two");
    assert(du_ta_key(&ta, 0x153, 0)); same("\nne two");
    assert(!du_ta_key(&ta, 0x1e, 0) && !du_ta_key(&ta, 0x1c, DU_CTRL) && !du_ta_key(&ta, 0x14b, DU_ALT));
    assert(du_ta_key(&ta, 0x149, 0) && du_ta_key(&ta, 0x151, 0));
}

static int px(int col) { return 4 + col * DU_TA_CELL_W + 1; }       /* inside cell `col`, nearest boundary col */
static int py(int row) { return 4 + row * DU_TA_CELL_H + 4; }
static void test_pointer(void)
{
    load("abc\ndef\nghi", DU_TA_LIMIT); du_ta_fit(&ta, BOX);
    /* Text origin (4,4); a column boundary is nearest within half a cell. */
    assert(du_ta_hit(&ta, BOX, px(2), py(1)) == 6);                  /* row 1, boundary 2 of "def" */
    assert(du_ta_hit(&ta, BOX, 150, py(1)) == 7);                    /* beyond the end of a line */
    assert(du_ta_hit(&ta, BOX, 4, 4) == 0 && du_ta_hit(&ta, BOX, 0, 0) == 0);
    assert(du_ta_hit(&ta, BOX, px(2), 90) >= 8);                     /* below the text: the last line */
    assert(du_ta_mouse_down(&ta, BOX, px(2), py(1), 0, 1000) == 1); at(6);
    assert(du_ta_mouse_move(&ta, BOX, 5, 7) == 1); sel(0, 6); assert(ta.cursor == 0);   /* drag selects */
    du_ta_mouse_up(&ta); assert(du_ta_mouse_move(&ta, BOX, px(2), py(1)) == 0); sel(0, 6);
    assert(du_ta_mouse_down(&ta, BOX, 5, 7, 0, 2000000000ull)); du_ta_mouse_up(&ta); at(0);
    assert(du_ta_mouse_down(&ta, BOX, px(2), py(1), DU_SHIFT, 3000000000ull)); sel(0, 6); assert(ta.cursor == 6 && ta.anchor == 0);
    du_ta_mouse_up(&ta);
    /* Double-click selects the word; a third click is single again. */
    load("abc def ghi", DU_TA_LIMIT); du_ta_fit(&ta, BOX);
    assert(du_ta_mouse_down(&ta, BOX, px(5), py(0), 0, 1000000000ull)); du_ta_mouse_up(&ta); assert(ta.cursor == 5);
    assert(du_ta_mouse_down(&ta, BOX, px(5), py(0), 0, 1300000000ull)); sel(4, 7); du_ta_mouse_up(&ta);
    assert(du_ta_mouse_down(&ta, BOX, px(5), py(0), 0, 1400000000ull)); at(5); du_ta_mouse_up(&ta);
    /* A slow or distant second click is a plain click. */
    assert(du_ta_mouse_down(&ta, BOX, px(5), py(0), 0, 3000000000ull)); du_ta_mouse_up(&ta); at(5);
    assert(du_ta_mouse_down(&ta, BOX, px(5), py(0), 0, 3600000000ull)); du_ta_mouse_up(&ta); at(5);
    assert(du_ta_mouse_down(&ta, BOX, px(5), py(0), 0, 3700000000ull)); du_ta_mouse_up(&ta); sel(4, 7);
    assert(du_ta_mouse_down(&ta, BOX, px(0), py(0), 0, 3800000000ull)); du_ta_mouse_up(&ta); at(0);
    assert(du_ta_mouse_down(&ta, BOX, px(3), py(0), 0, 3900000000ull)); du_ta_mouse_up(&ta); at(3);
    /* Word selection classes: blanks, punctuation, words; never across a newline. */
    load("ab, cd\n  \nef", DU_TA_LIMIT);
    du_ta_select_word(&ta, 2); sel(2, 3);
    du_ta_select_word(&ta, 3); sel(3, 4);
    du_ta_select_word(&ta, 5); sel(4, 6);                    /* "cd" */
    du_ta_select_word(&ta, 6); sel(4, 6);                    /* at the newline: the word before it */
    du_ta_select_word(&ta, 8); sel(7, 9);                    /* a blank-only line */
    du_ta_select_word(&ta, 12); sel(10, 12);
}

static void test_scrollbars(void)
{
    char many[400]; many[0] = 0;
    for (int i = 0; i < 40; i++) strcat(many, "line\n");
    load(many, DU_TA_LIMIT); du_ta_fit(&ta, BOX);               /* 41 visual lines, 7 rows: range 34 */
    int vx = 190;                                               /* the vertical bar spans x 182..198, y 2..102 */
    assert(du_ta_mouse_down(&ta, BOX, vx, 4, 0, 0) && ta.top == 0);        /* up arrow at the top: no change */
    assert(du_ta_mouse_down(&ta, BOX, vx, 94, 0, 0) && ta.top == 1);       /* down arrow */
    assert(du_ta_mouse_down(&ta, BOX, vx, 62, 0, 0) && ta.top == 7);       /* track below the thumb: a page less one line */
    assert(du_ta_mouse_down(&ta, BOX, vx, 22, 0, 0) && ta.top == 1);       /* track above the thumb */
    /* Dragging the thumb. For top=1 the track is 68px, the thumb 11px, at offset 1 (y = 2+16+1 ..). */
    assert(du_ta_mouse_down(&ta, BOX, vx, 2 + 16 + 1 + 3, 0, 0) && ta.drag == DU_TA_DRAG_VTHUMB && ta.grab == 3);
    assert(du_ta_mouse_move(&ta, BOX, vx, 2 + 16 + 57 + 3) && ta.top == 34);
    assert(du_ta_mouse_move(&ta, BOX, vx, 500) && ta.top == 34);
    assert(du_ta_mouse_move(&ta, BOX, vx, 0) && ta.top == 0);
    du_ta_mouse_up(&ta);
    at(0);                                                      /* bar clicks never move the caret */
    /* Horizontal bar (no wrap): a 60 column line in a 29 column view. */
    char wide[80]; memset(wide, 'x', 60); wide[60] = 0;
    load(wide, DU_TA_LIMIT); du_ta_fit(&ta, BOX);
    int hy = 120 - 2 - 8;
    assert(du_ta_mouse_down(&ta, BOX, 6, hy, 0, 0) && ta.left == 0);       /* left arrow at the origin */
    assert(du_ta_mouse_down(&ta, BOX, 170, hy, 0, 0) && ta.left == 1);     /* right arrow */
    assert(du_ta_mouse_down(&ta, BOX, 120, hy, 0, 0) && ta.left == 29);    /* track beyond the thumb: a page less one column */
    /* Wrap removes the horizontal bar and the text area reflows. */
    du_ta_set_wrap(&ta, 1); du_ta_fit(&ta, BOX); assert(du_ta_visual_lines(&ta) == 3 && ta.rows == 7 + 1);
}

static uint32_t canvas[240 * 320];
static struct du_canvas surface = {canvas, 320, 240, {0, 0, 320, 240}};
static void dump(const char *prefix, const char *name)
{
    if (!prefix) return;
    char path[1040]; snprintf(path, sizeof(path), "%s-%s.xrgb", prefix, name);
    FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(canvas, 4, 240 * 320, f) == 240 * 320); fclose(f);
}
static uint32_t at_px(int x, int y) { return canvas[y * 320 + x]; }
static void test_render(const char *prefix)
{
    struct du_rect box = {10, 10, 200, 120};
    canvas[239 * 320 + 319] = 0xaabbccddu;
    load("A\xc3\xa9 x\n\tB", DU_TA_LIMIT);
    du_ta_draw(&surface, box, &ta, 1); dump(prefix, "plain");
    /* 'A' row 0 is 01110: pixels at columns 1..3 of the glyph, cell origin (14,14), glyph 3 rows down. */
    assert(at_px(14 + 1, 17) == DU_INK && at_px(14 + 3, 17) == DU_INK && at_px(14, 17) == DU_WINDOW);
    /* The e-acute is a hollow 5x7 placeholder box in the next cell. */
    assert(at_px(20, 17) == DU_INK && at_px(24, 17) == DU_INK && at_px(20, 20) == DU_INK && at_px(22, 20) == DU_WINDOW && at_px(22, 23) == DU_INK);
    /* Caret one pixel left of the origin cell: focused only. */
    assert(at_px(13, 15) == DU_INK && at_px(13, 25) == DU_INK);
    du_ta_draw(&surface, box, &ta, 0); assert(at_px(13, 15) == DU_WINDOW);
    /* Selection fills whole cells; ink inverts. Unfocused it is gray. */
    du_ta_select(&ta, 0, 1); du_ta_draw(&surface, box, &ta, 1);
    assert(at_px(14, 14 + 12) == DU_SELECT && at_px(14 + 5, 14) == DU_SELECT && at_px(14 + 6 + 5, 14) == DU_WINDOW);
    assert(at_px(14 + 1, 17) == DU_SELECT_INK);
    du_ta_draw(&surface, box, &ta, 0); assert(at_px(14, 14 + 12) == DU_GRAY);
    /* A selected line break highlights one cell after the line. */
    du_ta_select(&ta, 3, 6); du_ta_draw(&surface, box, &ta, 1);
    assert(at_px(14 + 4 * 6 + 3, 14 + 5) == DU_SELECT);
    /* Bold doubles the stems horizontally. */
    du_ta_select(&ta, 0, 0); ta.bold = 1; du_ta_draw(&surface, box, &ta, 0); dump(prefix, "bold");
    assert(at_px(14 + 4, 17) == DU_INK); /* 'A' top row becomes columns 1..4 */
    /* Nothing is drawn outside the box. */
    assert(canvas[239 * 320 + 319] == 0xaabbccddu && at_px(9, 10) == 0 && at_px(10, 9) == 0 && at_px(210, 130) == 0);
    /* Scrollbars: both arrows are drawn; with everything visible they are gray. */
    ta.bold = 0; du_ta_draw(&surface, box, &ta, 0);
    assert(at_px(10 + 2 + 196 - 8, 10 + 2 + 8 + 1) == DU_GRAY);
    /* Clipping: a clip rectangle confines the widget. */
    memset(canvas, 0, sizeof(canvas)); surface.clip = (struct du_rect){10, 10, 50, 50};
    du_ta_draw(&surface, box, &ta, 1);
    assert(at_px(70, 70) == 0 && at_px(30, 30) == DU_WINDOW);
    surface.clip = (struct du_rect){0, 0, 320, 240};
    /* A long document with wrap and the viewport scrolled. */
    char many[800]; many[0] = 0;
    for (int i = 0; i < 40; i++) { char l[24]; snprintf(l, sizeof(l), "line %d of the text\n", i + 1); strcat(many, l); }
    load(many, DU_TA_LIMIT); du_ta_fit(&ta, box); du_ta_set_wrap(&ta, 1); du_ta_fit(&ta, box); du_ta_goto_line(&ta, 30); du_ta_draw(&surface, box, &ta, 1); dump(prefix, "scrolled");
    assert(ta.top > 0);
}

/* Deterministic random operations on a small buffer: invariants after every step. */
static uint32_t rng_state = 0x2545f491u;
static unsigned rnd(unsigned n) { rng_state = rng_state * 1664525u + 1013904223u; return (rng_state >> 8) % n; }
static void check_invariants(void)
{
    assert(ta.len <= ta.cap && !ta.text[ta.len] && ta.cursor <= ta.len && ta.anchor <= ta.len);
    unsigned lf = 0; for (size_t i = 0; i < ta.len; i++) lf += ta.text[i] == '\n';
    assert(lf == ta.lf);
    for (size_t p = ta.cursor, k = 0; k < 2; k++, p = ta.anchor) assert(p == ta.len || ((unsigned char)ta.text[p] & 0xc0) != 0x80);
    unsigned count = du_ta_visual_lines(&ta);
    assert(count >= 1 && count <= ta.len + 1 && ta.vl[0] == 0 && ta.top < count);
    for (unsigned i = 1; i < count; i++) assert(ta.vl[i] > ta.vl[i - 1] && ta.vl[i] <= ta.len);
    for (unsigned i = 1; i < count; i++) { size_t at = ta.vl[i]; assert(ta.text[at - 1] == '\n' || ((unsigned char)ta.text[at] & 0xc0) != 0x80 || at == ta.len); }
    assert(!ta.wrap || ta.left == 0);
    unsigned line, col; du_ta_line_col(&ta, ta.cursor, &line, &col); assert(line >= 1 && line <= ta.lf + 1 && col >= 1);
}
static uint32_t canvas_storage[240 * 320];
static void test_random_operations(void)
{
    static const char *const pieces[] = {"a", "b", " ", "  ", "\t", "\n", "word", "x\n", "\xc3\xa9", "\xe7\x8a\xac", "\xf0\x9f\x98\x80", "line of text", "\n\n", "zz"};
    static const unsigned motions[] = {DU_TA_LEFT, DU_TA_RIGHT, DU_TA_UP, DU_TA_DOWN, DU_TA_HOME, DU_TA_END, DU_TA_DOC_START, DU_TA_DOC_END,
                                       DU_TA_PAGE_UP, DU_TA_PAGE_DOWN, DU_TA_WORD_LEFT, DU_TA_WORD_RIGHT};
    assert(!du_ta_init(&ta, 160));
    du_ta_set_view(&ta, 9, 4);
    struct du_rect box = {0, 0, 200, 120};
    for (unsigned step = 0; step < 30000; step++) {
        switch (rnd(16)) {
        case 0: case 1: case 2: { const char *piece = pieces[rnd(14)]; du_ta_insert(&ta, piece, strlen(piece), (enum du_ta_edit)rnd(2) + DU_TA_EDIT_OTHER); break; }
        case 3: du_ta_backspace(&ta); break;
        case 4: du_ta_delete(&ta); break;
        case 5: case 6: du_ta_move(&ta, (enum du_ta_motion)motions[rnd(12)], (int)rnd(2)); break;
        case 7: du_ta_undo(&ta); if (rnd(2)) du_ta_undo(&ta); break;
        case 8: du_ta_set_wrap(&ta, (int)rnd(2)); break;
        case 9: du_ta_set_view(&ta, 1 + rnd(14), 1 + rnd(6)); break;
        case 10: { const char *needle = pieces[rnd(8)]; const char *repl = pieces[rnd(14)];
                   if (rnd(2)) du_ta_replace_all(&ta, needle, strlen(needle), repl, strlen(repl), (int)rnd(2));
                   else du_ta_find_select(&ta, needle, strlen(needle), (int)rnd(2), (int)rnd(2)); break; }
        case 11: du_ta_goto_line(&ta, 1 + rnd(8)); break;
        case 12: du_ta_select(&ta, rnd((unsigned)ta.len + 1), rnd((unsigned)ta.len + 1)); break;
        case 13: du_ta_mouse_down(&ta, box, (int)rnd(210), (int)rnd(130), rnd(2) ? DU_SHIFT : 0, (uint64_t)step * 100000000ull);
                 du_ta_mouse_move(&ta, box, (int)rnd(210), (int)rnd(130)); du_ta_mouse_up(&ta); break;
        case 14: du_ta_key(&ta, (unsigned[]){0x1c, 0x0f, 0x0e, 0x153, 0x14b, 0x14d, 0x148, 0x150}[rnd(8)], rnd(2) ? DU_SHIFT : 0); break;
        case 15: du_ta_text(&ta, 0x20 + rnd(0x7e - 0x20)); break;
        }
        ta.limit_hit = 0;
        check_invariants();
        if (!(step % 97)) { struct du_canvas c = {canvas_storage, 320, 240, {0, 0, 320, 240}}; du_ta_draw(&c, box, &ta, (int)rnd(2)); check_invariants(); }
    }
    du_ta_free(&ta);
}

int main(int argc, char **argv)
{
    test_editing(); test_undo(); test_limit(); test_movement(); test_wrap(); test_goto_and_scroll();
    test_search(); test_keys(); test_pointer(); test_scrollbars(); test_random_operations();
    test_render(argc > 1 ? argv[1] : NULL);
    du_ta_free(&ta);
    puts("PASS textarea: editing, UTF-8, undo, limit, movement, wrap, search/replace, keys, pointer, scrollbars, rendering");
    return 0;
}
