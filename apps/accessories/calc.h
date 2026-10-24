/* SPDX-License-Identifier: MIT
 * Calculator: a Windows 98 class calculator on libdeskui. Standard view:
 * immediate execution, memory, percent, square root, reciprocal. Scientific
 * view (G27, AX-4): operator precedence with parentheses, Dec/Hex/Oct/Bin,
 * Degrees/Radians/Grads, Inv and Hyp, the trigonometric, logarithmic, power
 * and logical functions and the statistics data keys of the Windows 98
 * Scientific Calculator. The engine keeps the typed entry as text and
 * computes in double precision (x87, which the SDK allows in applications). */
#ifndef CIUKI_ACC_CALC_H
#define CIUKI_ACC_CALC_H
#include "acc.h"
#define CALC_DIGITS 32          /* decimal digits of an entry */
#define CALC_ENTRY 64           /* longest entry text (64 binary digits) */
#define CALC_STACK 25           /* pending operators and parentheses (Windows: 25 levels) */
#define CALC_STAT 256           /* statistics data points */
enum calc_op { CALC_NONE, CALC_ADD, CALC_SUB, CALC_MUL, CALC_DIV,
               CALC_POW, CALC_ROOT, CALC_MOD, CALC_AND, CALC_OR, CALC_XOR, CALC_LSH, CALC_RSH };
enum calc_angle { CALC_DEG, CALC_RAD, CALC_GRAD };
struct calc_pending { double value; int op; };   /* op -1: an open parenthesis */
struct calc {
    double value;          /* left operand / last result */
    double operand;        /* right operand of the last operation (repeated by '=') */
    double memory;
    enum calc_op op, last_op;
    int entering;          /* the display holds an operand: typed, or a function result */
    int typed;             /* digits append to the entry (else the next digit starts a new one) */
    int has_memory, error;
    int use_exact; double exact;   /* the entry shows a result: later operations use its full precision */
    char entry[CALC_ENTRY + 4];
    char display[96];
    /* Scientific view. */
    int scientific, base, angle, inv, hyp, fe;
    int depth; struct calc_pending stack[CALC_STACK];
    int nstat; double stat[CALC_STAT];
};
void calc_init(struct calc *);
void calc_digit(struct calc *, int digit);   /* 0..9, 10..15 (A-F) in Hex */
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
/* Scientific view (calc_scientific is the View menu choice). */
void calc_scientific(struct calc *, int on);
void calc_set_base(struct calc *, int base);        /* 16, 10, 8 or 2 */
void calc_set_angle(struct calc *, enum calc_angle);
void calc_toggle_inv(struct calc *);
void calc_toggle_hyp(struct calc *);
void calc_toggle_fe(struct calc *);
void calc_binary(struct calc *, enum calc_op);       /* x^y (Inv: root), Lsh (Inv: Rsh), Mod, And, Or, Xor */
void calc_open_paren(struct calc *);
void calc_close_paren(struct calc *);
int calc_paren_depth(const struct calc *);
void calc_sin(struct calc *); void calc_cos(struct calc *); void calc_tan(struct calc *);
void calc_log(struct calc *); void calc_ln(struct calc *);
void calc_square(struct calc *);                    /* x^2 (Inv: square root) */
void calc_cube(struct calc *);                      /* x^3 (Inv: cube root) */
void calc_factorial(struct calc *);
void calc_pi(struct calc *);                        /* pi (Inv: 2 pi) */
void calc_exp(struct calc *);                       /* Exp: type the exponent */
void calc_dms(struct calc *);                       /* degrees to d.mmss (Inv: back) */
void calc_int(struct calc *);                       /* Int (Inv: fraction) */
void calc_not(struct calc *);
void calc_stat_clear(struct calc *);                /* CAD */
int calc_stat_load(struct calc *, unsigned index);
int calc_stat_delete(struct calc *, unsigned index);
void calc_stat_data(struct calc *);                 /* Dat */
void calc_stat_ave(struct calc *); void calc_stat_sum(struct calc *); void calc_stat_dev(struct calc *);
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
#define CALC_STD_W 260          /* Windows 98 standard layout */
#define CALC_STD_H 232
#define CALC_W 440              /* the window: large enough for the Scientific view (no client resize in protocol v2) */
#define CALC_H 300
struct calc_app {
    struct du_ui *ui; struct calc calc;
    struct du_menu menus[3]; struct du_menu_item edit_items[2], view_items[4], help_items[2];
    int digit_grouping;
    struct du_item stat_rows[CALC_STAT];
    char stat_text[CALC_STAT][48];
};
enum { CALC_M_COPY = 1, CALC_M_PASTE, CALC_M_STANDARD, CALC_M_GROUPING, CALC_M_HELP, CALC_M_ABOUT, CALC_M_SCIENTIFIC };
enum { CALC_R_HEX = 20, CALC_R_DEC, CALC_R_OCT, CALC_R_BIN, CALC_R_DEG, CALC_R_RAD, CALC_R_GRAD, CALC_C_INV, CALC_C_HYP };
enum { CALC_D_STAT = 300, CALC_STAT_LIST, CALC_STAT_LOAD, CALC_STAT_CD, CALC_STAT_CAD };
void calc_statistics_box(struct calc_app *);
enum { CALC_B_FIRST = 100 };   /* button ids: CALC_B_FIRST + index in the key table */
void calc_attach(struct calc_app *, struct du_ui *);
void calc_event(struct du_ui *, const struct du_event *, void *);
void calc_paint(struct du_ui *, void *);
/* The button of a label ("7", "sqrt", "MC", ...); -1 when absent (tests). */
int calc_button_id(const char *label);
int calc_sci_button_id(const char *label);   /* the Scientific table (ids differ from Standard; "C" is the clear key) */
void calc_view_set(struct calc_app *, int scientific);   /* View menu: 0 Standard, 1 Scientific */
#endif
