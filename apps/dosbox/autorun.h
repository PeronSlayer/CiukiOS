/* /Programs/dosbox COMMAND: what the window types at the DOS prompt once its VM
 * is running and the window has the input focus (owner decision 2026-10-12:
 * Programs > Games starts "dosbox --name=DOOM \DOOM\DOOM.EXE -config CIUKI.CFG").
 * The guest command still uses keyboard input: the program types
 * through its own window input as the user would, with the US layout the
 * VM's BIOS keyboard decodes (CIUKI_KEY_* codes are the set-1 make codes).
 * A leading "\DIR\PROGRAM" becomes two lines, "CD \DIR" and "PROGRAM [args]",
 * so the game finds its data files in its own directory; anything else is
 * typed as one line. A drive-qualified path adds a drive switch, then CD
 * and the program (F2-51 removable media). Pure functions, no OS calls (host test:
 * tests/host/desktop/dosbox_autorun_test.c).
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSBOX_AUTORUN_H
#define CIUKI_DOSBOX_AUTORUN_H
#include <ciuki/abi.h>
#include <stdint.h>
#include <string.h>
#define DOSBOX_COMMAND_MAX 96u      /* well below DOS's 127-byte command tail */
#define DOSBOX_LINES_MAX 3u
#define DOSBOX_KEYS_MAX (4u * (DOSBOX_COMMAND_MAX + 16u))
/* One key event. group: index of the first event of its character (a
 * shifted character is Shift down, key down, key up, Shift up); main: the
 * event that types it. line_end: Enter's release, after which the typist
 * pauses so the shell can run the line. */
struct dosbox_key { uint16_t code; uint8_t down, line_end; uint16_t group, main; };

/* US set-1 key for one printable ASCII character; *shift set when Shift is needed. 0: not typable. */
static inline uint16_t dosbox_key_for(char c, int *shift)
{
    static const char letters[] = "qwertyuiop" "asdfghjkl" "zxcvbnm";
    static const uint8_t letter_codes[] = {CIUKI_KEY_Q, CIUKI_KEY_W, CIUKI_KEY_E, CIUKI_KEY_R, CIUKI_KEY_T, CIUKI_KEY_Y,
        CIUKI_KEY_U, CIUKI_KEY_I, CIUKI_KEY_O, CIUKI_KEY_P, CIUKI_KEY_A, CIUKI_KEY_S, CIUKI_KEY_D, CIUKI_KEY_F,
        CIUKI_KEY_G, CIUKI_KEY_H, CIUKI_KEY_J, CIUKI_KEY_K, CIUKI_KEY_L, CIUKI_KEY_Z, CIUKI_KEY_X, CIUKI_KEY_C,
        CIUKI_KEY_V, CIUKI_KEY_B, CIUKI_KEY_N, CIUKI_KEY_M};
    *shift = 0;
    if (c >= 'A' && c <= 'Z') { *shift = 1; c = (char)(c - 'A' + 'a'); }
    if (c >= 'a' && c <= 'z') return letter_codes[strchr(letters, c) - letters];
    if (c >= '1' && c <= '9') return (uint16_t)(CIUKI_KEY_1 + (c - '1'));
    switch (c) {
    case '0': return CIUKI_KEY_0;
    case ' ': return CIUKI_KEY_SPACE;
    case '\\': return CIUKI_KEY_BACKSLASH;
    case '.': return CIUKI_KEY_PERIOD;
    case '-': return CIUKI_KEY_MINUS;
    case '_': *shift = 1; return CIUKI_KEY_MINUS;
    case ':': *shift = 1; return CIUKI_KEY_SEMICOLON;
    case '/': return CIUKI_KEY_SLASH;
    case '=': return CIUKI_KEY_EQUAL;
    case ',': return CIUKI_KEY_COMMA;
    default: return 0;
    }
}

/* Splits the command into at most three lines (see above). 0 or the line
 * count; -1 for an empty, too long or untypable command. */
