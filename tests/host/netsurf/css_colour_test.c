/* SPDX-License-Identifier: MIT
 * Parse through libcss's actual lexer/declaration pipeline; inspect its color
 * bytecode so a dropped declaration cannot pass as a rendered fallback. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <libcss/stylesheet.h>
#include "stylesheet.h"
#include "bytecode/opcodes.h"

static css_error resolve(void *pw, const char *base, lwc_string *relative,
        lwc_string **absolute)
{
    (void)pw;
    (void)base;
    *absolute = lwc_string_ref(relative);
    return CSS_OK;
}

static void check(const char *value, uint32_t expected, bool valid,
        bool quirks, size_t chunk)
{
    css_stylesheet_params params = {
        .params_version = CSS_STYLESHEET_PARAMS_VERSION_1,
        .level = CSS_LEVEL_21,
        .charset = "UTF-8",
        .url = "file:modern-colours",
        .inline_style = true,
        .allow_quirks = quirks,
        .resolve = resolve
    };
    char text[1024];
    int length = snprintf(text, sizeof(text),
            "color:#123456;color:%s; background-color:#abcdef;", value);
    assert(length > 0 && (size_t)length < sizeof(text));
    css_stylesheet *sheet = NULL;
    assert(css_stylesheet_create(&params, &sheet) == CSS_OK);
    for (size_t at = 0; at < (size_t)length; at += chunk) {
        size_t count = (size_t)length - at;
        if (count > chunk) count = chunk;
        css_error error = css_stylesheet_append_data(sheet,
                (const uint8_t *)text + at, count);
        assert(error == CSS_OK || error == CSS_NEEDDATA);
    }
    assert(css_stylesheet_data_done(sheet) == CSS_OK);
    assert(sheet->rule_list && sheet->rule_list->type == CSS_RULE_SELECTOR);
    css_style *style = ((css_rule_selector *)sheet->rule_list)->style;
    unsigned used = valid ? 6 : 4;
    if (!style || style->used != used ||
            (valid && style->bytecode[3] != expected)) {
        fprintf(stderr, "color %s: valid=%d used=%u expected=%u value=%08x expected=%08x\n",
                value, valid, style ? style->used : 0, used,
                style && style->used >= 4 ? style->bytecode[3] : 0, expected);
        fprintf(stderr, "quirks=%d chunk=%zu bytecode:", quirks, chunk);
        if (style) for (unsigned i = 0; i < style->used; i++)
            fprintf(stderr, " %08x", style->bytecode[i]);
        fputc('\n', stderr);
        assert(0);
    }
    assert(style->bytecode[0] == buildOPV(CSS_PROP_COLOR, 0, COLOR_SET));
    assert(style->bytecode[1] == 0xff123456u);
    if (valid)
        assert(style->bytecode[2] == buildOPV(CSS_PROP_COLOR, 0, COLOR_SET));
    assert(style->bytecode[used - 2] ==
            buildOPV(CSS_PROP_BACKGROUND_COLOR, 0, BACKGROUND_COLOR_SET));
    assert(style->bytecode[used - 1] == 0xffabcdefu); /* declaration recovery */
    assert(css_stylesheet_destroy(sheet) == CSS_OK);
}

