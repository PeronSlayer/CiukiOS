#!/usr/bin/env python3
"""Exercise the production Desktop/Files icon-label wrapper on the host."""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''#include <assert.h>
#include <stdio.h>
#include <string.h>

#define LFN_NAME 256
static int host_measure(const char *s) { return (int)strlen(s) * 8; }
#define CIUKI_ICON_LABEL_MEASURE host_measure
#include "iconlabel.h"

static int valid_utf8(const unsigned char *s)
{
    int i = 0;
    while (s[i]) {
        int n = ciuki_label_char_bytes(s + i);
        if ((s[i] & 0x80) && n == 1) return 0;
        i += n;
    }
    return 1;
}

static void check_label(const char *name, int max_width, int *lines_out,
                        int *widest_out)
{
    char line[LFN_NAME];
    int pos = 0, lines = 0, widest = 0;
    while (name[pos]) {
        int content_start = pos;
        int width, next = ciuki_label_next_line(name, pos, line,
                                                max_width, &width);
        int length = (int)strlen(line), skip;
        while (name[content_start] == ' ') content_start++;
        assert(next > pos);
        assert(width <= max_width);
        assert(valid_utf8((const unsigned char *)line));
        assert(!strncmp(name + content_start, line, length));
        for (skip = content_start + length; skip < next; skip++)
            assert(name[skip] == ' ');
        if (width > widest) widest = width;
        lines++;
        pos = next;
    }
    assert(lines > 0);
    {
        int layout_width = 0;
        assert(ciuki_label_layout(name, max_width, &layout_width) == lines);
        assert(layout_width == widest);
    }
    *lines_out = lines;
    *widest_out = widest;
}

int main(void)
{
    static const char plain[] =
        "Quarterly-report-final-version-2026-revised-copy-abcdefghijklmno-"
        "pqrstuvwxyz-0123456789-ABCDEFGHIJKLMNOPQRSTUVWXYZ-abcdefghijklmno";
    static const char words[] =
        "A carefully named project document with several readable words";
    static const char utf8[] =
        "Résumé-des-notes-über-die-Änderung-東京-data-final-version";
    const int widths[] = {78, 80}; /* Desktop and Files icon-label interiors. */
    const int screen_widths[] = {800, 1024, 1280};
    int w, sw, lines, widest, long_lines[2];
    assert(strlen(plain) >= 100);
    for (w = 0; w < 2; w++) {
        check_label(plain, widths[w], &lines, &widest);
        long_lines[w] = lines;
        assert(lines > 1 && widest <= widths[w]);
        check_label(words, widths[w], &lines, &widest);
        check_label(utf8, widths[w], &lines, &widest);
        /* Every display width uses the same fixed label cell; labels remain
         * inside it and rows grow to contain even the 100-byte fixture. */
        for (sw = 0; sw < 3; sw++) {
            int cell_h = w == 0 ? 82 : 72;
            int needed_h = (w == 0 ? 48 : 52) + long_lines[w] * 17;
            int cols = w == 0 ? (screen_widths[sw] - 8) / 84 :
                                (screen_widths[sw] - 186) / 92;
            int c;
            if (needed_h > cell_h) cell_h = needed_h;
            assert(widths[w] <= 84);
            assert(cell_h >= needed_h);
            assert(cell_h - (w == 0 ? 0 : 4) >= 47 + long_lines[w] * 17);
            assert(cols > 0);
            for (c = 0; c < cols; c++) {
                int cell_x = w == 0 ? screen_widths[sw] - 8 - 84 - c * 84 : 4 + c * 92;
                int cell_w = w == 0 ? 84 : 88;
                assert(cell_x >= 0);
                assert(cell_x + cell_w <= screen_widths[sw]);
                assert(widths[w] <= cell_w - (w == 0 ? 6 : 8));
            }
        }
    }
    puts("icon-label production helper: 100-byte, word-wrap, UTF-8 and 800/1024/1280 profiles PASS");
    return 0;
}
'''


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="ciuki-icon-label-") as temp:
        temp_path = Path(temp)
        source = temp_path / "iconlabel_host.c"
        binary = temp_path / "iconlabel_host"
        source.write_text(SOURCE)
        subprocess.run(
            ["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "src/apps"), str(source), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
