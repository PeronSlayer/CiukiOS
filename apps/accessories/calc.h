/* SPDX-License-Identifier: MIT
 * Calculator: a Windows 98 class standard calculator (immediate execution,
 * memory, percent, square root, reciprocal) on libdeskui. The engine keeps
 * the typed entry as text and computes in double precision (x87, which the
 * SDK allows in applications). */
#ifndef CIUKI_ACC_CALC_H
#define CIUKI_ACC_CALC_H
#include "acc.h"
#define CALC_DIGITS 32
enum calc_op { CALC_NONE, CALC_ADD, CALC_SUB, CALC_MUL, CALC_DIV };
struct calc {
    double value;          /* left operand / last result */
    double operand;        /* right operand of the last operation (repeated by '=') */
    double memory;
    enum calc_op op, last_op;
    int entering;          /* the display holds an operand: typed, or a function result */
    int typed;             /* digits append to the entry (else the next digit starts a new one) */
    int has_memory, error;
    char entry[CALC_DIGITS + 4];
    char display[64];
};
void calc_init(struct calc *);
void calc_digit(struct calc *, int digit);
void calc_point(struct calc *);
void calc_operator(struct calc *, enum calc_op);
void calc_equals(struct calc *);
void calc_clear(struct calc *);        /* C */
void calc_clear_entry(struct calc *);  /* CE */
void calc_backspace(struct calc *);
void calc_negate(struct calc *);       /* +/- */
void calc_percent(struct calc *);
void calc_sqrt(struct calc *);
void calc_reciprocal(struct calc *);
void calc_memory_clear(struct calc *);
void calc_memory_recall(struct calc *);
void calc_memory_store(struct calc *);
void calc_memory_add(struct calc *);
/* Keyboard: '0'-'9' . + - * / = Enter Esc Del Backspace % @ (sqrt) r (1/x) F9 (+/-). Returns 1 when handled. */
int calc_key(struct calc *, unsigned scancode, unsigned ch, unsigned modifiers);
double calc_current(const struct calc *);
/* Paste: feeds the characters of a text as keys; returns the number used. */
int calc_paste(struct calc *, const char *);

/* View (calc_view.c). */
#define CALC_W 260
#define CALC_H 232
struct calc_app {
    struct du_ui *ui; struct calc calc;
    struct du_menu menus[3]; struct du_menu_item edit_items[2], view_items[2], help_items[2];
    int digit_grouping;
};
enum { CALC_M_COPY = 1, CALC_M_PASTE, CALC_M_STANDARD, CALC_M_GROUPING, CALC_M_HELP, CALC_M_ABOUT };
enum { CALC_B_FIRST = 100 };   /* button ids: CALC_B_FIRST + index in the key table */
void calc_attach(struct calc_app *, struct du_ui *);
void calc_event(struct du_ui *, const struct du_event *, void *);
void calc_paint(struct du_ui *, void *);
/* The button of a label ("7", "sqrt", "MC", ...); -1 when absent (tests). */
int calc_button_id(const char *label);
#endif