int main(void)
{
    static const struct { const char *text; uint32_t value; } valid[] = {
        { "color-mix(in srgb,red,blue)", 0xff800080 },
        { "color-mix(in srgb,red 25%,blue)", 0xff4000bf },
        { "color-mix(in srgb,25% red,75% blue)", 0xff4000bf },
        { "color-mix(in srgb,red 80%,blue 80%)", 0xff800080 },
        { "color-mix(in srgb,red 25%,blue 25%)", 0x80800080 },
        { "color-mix(in srgb,#ff000080 50%,blue)", 0xc05500aa },
        { "color-mix(in srgb,red 42%,transparent)", 0x6bff0000 },
        { "color-mix(in srgb,transparent,blue)", 0x800000ff },
        { "color-mix(in srgb,red 0%,blue 0%)", 0x00000000 },
        { "color-mix(in srgb,color-mix(in srgb,red,blue),white)", 0xffc080c0 },
        { "#abc", 0xffaabbcc }, { "#aBcD", 0xddaabbcc },
        { "#010203", 0xff010203 }, { "#01020304", 0x04010203 },
        { "#f000", 0x00ff0000 }, { "#000000ff", 0xff000000 },
        { "rgb(1 2 3)", 0xff010203 }, { "rgba(1 2 3)", 0xff010203 },
        { "RGB(1,2,3)", 0xff010203 }, { "rgba(1,2,3)", 0xff010203 },
        { "rgb(1,2,3,.5)", 0x7f010203 },
        { "rgba(255,0,0,.5)", 0x7fff0000 },
        { "rgba(100%,0%,0%,.5)", 0x7fff0000 },
        { "rgba(255,0,0,50%)", 0x7fff0000 },
        { "rgba(100%,0%,0%,50%)", 0x7fff0000 },
        { "rgb(255 0 0 / .5)", 0x80ff0000 },
        { "rgba(100% 0% 0% / 50%)", 0x80ff0000 },
        { "rgb(100% 2 0% / 100%)", 0xffff0200 },
        { "rgb(50% 50% 50%)", 0xff808080 },
        { "rgb(1.5 2.5 3.5)", 0xff020304 },
        { "rgb(1.5,2.5,3.5)", 0xff010203 },
        { "rgb(-1000 1000 3 / -100)", 0x0000ff03 },
        { "rgb(99999999% -99999999% 0 / 99999999)", 0xffff0000 },
        { "rgb(99999999 -99999999 0 / 99999999%)", 0xffff0000 },
        { "rgb(none none NoNe / none)", 0 },
        { "rgb(none 100% 0)", 0xff00ff00 },
        { "rgb(1\n\t2 3 /\n 1)", 0xff010203 },
        { "light-dark(rgb(1 2 3),#0000)", 0xff010203 },
        { "light-dark(#abcd,rgba(1 2 3 / none))", 0xddaabbcc }
    };
    static const char *invalid[] = {
        "color-mix()", "color-mix(in srgb,red)",
        "color-mix(in srgb,red,blue,green)",
        "color-mix(in oklab,red,blue)", "color-mix(in srgb,currentColor,red)",
        "color-mix(in srgb,red -1%,blue)", "color-mix(in srgb,red 101%,blue)",
        "color-mix(in srgb,red 20% 20%,blue)", "color-mix(in srgb,20% red 20%,blue)",
        "color-mix(in srgb,red,,blue)", "color-mix(in srgb red,blue)",
        "color-mix(in srgb,red %,blue)",
        "#12", "#12345", "#1234567", "#123456789", "#abcdexff", "#gggg",
        "rgb()", "rgb(1 2)", "rgb(1 2 3 4)", "rgb(1 2 3 /)",
        "rgb(1,2 3)", "rgb(1 2,3)", "rgb(1,2,3 / .5)",
        "rgb(1 2 3,.5)", "rgb(1,2,3,4,5)", "rgb(1 2 3 / .5 / .5)",
        "rgb(1%,2,3%)", "rgb(none,0,0)", "rgb(0,none,0)",
        "rgb(0,0,0,none)", "rgb(auto 0 0)", "rgb(1px 0 0)",
        "rgb(1 2 3 / 1px)", "rgb(1 2 3 / red)", "rgb(1 2 3) junk"
    };
    unsigned count = 0;
    for (size_t chunk = 1; chunk <= 1024; chunk *= 1024) {
        for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
            check(valid[i].text, valid[i].value, true, false, chunk);
            count++;
        }
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            check(invalid[i], 0, false, false, chunk);
            count++;
        }
        check("abcd", 0, false, true, chunk);
        check("12345678", 0, false, true, chunk);
        check("abc", 0xffaabbcc, true, true, chunk);
        check("123456", 0xff123456, true, true, chunk);
        count += 4;
    }
    printf("PASS modern CSS colors: %u lexer/declaration/stream/recovery checks\n", count);
    fflush(stdout);
    return 0;
}
