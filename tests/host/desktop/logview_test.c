/* CiukiOS Log Viewer host tests (G13, ER-5): which logs exist, tail reading,
 * the window, refresh, renders. SPDX-License-Identifier: MIT */
#include "logview.h"
#include "acc_test.h"
#include <errno.h>
#include <sys/stat.h>
int setenv(const char *, const char *, int);

static void put(const char *root, const char *relative, const char *text, size_t length)
{
    char path[1024]; snprintf(path, sizeof(path), "%s%s", root, relative);
    FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(text, 1, length, f) == length); fclose(f);
}
static void crash(const char *root, const char *relative, unsigned sequence, const char *exe)
{
    char text[256];
    int n = snprintf(text, sizeof(text), "CiukiOS crash sequence=%u\nExecutable: %s\nPID: 7 TID: 9\nNative fault\nPage fault: write of unmapped address 00000010\n", sequence, exe);
    put(root, relative, text, (size_t)n);
}
static void dir(const char *root, const char *relative)
{
    char path[1024]; snprintf(path, sizeof(path), "%s%s", root, relative);
    assert(!mkdir(path, 0777) || errno == EEXIST);
}
int main(int argc, char **argv)
{
    assert(argc >= 2);
    const char *root = argv[1], *folder = argc > 2 ? argv[2] : NULL;
    setenv("HOME", root, 1);
    dir(root, "/system"); dir(root, "/system/logs"); dir(root, "/WINDOWS"); dir(root, "/WINDOWS/CRASH"); dir(root, "/SYSTEM");

    /* Nothing written yet: the two logs the system always names are listed. */
    struct lv_list list;
    lv_scan(&list, root);
    assert(list.count == 2 && list.entry[0].kind == LV_BOOT && list.entry[1].kind == LV_NETSURF);
    assert(!strcmp(list.entry[0].title, "Boot log") && !strcmp(list.entry[1].title, "NetSurf log"));

    /* A 100 KB boot log with CR LF lines, a Setup log and four crash files (one with a bad header). */
    static char big[100 * 1024]; size_t length = 0;
    for (unsigned line = 0; length + 40 < sizeof(big); line++) length += (size_t)snprintf(big + length, 40, "[%06u] boot line\r\n", line);
    put(root, "/system/logs/BOOT.LOG", big, length);
    put(root, "/SYSTEM/SETUP.LOG", "Setup done\n", 11);
    crash(root, "/system/logs/CRASH0.TXT", 3, "alpha");
    crash(root, "/system/logs/CRASH1.TXT", 5, "beta");
    crash(root, "/WINDOWS/CRASH/CRASH0.TXT", 4, "gamma.exe");
    put(root, "/system/logs/CRASH2.TXT", "not a crash file\n", 17);
    put(root, "/system/logs/crash3.txt", "CiukiOS crash sequence=2\nExecutable: lower\n", 43);   /* FAT names may be lower case */
    put(root, "/system/logs/CRASHES.TXT", "CiukiOS crash sequence=9\nExecutable: no\n", 41);      /* not CRASH<digit>.TXT */
    lv_scan(&list, root);
    assert(list.count == 7);
    assert(list.entry[2].kind == LV_SETUP && !strcmp(list.entry[2].title, "Setup log"));
    assert(!strcmp(list.entry[3].title, "Crash 5: beta") && !strcmp(list.entry[4].title, "Win32 crash 4: gamma.exe"));
    assert(!strcmp(list.entry[5].title, "Crash 3: alpha") && !strcmp(list.entry[6].title, "Crash 2: lower"));
    assert(list.entry[3].kind == LV_CRASH && list.entry[3].sequence == 5);

    /* Tail: bounded, starts on a line, CR LF folded, size reported. */
    char tail[4096]; size_t got; unsigned long total;
    assert(list.entry[0].scope == LV_MACHINE && !strcmp(list.entry[0].name, "BOOT.LOG") && !strcmp(list.entry[0].path, "C:\\SYSTEM\\LOGS\\BOOT.LOG"));
    assert(!lv_read_entry(&list.entry[0], tail, sizeof(tail), &got, &total));
    assert(total == length && got > 3000 && got < sizeof(tail) && tail[0] == '[' && !memchr(tail, '\r', got));
    assert(tail[got - 1] == '\n');
    char last[32]; snprintf(last, sizeof(last), "[%06u] boot line\n", (unsigned)(total / 20 - 1));
    assert(got >= strlen(last) && !memcmp(tail + got - strlen(last), last, strlen(last)));
    assert(!lv_read_tail(list.entry[2].path, tail, sizeof(tail), &got, &total) && got == 11 && total == 11 && !memcmp(tail, "Setup done\n", 11));
    put(root, "/system/logs/NUL.TXT", "a\0b\r\n", 5);
    char path[1024]; snprintf(path, sizeof(path), "%s/system/logs/NUL.TXT", root);
    assert(!lv_read_tail(path, tail, sizeof(tail), &got, &total) && got == 4 && !memcmp(tail, "a b\n", 4));
    snprintf(path, sizeof(path), "%s/system/logs/NONE.LOG", root);
    assert(lv_read_tail(path, tail, sizeof(tail), &got, &total) == -ENOENT && !got);
    assert(lv_read_tail(path, tail, 0, &got, &total) == -EINVAL);

    /* The window. */
    lv_root = root;
    static uint32_t pixels[LV_W * LV_H];
    static struct du_ui ui; static struct lv_app app;
    du_init(&ui, pixels, LV_W, LV_H);
    assert(!lv_attach(&app, &ui));
    assert(app.list.count == 7 && app.selected == 0 && app.ta.readonly && app.ta.len > 55000 && app.ta.len < LV_TEXT_MAX);
    assert(strstr(du_get(&ui, LV_STATUS)->text, "Boot log (read-only): last "));
    assert(app.ta.text[0] == '[' && strstr(app.ta.text, "boot line"));
    du_render(&ui);
    at_save(folder, "logview-boot", &ui);
    /* Down selects the NetSurf log, which does not exist yet and says who writes it. */
    at_tap(&ui, DU_KEY_DOWN); assert(app.selected == 1);
    assert(strstr(app.ta.text, "There is no NetSurf log yet") && strstr(app.ta.text, "NetSurf when a page fails"));
    assert(strstr(du_get(&ui, LV_STATUS)->text, "(not available)"));
    /* A crash file shows its text; typing never edits. */
    at_tap(&ui, DU_KEY_DOWN); at_tap(&ui, DU_KEY_DOWN); assert(app.selected == 3);
    assert(strstr(app.ta.text, "Executable: beta") && strstr(app.ta.text, "Page fault: write of unmapped address 00000010"));
    du_focus(&ui, LV_TEXT); size_t before = app.ta.len; at_tap(&ui, 0x1e); assert(app.ta.len == before);
    du_render(&ui);
    at_save(folder, "logview-crash", &ui);
    /* Refresh keeps the selected file even when a newer crash moves it down. */
    crash(root, "/system/logs/CRASH4.TXT", 8, "newest");
    lv_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = LV_M_REFRESH}, &app);
    assert(app.list.count == 8 && !strcmp(app.list.entry[3].title, "Crash 8: newest"));
    assert(app.selected == 4 && strstr(app.ta.text, "Executable: beta"));
    du_focus(&ui, LV_LIST);
    at_tap(&ui, DU_KEY_UP); assert(app.selected == 3 && strstr(app.ta.text, "Executable: newest"));
    at_tap(&ui, DU_KEY_DOWN); at_tap(&ui, DU_KEY_DOWN); assert(strstr(app.ta.text, "Executable: gamma.exe"));
    at_tap(&ui, DU_KEY_UP);
    /* Copy a selection. */
    du_ta_select(&app.ta, 0, 7);
    lv_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = LV_M_COPY}, &app);
    assert(!strcmp(du_clipboard_get(), "CiukiOS"));
    /* Resize keeps the content. */
    uint32_t *large = calloc(760 * 520 + 1, 4); assert(large);
    large[760 * 520] = 0xdeadbeef;
    du_resize(&ui, large, 760, 520);
    assert(app.selected == 4 && du_get(&ui, LV_STATUS)->box.y == 500 && du_get(&ui, LV_STATUS)->box.h == 20);
    du_render(&ui); assert(large[760 * 520] == 0xdeadbeef);
    du_resize(&ui, pixels, LV_W, LV_H); free(large);
    lv_detach(&app);

    /* sec-sysvc: the user's own logs (the per-user log folder of the SYSTEM
     * log service; <root>/user/logs here) beside the system logs: NetSurf's
     * log, native and Win32 crash reports, and maintenance logs. */
    dir(root, "/user"); dir(root, "/user/logs");
    const char *ns = "2026-10-28T10:00:00Z [netsurf] exit reason=quit\n";
    put(root, "/user/logs/NETSURF.LOG", ns, strlen(ns));
    crash(root, "/user/logs/CRASH0.TXT", 9, "user-native");
    char w32[160]; int w = snprintf(w32, sizeof(w32), "CiukiOS crash sequence=1\nExecutable: game.exe\nPID: 3 TID: 4\nException: c0000005\n");
    put(root, "/user/logs/WCRASH0.TXT", w32, (size_t)w);
    put(root, "/system/logs/SCANDISK.LOG", "ScanDisk: no errors\n", 20);
    lv_scan(&list, root);
    assert(list.count == 11);
    assert(list.entry[1].kind == LV_NETSURF && list.entry[1].scope == LV_USER &&
           strstr(list.entry[1].path, "Local Settings\\Application Data\\CiukiOS\\Logs\\NETSURF.LOG"));
    assert(list.entry[3].kind == LV_SYSTEM && !strcmp(list.entry[3].title, "ScanDisk log") && list.entry[3].scope == LV_MACHINE);
    assert(!strcmp(list.entry[4].title, "Crash 9: user-native") && list.entry[4].scope == LV_USER);
    assert(!strcmp(list.entry[10].title, "Win32 crash 1: game.exe") && list.entry[10].scope == LV_USER);
    assert(!lv_read_entry(&list.entry[1], tail, sizeof(tail), &got, &total) && total == strlen(ns) && strstr(tail, "exit reason=quit"));
    assert(!lv_read_entry(&list.entry[3], tail, 8, &got, &total) && total == 20 && got <= 8);
    puts("logview: PASS");
    return 0;
}
