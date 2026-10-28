/* fx-exeicons: ciukishell's SHGetFileInfoA/ExtractIconA (sdk/win32/shell/icons.c)
 * over the Linux kernel32 adapter (host.c) with USER stand-ins that record
 * the icon locations the shell resolves. argv: scratch root, fixture folder
 * (icon_fixtures.py: a PE, an NE and a DOS program, a PIF).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include "host.h"
#include <w32/user32.h>
#pragma clang diagnostic pop
struct fileinfo { HICON icon; int index; DWORD attributes; char display[260], type[80]; };
uintptr_t W32API SHGetFileInfoA(const char *, DWORD, struct fileinfo *, UINT, UINT);
HICON W32API ExtractIconA(HINSTANCE, const char *, UINT);

/* USER stand-ins: PrivateExtractIcons finds icons in .ico/.exe/.dll/.icl
 * files that exist (icon handle 0x5000); CiukiShellIcon returns 0x100+kind. */
static char last_path[260];
static int last_index;
UINT W32API PrivateExtractIconsA(const char *path, int index, int cx, int cy, HICON *icons, UINT *ids, UINT n, UINT flags)
{
    (void)cx; (void)cy; (void)ids; (void)flags;
    snprintf(last_path, sizeof(last_path), "%s", path);
    last_index = index;
    if (GetFileAttributesA(path) == ~0u) return icons ? 0xffffffffu : 0;
    const char *dot = strrchr(path, '.');
    int iconic = dot && (!strcasecmp(dot, ".ico") || !strcasecmp(dot, ".exe") || !strcasecmp(dot, ".dll") || !strcasecmp(dot, ".icl"));
    if (!icons) return iconic ? 1 : 0;
    if (!iconic || !n) return 0;
    icons[0] = (HICON)(uintptr_t)0x5000;
    return 1;
}
HICON W32API CiukiShellIcon(UINT kind) { return (HICON)(uintptr_t)(0x100 + kind); }
BOOL W32API DestroyIcon(HICON h) { (void)h; return 1; }

