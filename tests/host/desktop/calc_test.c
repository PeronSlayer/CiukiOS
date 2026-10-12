/* Calculator engine and window host tests (legacy recovery, 0.9.0).
 * SPDX-License-Identifier: MIT */
#include "calc.h"
#include "acc_test.h"
static struct calc c;
static void keys(const char *s) { calc_init(&c); for (; *s; s++) calc_key(&c, 0, (unsigned char)*s, 0); }
static void expect(const char *input, const char *display)
{
    keys(input);
    if (strcmp(c.display, display)) { fprintf(stderr, "calc '%s': got '%s', want '%s'\n", input, c.display, display); abort(); }
}
int main(int argc, char **argv)
{
    const char *folder = argc > 1 ? argv[1] : NULL;
    calc_init(&c); assert(!strcmp(c.display, "0."));
    expect("12", "12.");
    expect("2+3=", "5.");
    expect("2+3*4=", "20.");          /* immediate execution */
    expect("10-4==", "2.");           /* = repeats - 4 */
    expect("2*=", "4.");              /* 2 * = squares */
    expect("7/2=", "3.5");
    expect("1/3=", "0.333333333333333");
    expect("5/0=", "Cannot divide by zero.");
    expect("0/0=", "Result of function is undefined.");
    expect("9@", "3.");
    expect("2+9@=", "5.");            /* sqrt result is the right operand */
    expect("4r", "0.25");
    expect("50+10%", "5.");           /* 50 + 10% -> right operand 5 */
    expect("50+10%=", "55.");
    expect("0.1+0.2=", "0.3");
    expect("..5", "0.5");
    expect("007", "7.");
    expect("1e", "1.");               /* unknown keys are ignored */
    /* negate, backspace, CE, C and memory */
    keys("123"); calc_negate(&c); assert(!strcmp(c.display, "-123."));
    calc_backspace(&c); assert(!strcmp(c.display, "-12."));
    calc_clear_entry(&c); assert(!strcmp(c.display, "0."));
    keys("6*7="); calc_memory_store(&c); assert(c.has_memory && c.memory == 42);
    calc_clear(&c); assert(c.has_memory && !strcmp(c.display, "0."));
    calc_memory_recall(&c); assert(!strcmp(c.display, "42."));
    calc_key(&c, 0, '+', 0); calc_memory_recall(&c); calc_equals(&c); assert(!strcmp(c.display, "84."));
    calc_memory_add(&c); assert(c.memory == 126);
    calc_memory_clear(&c); assert(!c.has_memory);
    /* errors clear on the next digit; operators are ignored meanwhile */
    keys("1/0="); calc_operator(&c, CALC_ADD); assert(c.error);
    calc_digit(&c, 4); assert(!c.error && !strcmp(c.display, "4."));
    keys("4"); calc_negate(&c); calc_sqrt(&c); assert(!strcmp(c.display, "Invalid input for function."));
    /* digit limit */
    calc_init(&c); for (int i = 0; i < 40; i++) calc_digit(&c, 9);
    assert(strlen(c.entry) == CALC_DIGITS);
    /* keyboard scan codes: keypad, Enter, Esc, F9 */
    calc_init(&c); calc_key(&c, 0x4f, 0, 0); calc_key(&c, 0x4e, 0, 0); calc_key(&c, 0x50, 0, 0); calc_key(&c, DU_KEY_ENTER, 0, 0);
    assert(!strcmp(c.display, "3."));
    calc_key(&c, 0x43, 0, 0); assert(!strcmp(c.display, "-3."));
    calc_key(&c, DU_KEY_ESC, 0, 0); assert(!strcmp(c.display, "0."));
    assert(calc_paste(&c, "12*3\n") == 5 && !strcmp(c.display, "36."));

    /* The window: clicks on the keys, keyboard through libdeskui, menus. */
    static uint32_t pixels[CALC_W * CALC_H];
    static struct du_ui ui; static struct calc_app app;
    du_init(&ui, pixels, CALC_W, CALC_H);
    calc_attach(&app, &ui);
    du_render(&ui);
    at_save(folder, "calc-initial", &ui);
    at_click_widget(&ui, calc_button_id("7")); at_click_widget(&ui, calc_button_id("*"));
    at_click_widget(&ui, calc_button_id("6")); at_click_widget(&ui, calc_button_id("="));
    assert(!strcmp(app.calc.display, "42."));
    at_click_widget(&ui, calc_button_id("MS"));
    at_tap(&ui, 0x02); at_tap(&ui, 0x03); at_tap(&ui, 0x04); at_tap(&ui, 0x05);   /* 1234 typed */
    assert(!strcmp(app.calc.display, "1234."));
    calc_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = CALC_M_GROUPING}, &app);
    at_tap(&ui, 0x06); at_tap(&ui, 0x07); at_tap(&ui, 0x08);                       /* 1234567 */
    du_render(&ui);
    at_save(folder, "calc-grouped-memory", &ui);
    at_chord(&ui, DU_KEY_LCTRL, 0x2e);                                               /* Ctrl+C */
    assert(!strcmp(du_clipboard_get(), "1234567"));
    at_tap(&ui, DU_KEY_ESC); assert(!strcmp(app.calc.display, "0."));
    puts("calc: PASS");
    return 0;
}
