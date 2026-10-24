/* G13 (ER-2): the fatal-error screen text and its restart choice.
 * SPDX-License-Identifier: GPL-2.0-only */
#define FATAL_HOST 1
#include "../../src/kernel/core/fatal.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static enum fatal_choice feed(struct fatal_keys *k, const uint8_t *bytes, size_t n)
{
    enum fatal_choice last = FATAL_NONE;
    for (size_t i = 0; i < n; i++) {
        enum fatal_choice c = fatal_key_feed(k, bytes[i]);
        if (c != FATAL_NONE) last = c;
    }
    return last;
}
#define FEED(k, ...) feed(k, (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }))

int main(void)
{
    char text[FATAL_SCREEN_BYTES];
    /* A page fault: Windows 98 wording with the cs:eip of the fault. */
    struct fatal_report pf = { .frame = true, .vector = 14, .error = 2, .cs = 8, .eip = 0xC0011E36, .cr2 = 0x10, .task = "desktop" };
    size_t n = fatal_format(&pf, text, sizeof(text));
    CHECK(n == strlen(text) && n < sizeof(text));
    CHECK(strstr(text, "A fatal exception 0E has occurred at 0008:C0011E36 in task \"desktop\"."));
    CHECK(strstr(text, "page fault: write of unmapped address 00000010 (error code 00000002)."));
    CHECK(strstr(text, "The system cannot continue."));
    CHECK(strstr(text, "Press ENTER or CTRL+ALT+DEL to restart your computer."));
    CHECK(strstr(text, "Press ESC to keep this screen"));
    for (const char *line = text; *line;) {            /* every line fits 80 columns */
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        CHECK(len <= 80);
        line += len + (end ? 1 : 0);
    }
    pf.error = 3; pf.vector = 13;
    fatal_format(&pf, text, sizeof(text));
    CHECK(strstr(text, "exception 0D") && strstr(text, "general protection fault: selector 0003 (error code 00000003)."));
    pf.vector = 14; pf.error = 1;
    fatal_format(&pf, text, sizeof(text));
    CHECK(strstr(text, "read of protected address"));
    /* A panic without a frame: the reason is clipped and made printable. */
    struct fatal_report why = { .frame = false, .task = NULL, .reason = "out of memory\x01 while mapping" };
    fatal_format(&why, text, sizeof(text));
    CHECK(strstr(text, "A fatal error has occurred in task \"boot\":"));
    CHECK(strstr(text, "out of memory? while mapping"));
    char longest[400]; memset(longest, 'x', sizeof(longest) - 1); longest[sizeof(longest) - 1] = 0;
    why.reason = longest; why.task = longest;
    n = fatal_format(&why, text, sizeof(text));
    CHECK(n < sizeof(text) && strlen(text) == n && strstr(text, "ESC"));
    n = fatal_format(&why, text, 40);                  /* truncation stays terminated */
    CHECK(n <= 39 && strlen(text) == n);
    CHECK(fatal_format(&why, text, 0) == 0);
    /* Keys: set 2 (the kernel driver's untranslated mode). */
    struct fatal_keys k = { 0 };
    CHECK(FEED(&k, 0x5a) == FATAL_RESTART);           /* Enter */
    CHECK(FEED(&k, 0xf0, 0x5a) == FATAL_NONE);        /* Enter released */
    CHECK(FEED(&k, 0x76) == FATAL_STAY);              /* Esc */
    CHECK(FEED(&k, 0xf0, 0x76) == FATAL_NONE);
    CHECK(FEED(&k, 0xe0, 0x71) == FATAL_NONE);        /* Delete alone */
    CHECK(FEED(&k, 0x14, 0xe0, 0x71) == FATAL_NONE);  /* Ctrl+Delete */
    CHECK(FEED(&k, 0x11, 0xe0, 0x71) == FATAL_RESTART); /* Ctrl+Alt+Delete */
    CHECK(FEED(&k, 0xe0, 0xf0, 0x71, 0xf0, 0x11, 0xf0, 0x14) == FATAL_NONE);
    CHECK(!k.ctrl && !k.alt);
    CHECK(FEED(&k, 0x71) == FATAL_NONE);              /* keypad '.', not Delete */
    CHECK(FEED(&k, 0x14, 0x11, 0x71) == FATAL_NONE);
    CHECK(FEED(&k, 0xe0, 0x14, 0xe0, 0x11, 0xe0, 0x71) == FATAL_RESTART); /* right Ctrl and Alt */
    CHECK(FEED(&k, 0xe0, 0x76) == FATAL_NONE && FEED(&k, 0x1c) == FATAL_NONE); /* neither Esc nor Enter */
    struct fatal_keys fresh = { 0 };
    CHECK(FEED(&fresh, 0xf0, 0x14) == FATAL_NONE && !fresh.ctrl);
    CHECK(FEED(&fresh, 0xe0, 0x5a) == FATAL_RESTART); /* keypad Enter */
    /* Translated (set 1) controller. */
    struct fatal_keys t = { .translated = true };
    CHECK(FEED(&t, 0x1c) == FATAL_RESTART && FEED(&t, 0x9c) == FATAL_NONE);
    CHECK(FEED(&t, 0x01) == FATAL_STAY);
    CHECK(FEED(&t, 0x1d, 0x38, 0xe0, 0x53) == FATAL_RESTART);
    CHECK(FEED(&t, 0xe0, 0xd3, 0xb8, 0x9d) == FATAL_NONE && !t.ctrl && !t.alt);
    CHECK(FEED(&t, 0xe0, 0x53) == FATAL_NONE);
    puts("PASS fatal screen: Windows 98 text, clipping, set 1 and set 2 restart choice");
    return 0;
}