static void copy_in(const char *fixtures, const char *name, const char *w32)
{
    char from[1200], to[1400];
    snprintf(from, sizeof(from), "%s/%s", fixtures, name);
    snprintf(to, sizeof(to), "%s/%c%s", host_root, w32[0], w32 + 2);
    for (char *p = to + strlen(host_root); *p; p++) if (*p == '\\') *p = '/';
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    assert(in && out);
    char chunk[4096]; size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), in))) assert(fwrite(chunk, 1, n, out) == n);
    fclose(in); assert(!fclose(out));
}
static void dir(const char *w32)
{
    char p[1400];
    snprintf(p, sizeof(p), "%s/%c%s", host_root, w32[0], w32 + 2);
    for (char *q = p + strlen(host_root); *q; q++) if (*q == '\\') *q = '/';
    assert(!mkdir(p, 0700));
}
static void readonly(const char *w32)
{
    char p[1400];
    snprintf(p, sizeof(p), "%s/%c%s", host_root, w32[0], w32 + 2);
    for (char *q = p + strlen(host_root); *q; q++) if (*q == '\\') *q = '/';
    assert(!chmod(p, 0500));
}
/* A CiukiOS CIUKLNK3 shortcut (apps/desktop/shell_link.h). */
static void link_file(const char *w32, const char *target, const char *icon, int index)
{
    const char *fields[5] = {"Link", target, "", "", icon};
    unsigned char b[1024] = "CIUKLNK3";
    unsigned at = 40;
    b[8] = 1; memcpy(b + 12, &index, 4);
    for (unsigned k = 0; k < 5; k++) {
        unsigned n = (unsigned)strlen(fields[k]);
        memcpy(b + 16 + 4 * k, &n, 4); memcpy(b + at, fields[k], n); at += n;
    }
    char p[1400];
    snprintf(p, sizeof(p), "%s/%c%s", host_root, w32[0], w32 + 2);
    for (char *q = p + strlen(host_root); *q; q++) if (*q == '\\') *q = '/';
    FILE *f = fopen(p, "wb"); assert(f && fwrite(b, 1, at, f) == at); assert(!fclose(f));
}
static void location(const char *path, const char *file, int index)
{
    struct fileinfo fi;
    assert(SHGetFileInfoA(path, 0, &fi, sizeof(fi), 0x1000));
    if (strcmp(fi.display, file) || fi.index != index) {
        fprintf(stderr, "%s: location %s,%d, expected %s,%d\n", path, fi.display, fi.index, file, index);
        abort();
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    host_init(argv[1]);
    const char *fixtures = argv[2];
    dir("C:\\SIERRA"); dir("C:\\SIERRA\\Half-Life"); dir("C:\\GAMES"); dir("C:\\Menu"); dir("C:\\Custom"); dir("C:\\Plain");
    copy_in(fixtures, "hl.exe", "C:\\SIERRA\\Half-Life\\hl.exe");
    copy_in(fixtures, "doom.exe", "C:\\GAMES\\DOOM.EXE");
    copy_in(fixtures, "doom.pif", "C:\\GAMES\\DOOM.PIF");
    copy_in(fixtures, "plain.pif", "C:\\GAMES\\PLAIN.PIF");
    copy_in(fixtures, "win16.exe", "C:\\GAMES\\WIN16.EXE");
    host_put("C:\\GAMES\\DOOM.ICO", "icon");
    host_put("C:\\GAMES\\notes.txt", "text");
    host_put("C:\\Custom\\desktop.ini", "[.ShellClassInfo]\r\nIconFile=folder.ico\r\nIconIndex=2\r\n");
    host_put("C:\\Custom\\folder.ico", "icon");
    host_put("C:\\Plain\\desktop.ini", "[.ShellClassInfo]\r\nIconFile=folder.ico\r\n");
    host_put("D:\\autorun.inf", "[autorun]\r\nopen=setup.exe\r\nicon=cd.ico,2\r\n");
    host_put("D:\\cd.ico", "icon");
    host_put("C:\\Menu\\Site.url", "[InternetShortcut]\r\nURL=http://x/\r\nIconFile=C:\\GAMES\\DOOM.ICO\r\nIconIndex=1\r\n");
    link_file("C:\\Menu\\Half-Life.lnk", "C:\\SIERRA\\Half-Life\\hl.exe", "", 0);
    link_file("C:\\Menu\\Icon.lnk", "C:\\SIERRA\\Half-Life\\hl.exe", "%SystemRoot%\\SYSTEM\\set.dll", -3);
    link_file("C:\\Menu\\Notes.lnk", "C:\\GAMES\\notes.txt", "", 0);
    link_file("C:\\Menu\\Relative.lnk", "C:\\GAMES\\DOOM.EXE", "..\\GAMES\\DOOM.ICO", 0);
    readonly("C:\\Custom");

    /* Programs show themselves; shortcuts their IconLocation, else their target's. */
    location("C:\\SIERRA\\Half-Life\\hl.exe", "C:\\SIERRA\\Half-Life\\hl.exe", 0);
    location("C:\\Menu\\Half-Life.lnk", "C:\\SIERRA\\Half-Life\\hl.exe", 0);
    location("C:\\Menu\\Icon.lnk", "C:\\WINDOWS\\SYSTEM\\set.dll", -3);
    location("C:\\Menu\\Relative.lnk", "C:\\GAMES\\DOOM.ICO", 0);
    location("C:\\Menu\\Notes.lnk", "C:\\WINDOWS\\SYSTEM\\SHELL32.DLL", 0);        /* a document: shell32's document icon */
    location("C:\\GAMES\\DOOM.PIF", "C:\\GAMES\\DOOM.ICO", 0);
    location("C:\\GAMES\\PLAIN.PIF", "C:\\GAMES\\DOOM.EXE", 0);                   /* its program (no icon: the MS-DOS one) */
    location("C:\\Menu\\Site.url", "C:\\GAMES\\DOOM.ICO", 1);
    location("C:\\Custom", "C:\\Custom\\folder.ico", 2);                           /* read-only folder: desktop.ini */
    location("C:\\Plain", "C:\\WINDOWS\\SYSTEM\\SHELL32.DLL", 3);                  /* writable folder: ignored */
    location("D:\\", "D:\\cd.ico", 2);                                             /* AUTORUN.INF icon= */
    puts("PASS SHGFI_ICONLOCATION: programs, shortcuts (IconLocation, %SystemRoot%, relative, target), PIF, URL, desktop.ini, AUTORUN.INF");

    struct fileinfo fi;
    assert(SHGetFileInfoA("C:\\Menu\\Half-Life.lnk", 0, &fi, sizeof(fi), 0x100) == 1);
    assert(fi.icon == (HICON)(uintptr_t)0x5000 && !strcmp(last_path, "C:\\SIERRA\\Half-Life\\hl.exe") && !last_index);
    assert(SHGetFileInfoA("C:\\GAMES\\notes.txt", 0, &fi, sizeof(fi), 0x100) == 1 && fi.icon == (HICON)(uintptr_t)0x100);
    assert(SHGetFileInfoA("C:\\Plain", 0, &fi, sizeof(fi), 0x100) == 1 && fi.icon == (HICON)(uintptr_t)0x101);
    assert(SHGetFileInfoA("C:\\Plain", 0, &fi, sizeof(fi), 0x102) == 1 && fi.icon == (HICON)(uintptr_t)0x103);   /* SHGFI_OPENICON */
    assert(SHGetFileInfoA("C:\\GAMES\\DOOM.EXE", 0, &fi, sizeof(fi), 0x100) == 1 && fi.icon == (HICON)(uintptr_t)0x5000);
    assert(SHGetFileInfoA("anything.exe", 0x80, &fi, sizeof(fi), 0x110) == 1 && fi.icon == (HICON)(uintptr_t)0x102);   /* USEFILEATTRIBUTES */
    assert(SHGetFileInfoA("anything", 0x10, &fi, sizeof(fi), 0x110) == 1 && fi.icon == (HICON)(uintptr_t)0x101);
    assert(SHGetFileInfoA("C:\\SIERRA\\Half-Life\\hl.exe", 0, &fi, sizeof(fi), 0x400) && !strcmp(fi.type, "Application"));
    assert(SHGetFileInfoA("C:\\Menu\\Half-Life.lnk", 0, &fi, sizeof(fi), 0x400) && !strcmp(fi.type, "Shortcut"));
    assert(SHGetFileInfoA("C:\\GAMES\\notes.txt", 0, &fi, sizeof(fi), 0x400) && !strcmp(fi.type, "TXT File"));
    assert(SHGetFileInfoA("C:\\Plain", 0, &fi, sizeof(fi), 0x400) && !strcmp(fi.type, "File Folder"));
    assert(!SHGetFileInfoA("C:\\GAMES\\notes.txt", 0, &fi, sizeof(fi), 0x4100) && !fi.icon);   /* no shell image list */
    puts("PASS SHGFI_ICON: own icons, shell defaults (document, folder, open folder, program), USEFILEATTRIBUTES, TYPENAME");

    assert(SHGetFileInfoA("C:\\SIERRA\\Half-Life\\hl.exe", 0, 0, 0, 0x2000) == (0x4550u | 0x0400u << 16));
    assert(SHGetFileInfoA("C:\\GAMES\\DOOM.EXE", 0, 0, 0, 0x2000) == 0x5a4d);
    assert(SHGetFileInfoA("C:\\GAMES\\WIN16.EXE", 0, 0, 0, 0x2000) == (0x454eu | 0x030au << 16));
    assert(!SHGetFileInfoA("C:\\GAMES\\notes.txt", 0, 0, 0, 0x2000));
    assert(!SHGetFileInfoA("C:\\GAMES\\DOOM.EXE", 0, 0, 0, 0x2100));               /* EXETYPE combines with nothing */
    puts("PASS SHGFI_EXETYPE: PE (Windows 4.0), MS-DOS, NE (Windows 3.10), not a program");

    assert(ExtractIconA(0, "C:\\GAMES\\notes.txt", 0) == (HICON)(uintptr_t)1);
    assert(!ExtractIconA(0, "C:\\missing.exe", 0));
    assert(ExtractIconA(0, "C:\\SIERRA\\Half-Life\\hl.exe", 0) == (HICON)(uintptr_t)0x5000);
    puts("PASS ExtractIconA: an icon, NULL for a missing file, 1 for a file that is not a program, library or icon file");
    chmod(argv[1], 0700);
    char p[1400]; snprintf(p, sizeof(p), "%s/C/Custom", host_root); chmod(p, 0700);
    host_finish();
    return 0;
}