static inline int dosbox_command_lines(const char *command, char lines[DOSBOX_LINES_MAX][DOSBOX_COMMAND_MAX + 4])
{
    size_t n = strlen(command);
    if (!n || n > DOSBOX_COMMAND_MAX || command[0] == ' ' || command[n - 1] == ' ') return -1;
    for (size_t i = 0; i < n; i++) { int shift; if (!dosbox_key_for(command[i], &shift)) return -1; }
    /* A host media launch explicitly switches drive before changing directory. */
    if (n>3 && command[1]==':' && command[2]=='\\' &&
        ((command[0]>='A' && command[0]<='Z') || (command[0]>='a' && command[0]<='z'))) {
        lines[0][0]=command[0]; lines[0][1]=':'; lines[0][2]=0;
        size_t end=strcspn(command," "), last=2;
        for (size_t i=3;i<end;i++) if (command[i]=='\\') last=i;
        if (last+1>=end) return -1;
        memcpy(lines[1],"CD ",3);
        size_t dir=last==2 ? 1 : last-2;
        memcpy(lines[1]+3,command+2,dir); lines[1][3+dir]=0;
        memcpy(lines[2],command+last+1,n-last); return 3;
    }
    size_t first = strcspn(command, " ");               /* the program token */
    size_t cut = 0;
    for (size_t i = 1; i < first; i++) if (command[i] == '\\') cut = i;
    if (command[0] == '\\' && cut > 1 && cut + 1 < first) {
        memcpy(lines[0], "CD ", 3); memcpy(lines[0] + 3, command, cut); lines[0][3 + cut] = 0;
        memcpy(lines[1], command + cut + 1, n - cut - 1); lines[1][n - cut - 1] = 0;
        return 2;
    }
    memcpy(lines[0], command, n + 1);
    return 1;
}

/* The key events typing the command's lines, each ended by Enter. Count, or -1. */
static inline int dosbox_command_keys(const char *command, struct dosbox_key *keys, unsigned max)
{
    char lines[DOSBOX_LINES_MAX][DOSBOX_COMMAND_MAX + 4];
    int count = dosbox_command_lines(command, lines);
    if (count < 0) return -1;
    unsigned at = 0;
    for (int l = 0; l < count; l++) {
        for (const char *p = lines[l]; ; p++) {
            int shift = 0;
            uint16_t code = *p ? dosbox_key_for(*p, &shift) : CIUKI_KEY_ENTER;
            if (at + 4 > max) return -1;
            uint16_t group = (uint16_t)at, main = (uint16_t)(at + (shift ? 1 : 0));
            if (shift) keys[at++] = (struct dosbox_key){CIUKI_KEY_LEFTSHIFT, 1, 0, group, main};
            keys[at++] = (struct dosbox_key){code, 1, 0, group, main};
            keys[at++] = (struct dosbox_key){code, 0, (uint8_t)!*p, group, main};
            if (shift) keys[at++] = (struct dosbox_key){CIUKI_KEY_LEFTSHIFT, 0, 0, group, main};
            if (!*p) break;
        }
    }
    return (int)at;
}

/* Command launches clear boot text using the DOS shell's own CLS command;
 * no guest RAM or register is rewritten by the desktop client. */
static inline int dosbox_launch_keys(const char *command, struct dosbox_key *keys, unsigned max)
{
    int prefix = dosbox_command_keys("CLS", keys, max);
    if (prefix < 0) return -1;
    int count = dosbox_command_keys(command, keys + prefix, max - (unsigned)prefix);
    if (count < 0) return -1;
    for (int i = 0; i < count; i++) {
        keys[prefix + i].group += (uint16_t)prefix;
        keys[prefix + i].main += (uint16_t)prefix;
    }
    return prefix + count;
}

/* Where to resume after the window lost the focus at event `failed`: the
 * kernel then released every held key. Not yet typed: the character again
 * from its first event (Shift included); typed (its key down delivered):
 * the next character, so nothing is typed twice. Enter's release is the
 * line end, kept so the typist still pauses. */
static inline unsigned dosbox_resume(const struct dosbox_key *keys, unsigned count, unsigned failed)
{
    if (failed >= count) return count;
    const struct dosbox_key *k = &keys[failed];
    if (failed <= k->main) return k->group;
    unsigned next = failed;
    while (next < count && keys[next].group == k->group && !keys[next].line_end) next++;
    return next;
}
#endif
