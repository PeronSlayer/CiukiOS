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
static void sci(const char *s) { calc_init(&c); calc_scientific(&c, 1); for (; *s; s++) calc_key(&c, 0, (unsigned char)*s, 0); }
static void sexpect(const char *input, const char *display)
{
    sci(input);
    if (strcmp(c.display, display)) { fprintf(stderr, "calc sci '%s': got '%s', want '%s'\n", input, c.display, display); abort(); }
}
/* Scientific engine: the Windows 98 Scientific Calculator behaviours. */
static void scientific_engine(void)
{
    sexpect("2+3*4=", "14.");              /* operator precedence, unlike Standard */
    sexpect("2+3*4y2=", "50.");            /* x^y binds tighter than * */
    sexpect("2*(3+4)=", "14.");
    sexpect("(2+3)*(4+1)=", "25.");
    sexpect("2*3==", "18.");               /* = repeats */
    sexpect("2y3=", "8.");
    sexpect("27iy3=", "3.");               /* Inv x^y is the y-th root */
    sexpect("30s", "0.5");                 /* degrees */
    sexpect("60o", "0.5");
    sexpect("45t", "1.");
    sexpect("0.5is", "30.");               /* Inv sin */
    sexpect("1it", "45.");                 /* Inv tan */
    sexpect("2is", "Invalid input for function.");
    sexpect("1lis", "0.");                 /* log 1 = 0; asin 0 */
    sexpect("1000l", "3.");
    sexpect("3il", "1000.");               /* Inv log = 10^x */
    sexpect("1in", "2.71828182845905");    /* Inv ln = e^x */
    sexpect("0l", "Invalid input for function.");
    sexpect("0n", "Invalid input for function.");
    sexpect("5!", "120.");
    sexpect("0!", "1.");
    sexpect("171!", "Result of function is undefined.");
    sexpect("12q", "144.");
    sexpect("144iq", "12.");
    sexpect("3#", "27.");
    sexpect("27i#", "3.");
    sexpect("p", "3.14159265358979");
    sexpect("ip", "6.28318530717959");
    sexpect("10%3=", "1.");
    sexpect("255&15=", "15.");
    sexpect("12|3=", "15.");
    sexpect("12^10=", "6.");
    sexpect("1<4=", "16.");
    sexpect("256i<4=", "16.");             /* Inv Lsh = Rsh */
    sexpect("0~", "-1.");
    sexpect("3.7;", "3.");
    sexpect("3.75i;", "0.75");
    sexpect("10.5m", "10.3");              /* d.mmss */
    sexpect("10.3im", "10.5");
    sexpect("123v", "1.23e+2");            /* F-E */
    sexpect("1.5x3", "1.5e+3");            /* Exp: the exponent is typed */
    sexpect("1.5x3+0=", "1500.");
    sexpect("1x", "1.e+");
    sexpect("5/0=", "Cannot divide by zero.");
    sexpect("1i", "1.");                   /* Inv alone changes nothing */
    sexpect("2@", "2.");                   /* @ is not a key in Scientific */
    /* radians and grads */
    calc_init(&c); calc_scientific(&c, 1); calc_key(&c, 0x3d, 0, 0); assert(c.angle == CALC_RAD);
    calc_key(&c, 0, 'p', 0); calc_key(&c, 0, 's', 0); assert(!strcmp(c.display, "0."));   /* sin(pi) is 0, not 1.2e-16 */
    calc_key(&c, 0x3e, 0, 0); assert(c.angle == CALC_GRAD);
    calc_clear(&c); assert(c.angle == CALC_GRAD && c.scientific);                     /* C keeps the unit */
    calc_digit(&c, 5); calc_digit(&c, 0); calc_sin(&c); assert(!strcmp(c.display, "0.707106781186547"));
    calc_key(&c, 0x3c, 0, 0); assert(c.angle == CALC_DEG);
    /* hyperbolic */
    sci("0"); calc_toggle_hyp(&c); calc_cos(&c); assert(!strcmp(c.display, "1.") && !c.hyp);
    sci("1"); calc_toggle_hyp(&c); calc_sin(&c); assert(!strcmp(c.display, "1.1752011936438"));
    sci("1"); calc_toggle_hyp(&c); calc_toggle_inv(&c); calc_cos(&c); assert(!strcmp(c.display, "0."));
    sci("0.5"); calc_toggle_hyp(&c); calc_toggle_inv(&c); calc_cos(&c); assert(c.error);
    /* parentheses */
    sci("2*(3"); assert(calc_paren_depth(&c) == 1);
    calc_close_paren(&c); assert(!calc_paren_depth(&c) && !strcmp(c.display, "3."));
    sci("((("); assert(calc_paren_depth(&c) == 3);
    calc_equals(&c); assert(!calc_paren_depth(&c));          /* = closes them */
    sci(")"); assert(!calc_paren_depth(&c));
    sci("2*"); for (int i = 0; i < CALC_STACK + 2 && !c.error; i++) calc_open_paren(&c);
    assert(c.error);                                         /* 25 levels */
    /* bases: integers, two's complement, digit limits */
    calc_init(&c); calc_scientific(&c, 1);
    calc_key(&c, 0x3f, 0, 0); assert(c.base == 16);
    calc_key(&c, 0, 'f', 0); calc_key(&c, 0, 'F', 0); assert(!strcmp(c.display, "FF"));
    calc_key(&c, 0x40, 0, 0); assert(c.base == 10 && !strcmp(c.display, "255."));
    calc_key(&c, 0x42, 0, 0); assert(!strcmp(c.display, "11111111"));
    calc_digit(&c, 2); assert(!c.entering && !strcmp(c.display, "11111111"));   /* 2 is not a binary digit */
    calc_key(&c, 0x41, 0, 0); assert(!strcmp(c.display, "377"));
    calc_key(&c, 0x3f, 0, 0); assert(!strcmp(c.display, "FF"));
    calc_clear(&c); assert(c.base == 16);
    calc_key(&c, 0, '1', 0); calc_key(&c, 0, '-', 0); calc_key(&c, 0, '2', 0); calc_key(&c, 0, '=', 0);
    assert(!strcmp(c.display, "FFFFFFFFFFFFFFFF"));
    calc_set_base(&c, 10); assert(!strcmp(c.display, "-1."));
    sci("3.9"); calc_set_base(&c, 16); assert(!strcmp(c.display, "3"));          /* fractions are dropped */
    calc_point(&c); assert(!strcmp(c.display, "3"));
    calc_set_base(&c, 2); calc_digit(&c, 2); assert(!strcmp(c.display, "11"));
    calc_clear(&c); for (int i = 0; i < 70; i++) calc_digit(&c, 1);
    assert(strlen(c.entry) == 64);
    calc_set_base(&c, 8); calc_clear(&c); for (int i = 0; i < 30; i++) calc_digit(&c, 7);
    assert(strlen(c.entry) == 22);
    calc_set_base(&c, 10); calc_toggle_fe(&c); assert(c.fe);
    calc_set_base(&c, 16); calc_toggle_fe(&c); assert(c.fe);                    /* F-E does not apply in Hex */
    /* statistics */
    sci("2"); calc_stat_data(&c); calc_digit(&c, 4); calc_stat_data(&c); calc_digit(&c, 6); calc_stat_data(&c);
    assert(c.nstat == 3);
    calc_stat_ave(&c); assert(!strcmp(c.display, "4."));
    calc_stat_sum(&c); assert(!strcmp(c.display, "12."));
    calc_stat_dev(&c); assert(!strcmp(c.display, "2."));                         /* sample deviation */
    calc_toggle_inv(&c); calc_stat_dev(&c); assert(!strcmp(c.display, "1.63299316185545"));   /* population */
    calc_stat_sum(&c); assert(!strcmp(c.display, "12."));
    calc_toggle_inv(&c); calc_stat_sum(&c); assert(!strcmp(c.display, "56."));                   /* sum of squares */
    calc_stat_clear(&c); calc_stat_ave(&c); assert(c.error);
    /* switching the view keeps the number; leaving Scientific returns to Dec and immediate execution */
    sci("7"); calc_scientific(&c, 0); assert(!c.scientific && !strcmp(c.display, "7."));
    for (const char *k = "+2*3="; *k; k++) calc_key(&c, 0, (unsigned char)*k, 0);
    assert(!strcmp(c.display, "27."));
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
    scientific_engine();

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

    /* View > Scientific rebuilds the keys; radios, check boxes and the A-F slots follow the engine. */
    calc_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = CALC_M_SCIENTIFIC}, &app);
    du_render(&ui);
    assert(app.calc.scientific && (app.view_items[1].flags & DU_ITEM_CHECKED));
    assert(ui.count <= DU_WIDGETS && du_get(&ui, calc_sci_button_id("Backspace")));
    at_save(folder, "calc-scientific", &ui);
    at_click_widget(&ui, calc_sci_button_id("2")); at_click_widget(&ui, calc_sci_button_id("x^y"));
    at_click_widget(&ui, calc_sci_button_id("1")); at_click_widget(&ui, calc_sci_button_id("0")); at_click_widget(&ui, calc_sci_button_id("="));
    assert(!strcmp(app.calc.display, "1024."));
    at_click_widget(&ui, calc_sci_button_id("C"));                                  /* Clear (the first "C") */
    at_click_widget(&ui, CALC_C_INV);
    assert(app.calc.inv && du_get(&ui, CALC_C_INV)->value);
    at_click_widget(&ui, calc_sci_button_id("9")); at_click_widget(&ui, calc_sci_button_id("x^2"));   /* Inv x^2 = sqrt */
    assert(!strcmp(app.calc.display, "3.") && !app.calc.inv && !du_get(&ui, CALC_C_INV)->value);
    at_click_widget(&ui, CALC_R_RAD); assert(app.calc.angle == CALC_RAD && du_get(&ui, CALC_R_RAD)->value && !du_get(&ui, CALC_R_DEG)->value);
    at_click_widget(&ui, CALC_R_DEG); assert(app.calc.angle == CALC_DEG);
    at_click_widget(&ui, CALC_R_HEX);
    assert(app.calc.base == 16 && !du_get(&ui, CALC_R_DEC)->value);
    int sta = calc_sci_button_id("Sta");
    assert(!du_get(&ui, sta)->hidden && !strcmp(du_get(&ui, sta)->text, "A") && !du_get(&ui, sta + 5)->hidden);   /* A-F */
    at_click_widget(&ui, calc_sci_button_id("C")); at_click_widget(&ui, sta + 5); at_click_widget(&ui, sta + 1);   /* F then B */
    assert(!strcmp(app.calc.display, "FB"));
    at_click_widget(&ui, CALC_R_BIN);
    assert(du_get(&ui, calc_sci_button_id("2"))->disabled && du_get(&ui, sta + 5)->hidden && du_get(&ui, calc_sci_button_id("sin"))->disabled);
    at_click_widget(&ui, calc_sci_button_id("C")); at_click_widget(&ui, calc_sci_button_id("1")); at_click_widget(&ui, calc_sci_button_id("0"));
    at_click_widget(&ui, calc_sci_button_id("2")); assert(!strcmp(app.calc.display, "10"));   /* disabled key: ignored */
    du_render(&ui);
    at_save(folder, "calc-scientific-bin", &ui);
    calc_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = CALC_M_STANDARD}, &app);
    assert(!app.calc.scientific && app.calc.base == 10 && du_get(&ui, calc_button_id("7")));
    assert(!du_get(&ui, CALC_R_HEX));
    calc_view_set(&app,1);calc_stat_clear(&app.calc);
    calc_digit(&app.calc,2);calc_stat_data(&app.calc);
    calc_digit(&app.calc,4);calc_stat_data(&app.calc);
    calc_digit(&app.calc,6);calc_stat_data(&app.calc);
    at_click_widget(&ui,calc_sci_button_id("Sta"));
    assert(du_dialog_id(&ui)==CALC_D_STAT && app.calc.nstat==3);
    assert(ui.dialog.items[4].count==3 && strstr(ui.dialog.items[4].items[1].text[0],"4"));
    ui.dialog.items[4].active=1;
    struct du_rect cd=ui.dialog.items[6].box;
    at_click(&ui,cd.x+10,cd.y+10);
    assert(app.calc.nstat==2 && app.calc.stat[0]==2 && app.calc.stat[1]==6);
    struct du_rect load=ui.dialog.items[5].box;
    at_click(&ui,load.x+10,load.y+10);
    assert(!du_dialog_id(&ui) && calc_current(&app.calc)==6);
    at_click_widget(&ui,calc_sci_button_id("Sta"));
    struct du_rect cad=ui.dialog.items[7].box;
    at_click(&ui,cad.x+10,cad.y+10);
    assert(!app.calc.nstat && ui.dialog.items[5].disabled && ui.dialog.items[6].disabled);
    du_dialog_close(&ui);calc_stat_data(&app.calc);
    at_click_widget(&ui,calc_sci_button_id("Sta"));
    struct du_rect ret=ui.dialog.items[0].box;
    at_click(&ui,ret.x+10,ret.y+10);
    assert(!du_dialog_id(&ui) && app.calc.nstat==1);
    puts("calc: PASS");
    return 0;
}
