/* /Programs/dosbox COMMAND typing (apps/dosbox/autorun.h): line split, US set-1
 * key events decoded back by an independent US keyboard table, and the
 * resume point after a focus loss.
 * SPDX-License-Identifier: MIT */
#include "../../../apps/dosbox/autorun.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* IBM PC/AT US layout, scan code set 1 make codes (independent of autorun.h). */
static const char *const us_rows[][3] = {
    {"\x02", "1234567890-=", "!@#$%^&*()_+"},
    {"\x10", "qwertyuiop[]", "QWERTYUIOP{}"},
    {"\x1e", "asdfghjkl;'`", "ASDFGHJKL:\"~"},
    {"\x2b", "\\zxcvbnm,./", "|ZXCVBNM<>?"},
};
static char decode(unsigned code, int shift)
{
    if (code == 0x39) return ' ';
    for (unsigned r = 0; r < sizeof(us_rows) / sizeof(us_rows[0]); r++) {
        unsigned first = (unsigned char)us_rows[r][0][0], n = (unsigned)strlen(us_rows[r][1]);
        if (code >= first && code < first + n) return us_rows[r][shift ? 2 : 1][code - first];
    }
    return 0;
}
/* Types the events into a model keyboard: Shift state, a character per key
 * down, '\n' per Enter; checks every down has its up. */
static void replay(const struct dosbox_key *keys, int count, char *out, size_t size)
{
    int shift = 0, held[0x80] = {0};
    size_t at = 0;
    for (int i = 0; i < count; i++) {
        const struct dosbox_key *k = &keys[i];
        assert(k->code > 0 && k->code < 0x80 && k->down <= 1);
        assert(held[k->code] != k->down);
        held[k->code] = k->down;
        if (k->code == CIUKI_KEY_LEFTSHIFT) { shift = k->down; continue; }
        assert(k->line_end == (k->code == CIUKI_KEY_ENTER && !k->down));
        if (!k->down) continue;
        assert(at + 1 < size);
        out[at++] = k->code == CIUKI_KEY_ENTER ? '\n' : decode(k->code, shift);
        assert(out[at - 1]);
        assert((unsigned)i == k->main && k->group <= k->main);
    }
    for (unsigned c = 0; c < 0x80; c++) assert(!held[c]);
    out[at] = 0;
}
static void typed(const char *command, const char *expect)
{
    struct dosbox_key keys[DOSBOX_KEYS_MAX];
    char out[2 * DOSBOX_COMMAND_MAX];
    int n = dosbox_command_keys(command, keys, DOSBOX_KEYS_MAX);
    assert(n > 0);
    replay(keys, n, out, sizeof(out));
    if (strcmp(out, expect)) { fprintf(stderr, "'%s' typed '%s', expected '%s'\n", command, out, expect); assert(0); }
}

int main(void)
{
    /* The Programs > Games commands (apps/desktop/catalog.h desk_games). */
    typed("\\WOLF3D\\WOLF3D.EXE", "CD \\WOLF3D\nWOLF3D.EXE\n");
    typed("\\DOOM\\DOOM.EXE -config CIUKI.CFG", "CD \\DOOM\nDOOM.EXE -config CIUKI.CFG\n");
    /* One line: no directory before the program, or not a path. */
    typed("\\RUNREC.COM \\DOOM DOOM.EXE", "\\RUNREC.COM \\DOOM DOOM.EXE\n");
    typed("DIR /W", "DIR /W\n");
    typed("\\WOLF3D\\", "\\WOLF3D\\\n");
    typed("C:\\GAMES_1\\A=B,C.EXE x", "C:\nCD \\GAMES_1\nA=B,C.EXE x\n");
    typed("E:\\SETUP\\INSTALL.COM /S", "E:\nCD \\SETUP\nINSTALL.COM /S\n");
    typed("D:\\START.COM", "D:\nCD \\\nSTART.COM\n");
    typed("\\A\\B\\C.EXE 0123456789", "CD \\A\\B\nC.EXE 0123456789\n");
    /* Refused: empty, edge spaces, untypable characters, too long. */
    struct dosbox_key keys[DOSBOX_KEYS_MAX];
    static const char *const bad[] = {"", " DIR", "DIR ", "DOOM;", "A\tB", "\xc3\xa9", "A|B", "A>B"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) assert(dosbox_command_keys(bad[i], keys, DOSBOX_KEYS_MAX) == -1);
    char longest[DOSBOX_COMMAND_MAX + 2];
    memset(longest, 'A', sizeof(longest)); longest[DOSBOX_COMMAND_MAX] = 0;
    assert(dosbox_command_keys(longest, keys, DOSBOX_KEYS_MAX) > 0);
    longest[DOSBOX_COMMAND_MAX] = 'A'; longest[DOSBOX_COMMAND_MAX + 1] = 0;
    assert(dosbox_command_keys(longest, keys, DOSBOX_KEYS_MAX) == -1);
    assert(dosbox_command_keys("DIR", keys, 8) == -1);           /* the caller's buffer is too small */
    /* Resume after a focus loss at each event of "Ab": Shift, A down/up, Shift up | b down/up | Enter. */
    int n = dosbox_command_keys("Ab", keys, DOSBOX_KEYS_MAX);
    assert(n == 8 && keys[0].code == CIUKI_KEY_LEFTSHIFT && keys[1].code == CIUKI_KEY_A && keys[4].code == CIUKI_KEY_B);
    static const unsigned resume[] = {0, 0, 4, 4, 4, 6, 6, 7, 8};
    for (unsigned i = 0; i <= 8; i++) assert(dosbox_resume(keys, 8, i) == resume[i]);
    char launched[2 * DOSBOX_COMMAND_MAX];
    n = dosbox_launch_keys("\\DOOM\\DOOM.EXE -config CIUKI.CFG", keys, DOSBOX_KEYS_MAX);
    assert(n > 0); replay(keys, n, launched, sizeof(launched));
    assert(!strcmp(launched, "CLS\nCD \\DOOM\nDOOM.EXE -config CIUKI.CFG\n"));
    longest[DOSBOX_COMMAND_MAX] = 0;
    assert(dosbox_launch_keys(longest, keys, DOSBOX_KEYS_MAX) > 0);
    assert(dosbox_launch_keys("DOOM", keys, 8) == -1);
    puts("PASS dosbox autorun: \\DIR\\PROGRAM split into CD and the program, US set-1 events decode back, refusals, focus-loss resume");
    return 0;
}
