/* LFNTEST.COM: the long file name API (INT 21h AX=71xxh) end to end, run
 * from the DOS prompt by scripts/qemu_test_long_names.py. Lines go to the
 * console (and COM1); the last one is "LFNTEST PASS" or "LFNTEST FAIL ...".
 * It leaves C:\LFNTEST and C:\TRUNC.DAT for the host to check. */
typedef unsigned char u8;
typedef unsigned int u16;
typedef unsigned long u32;
struct regs { u16 ax, bx, cx, dx, si, di, ds, es, flags; };
int intr(int n, struct regs *r);
static u16 my_ds(void);
#pragma aux my_ds = "mov ax,ds" value [ax];

static struct regs R;
static char fd[320];                /* Win32 find data */
static char buf[300], buf2[300];
static int failed;

static void putc_(char c) { struct regs r; r.ax = 0x0200; r.dx = (u8)c; r.ds = r.es = my_ds(); intr(0x21, &r); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void nl(void) { puts_("\r\n"); }
static void hex(u16 v) { int i; for (i = 12; i >= 0; i -= 4) putc_("0123456789ABCDEF"[(v >> i) & 15]); }
static void dec(u16 v) { char t[6]; int n = 0; do t[n++] = (char)('0' + v % 10); while (v /= 10); while (n) putc_(t[--n]); }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void scpy(char *d, const char *s) { while ((*d++ = *s++) != 0) ; }
static void scat(char *d, const char *s) { scpy(d + slen(d), s); }

static void clr(void) { int i; for (i = 0; i < (int)sizeof R / 2; i++) ((u16 *)&R)[i] = 0; R.ds = R.es = my_ds(); }
static int dos(u16 ax) { R.ax = ax; return intr(0x21, &R); }
static void fail(const char *step)
{
    puts_("LFNTEST FAIL "); puts_(step); puts_(" ax="); hex(R.ax); nl();
    failed = 1;
}
static void pass(const char *step) { puts_("LFNTEST ok "); puts_(step); nl(); }
#define CHECK(cond, step) do { if (!(cond)) { fail(step); return 1; } pass(step); } while (0)

static int mkdir_l(const char *p) { clr(); R.dx = (u16)p; return dos(0x7139); }
static int rmdir_l(const char *p) { clr(); R.dx = (u16)p; return dos(0x713A); }
static int chdir_l(const char *p) { clr(); R.dx = (u16)p; return dos(0x713B); }
static int open_l(const char *p, u16 mode, u16 action)
{
    clr(); R.si = (u16)p; R.bx = mode; R.dx = action; R.cx = 0;
    return dos(0x716C) ? -1 : (int)R.ax;
}
static int close_(int h) { clr(); R.bx = h; return dos(0x3E00); }
static int write_(int h, const char *s, int n) { clr(); R.bx = h; R.cx = n; R.dx = (u16)s; return dos(0x4000) ? -1 : (int)R.ax; }
static int read_(int h, char *s, int n) { clr(); R.bx = h; R.cx = n; R.dx = (u16)s; return dos(0x3F00) ? -1 : (int)R.ax; }
static int truename(const char *p, int cl, char *out) { clr(); R.si = (u16)p; R.di = (u16)out; R.cx = cl; return dos(0x7160); }
static int find_first(const char *p, int attr)
{
    clr(); R.dx = (u16)p; R.di = (u16)fd; R.cx = attr; R.si = 1;
    return dos(0x714E) ? -1 : (int)R.ax;
}
static int find_next(int h) { clr(); R.bx = h; R.di = (u16)fd; R.si = 1; return dos(0x714F); }
static void find_close(int h) { clr(); R.bx = h; dos(0x71A1); }
static int rename_l(const char *a, const char *b) { clr(); R.dx = (u16)a; R.di = (u16)b; return dos(0x7156); }
static int exists(const char *p) { int h = find_first(p, 0x16); if (h < 0) return 0; find_close(h); return 1; }
static int make_file(const char *p, const char *text)
{
    int h = open_l(p, 2, 0x12);
    if (h < 0) return 0;
    write_(h, text, slen(text));
    close_(h);
    return 1;
}
static char *name_of(void) { return fd + 44; }
static char *alt_of(void) { return fd + 304; }

#define D "C:\\LFNTEST"
#define DOCS D "\\My Documents 2026"
#define GROW D "\\Growth folder with many entries"

static int test_truncate(void)
{
    static char big[512];
    int h, i;
    u16 size;
    clr(); R.dx = (u16)"C:\\TRUNC.DAT"; R.cx = 0;
    CHECK(!dos(0x3C00), "3ch-create");
    h = R.ax;
    for (i = 0; i < 512; i++) big[i] = (char)('A' + i % 26);
    for (i = 0; i < 8; i++) write_(h, big, 512);
    close_(h);
    clr(); R.dx = (u16)"C:\\TRUNC.DAT"; R.cx = 0;
    CHECK(!dos(0x3C00), "3ch-existing");
    h = R.ax;
    write_(h, "0123456789", 10);
    close_(h);
    clr(); R.dx = (u16)"C:\\TRUNC.DAT"; dos(0x3D00); h = R.ax;
    clr(); R.bx = h; dos(0x4202); size = R.ax;
    close_(h);
    puts_("LFNTEST trunc size="); dec(size); nl();
    CHECK(size == 10 && R.dx == 0, "3ch-truncates");
    clr(); R.dx = (u16)"C:\\TRUNC.DAT"; R.cx = 1; dos(0x4301);
    clr(); R.dx = (u16)"C:\\TRUNC.DAT"; R.cx = 0;
    CHECK(dos(0x3C00) && R.ax == 5, "3ch-readonly-denied");
    clr(); R.dx = (u16)"C:\\TRUNC.DAT"; R.cx = 0x20; dos(0x4301);
    return 0;
}

static int run(void)
{
    int h, n, i, count;
    clr(); R.bx = 0x4C46;
    CHECK(!dos(0x71FF) && R.bx == 0x4F4B, "installed");
    clr(); R.dx = (u16)"C:\\"; R.di = (u16)buf; R.cx = 32;
    CHECK(!dos(0x71A0) && (R.bx & 0x4000) && R.cx == 255 && seq(buf, "FAT"), "volume-info");
    if (test_truncate()) return 1;

    CHECK(!mkdir_l(D), "mkdir-short");
    CHECK(!mkdir_l(DOCS), "mkdir-long");
    CHECK(mkdir_l(DOCS) && R.ax == 5, "mkdir-exists");
    CHECK(mkdir_l(D "\\bad|name"), "mkdir-invalid");

    h = open_l(DOCS "\\New Text Document.txt", 2, 0x10);
    CHECK(h >= 0 && R.cx == 2, "create-long");
    write_(h, "hello long names\r\n", 18);
    clr(); R.bx = h; R.dx = (u16)buf;
    CHECK(!dos(0x71A6) && *(u16 *)(buf + 38) == 18, "handle-info");
    close_(h);
    h = open_l(DOCS "\\new text document.TXT", 0, 1);
    CHECK(h >= 0 && R.cx == 1, "open-any-case");
    n = read_(h, buf, 100);
    close_(h);
    buf[n > 0 ? n : 0] = 0;
    CHECK(seq(buf, "hello long names\r\n"), "read-back");
    CHECK(open_l(DOCS "\\New Text Document.txt", 2, 0x10) < 0 && R.ax == 80, "create-new-exists");

    CHECK(!truename(DOCS "\\New Text Document.txt", 1, buf), "truename-short");
    puts_("LFNTEST short="); puts_(buf); nl();
    CHECK(seq(buf, "C:\\LFNTEST\\MYDOCU~1\\NEWTEX~1.TXT"), "short-alias");
    CHECK(!truename(D "\\MYDOCU~1\\NEWTEX~1.TXT", 2, buf) && seq(buf, DOCS "\\New Text Document.txt"), "truename-long");

    CHECK(!chdir_l(DOCS), "chdir-long");
    clr(); R.dx = 0; R.si = (u16)buf;
    CHECK(!dos(0x7147) && seq(buf, "LFNTEST\\My Documents 2026"), "getcwd-long");
    h = open_l("New Text Document.txt", 0, 1);
    CHECK(h >= 0, "open-relative");
    close_(h);
    CHECK(!chdir_l("..\\.."), "chdir-dotdot");
    clr(); R.dx = 0; R.si = (u16)buf;
    CHECK(!dos(0x7147) && buf[0] == 0, "getcwd-root");

    h = find_first(DOCS "\\*", 0x16);
    CHECK(h >= 0, "find-first");
    count = 0;
    do {
        puts_("LFNTEST found \""); puts_(name_of()); puts_("\" alt \""); puts_(alt_of()); puts_("\""); nl();
        if (seq(name_of(), "New Text Document.txt") && seq(alt_of(), "NEWTEX~1.TXT")) count++;
    } while (!find_next(h));
    CHECK(R.ax == 18, "find-next-end");
    find_close(h);
    CHECK(count == 1, "find-long-name");

    CHECK(!rename_l(DOCS "\\New Text Document.txt", DOCS "\\Renamed with a much longer name than before.txt"), "rename-long");
    CHECK(exists(DOCS "\\Renamed with a much longer name than before.txt") && !exists(DOCS "\\New Text Document.txt"), "rename-result");
    CHECK(make_file(D "\\casefile.txt", "c"), "create-lower");
    CHECK(!rename_l(D "\\casefile.txt", D "\\CaseFile.TXT"), "rename-case");
    h = find_first(D "\\casefile.txt", 0x16);
    CHECK(h >= 0 && seq(name_of(), "CaseFile.TXT"), "case-kept");
    find_close(h);

    CHECK(!mkdir_l(D "\\Another folder"), "mkdir-another");
    CHECK(!rename_l(DOCS "\\Renamed with a much longer name than before.txt", D "\\Another folder\\Moved file.txt"), "move-file");
    CHECK(!rename_l(D "\\Another folder", DOCS "\\Another folder"), "move-folder");
    CHECK(rename_l(DOCS, DOCS "\\Another folder\\Inside itself") && R.ax == 5, "move-into-itself");
    CHECK(!chdir_l(DOCS "\\Another folder"), "chdir-moved");
    CHECK(!chdir_l(".."), "chdir-moved-parent");
    clr(); R.dx = 0; R.si = (u16)buf;
    CHECK(!dos(0x7147) && seq(buf, "LFNTEST\\My Documents 2026"), "moved-dotdot");
    chdir_l("\\");
    h = open_l(DOCS "\\Another folder\\Moved file.txt", 0, 1);
    n = read_(h, buf, 100); close_(h); buf[n > 0 ? n : 0] = 0;
    CHECK(h >= 0 && seq(buf, "hello long names\r\n"), "moved-content");

    clr(); R.dx = (u16)(DOCS "\\Another folder"); R.bx = 0;
    CHECK(!dos(0x7143) && R.cx == 0x10, "attr-folder");
    clr(); R.dx = (u16)(D "\\CaseFile.TXT"); R.bx = 1; R.cx = 0x21;
    CHECK(!dos(0x7143), "attr-set");
    clr(); R.dx = (u16)(D "\\CaseFile.TXT"); R.bx = 0;
    CHECK(!dos(0x7143) && R.cx == 0x21, "attr-get");
    clr(); R.dx = (u16)(D "\\CaseFile.TXT"); R.bx = 1; R.cx = 0x20; dos(0x7143);
    clr(); R.dx = (u16)(D "\\CaseFile.TXT"); R.bx = 3; R.cx = 0x6000; R.di = 0x5B3E;
    CHECK(!dos(0x7143), "time-set");
    clr(); R.dx = (u16)(D "\\CaseFile.TXT"); R.bx = 4;
    CHECK(!dos(0x7143) && R.cx == 0x6000 && R.di == 0x5B3E, "time-get");

    /* Sixty names of five slots each: the folder grows past one cluster. */
    CHECK(!mkdir_l(GROW), "mkdir-growth");
    for (i = 0; i < 60; i++) {
        scpy(buf, GROW "\\Long file name number ");
        buf2[0] = (char)('0' + i / 10); buf2[1] = (char)('0' + i % 10); buf2[2] = 0;
        scat(buf, buf2);
        scat(buf, " in growth test.txt");
        if (!make_file(buf, buf2)) { fail("growth-create"); return 1; }
    }
    pass("growth-create");
    count = 0;
    h = find_first(GROW "\\*.txt", 0x16);
    if (h >= 0) { do count++; while (!find_next(h)); find_close(h); }
    puts_("LFNTEST growth count="); dec(count); nl();
    CHECK(count == 60, "growth-find");
    clr(); R.dx = (u16)(GROW "\\*number 1?*"); R.si = 1; R.cx = 0x16;
    CHECK(!dos(0x7141), "delete-wild");
    count = 0;
    h = find_first(GROW "\\*", 0x16);
    if (h >= 0) { do if (name_of()[0] != '.') count++; while (!find_next(h)); find_close(h); }
    CHECK(count == 50, "delete-wild-count");
    clr(); R.dx = (u16)(GROW "\\Long file name number 20 in growth test.txt"); R.si = 0;
    CHECK(!dos(0x7141) && !exists(GROW "\\Long file name number 20 in growth test.txt"), "delete-one");
    CHECK(make_file(GROW "\\Long file name number 20 in growth test.txt", "20"), "recreate");

    /* The 8.3 API drops long name entries it leaves behind. */
    CHECK(make_file(D "\\Deleted by short name.txt", "x"), "create-for-83-delete");
    truename(D "\\Deleted by short name.txt", 1, buf2);
    clr(); R.dx = (u16)buf2;
    CHECK(!dos(0x4100) && !exists(D "\\Deleted by short name.txt"), "83-delete");
    CHECK(make_file(D "\\Renamed by short name.txt", "y"), "create-for-83-rename");
    truename(D "\\Renamed by short name.txt", 1, buf2);
    clr(); R.dx = (u16)buf2; R.di = (u16)(D "\\RENAMED.TXT");
    CHECK(!dos(0x5600), "83-rename");
    h = find_first(D "\\RENAMED.TXT", 0x16);
    CHECK(h >= 0 && seq(name_of(), "RENAMED.TXT") && !alt_of()[0], "83-rename-no-long-name");
    find_close(h);
    CHECK(!mkdir_l(D "\\Empty folder with a long name"), "mkdir-empty");
    CHECK(!rmdir_l(D "\\Empty folder with a long name") && !exists(D "\\Empty folder with a long name"), "rmdir-long");
    CHECK(!mkdir_l(D "\\Removed by short name"), "mkdir-83-rmdir");
    truename(D "\\Removed by short name", 1, buf2);
    clr(); R.dx = (u16)buf2;
    CHECK(!dos(0x3A00) && !exists(D "\\Removed by short name"), "83-rmdir");
    clr(); R.si = (u16)"Some long name.text"; R.di = (u16)buf; R.dx = 0x0100;
    CHECK(!dos(0x71A8) && seq(buf, "SOMELO~1.TEX"), "generate-short");
    return 0;
}

int probe_main(void)
{
    int r = run();
    puts_(r || failed ? "LFNTEST FAIL" : "LFNTEST PASS");
    nl();
    return r || failed;
}
