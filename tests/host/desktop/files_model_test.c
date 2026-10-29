/* SPDX-License-Identifier: MIT
 * Files model against the fake tree: navigation, canonical case, history,
 * selection, sorting, type names, find, create/rename, and incremental
 * copy/move/delete/properties jobs with short I/O and injected errors. */
#include "../../../apps/libdeskui/deskui.h" /* before <time.h>: the CiukiOS ABI header defines CLOCKS_PER_SEC */
#include "../../../apps/files/entry_order.h"
#include "fake_vfs.h"
#include "../../../apps/files/transfer_ui.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fixture(void)
{
    fake_reset();
    fake_create("/system", FM_DIR); fake_create("/system/sub", FM_DIR); fake_create("/Documents", FM_DIR);
    fake_create("/mnt", FM_DIR); fake_create("/mnt/d", FM_DIR);
    fake_file("/system/readme.txt", 33001, 0);
    fake_file("/system/sub/a.txt", 17, 42);
    fake_create("/system/Z.txt", FM_FILE);
}
static void finish(struct fm_job *j) { for (unsigned i = 0; i < 20000 && j->state == FM_RUNNING; i++) fm_job_step(j); assert(j->state != FM_RUNNING); }
static void same(const char *a, const char *b)
{
    int x = fake_lookup(a), y = fake_lookup(b); assert(x >= 0 && y >= 0);
    assert(fake_nodes[x].meta.size == fake_nodes[y].meta.size);
    assert(!memcmp(fake_nodes[x].data, fake_nodes[y].data, (size_t)fake_nodes[x].meta.size));
}
static unsigned index_of(struct fm_model *m, const char *name)
{ for (unsigned i = 0; i < m->count; i++) if (!strcmp(m->entries[i].name, name)) return i; assert(!"missing entry"); return 0; }

static void computer_tests(void)
{
    static const struct { const char *posix, *display; } paths[] = {
        {"/", "C:\\"}, {"/Programs", "C:\\Programs"}, {"/Documents/My File.txt", "C:\\Documents\\My File.txt"},
        {"/mnt/d", "D:\\"}, {"/mnt/d/Games/MiXeD", "D:\\Games\\MiXeD"}, {"/mnt/z/x", "Z:\\x"},
        {"/mnt/data/x", "C:\\mnt\\data\\x"}, {FM_COMPUTER, "Computer"}
    };
    char out[FM_PATH], back[FM_PATH];
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        assert(!fm_display_path(out, paths[i].posix) && !strcmp(out, paths[i].display));
        assert(!fm_posix_path(back, out) && !strcmp(back, paths[i].posix));
    }
    assert(!fm_posix_path(out, "d:/Games\\MiXeD\\\\") && !strcmp(out, "/mnt/d/Games/MiXeD"));
    assert(!fm_posix_path(out, "/Programs///MiXeD//") && !strcmp(out, "/Programs/MiXeD"));
    assert(!fm_display_path(out, "/mnt/D/Games///") && !strcmp(out, "D:\\Games"));
    assert(!fm_posix_path(out, "C:") && !strcmp(out, "/"));
    assert(!fm_posix_path(out, "D:") && !strcmp(out, "/mnt/d"));
    assert(fm_posix_path(out, "C:Programs") == -EINVAL);
    /* fx-17b: floppies A:/B: live under /mnt like D:..Z:; C: is "/". */
    assert(!fm_posix_path(out, "A:\\") && !strcmp(out, "/mnt/a") && !fm_display_path(out, "/mnt/b/DOC.TXT") && !strcmp(out, "B:\\DOC.TXT"));
    assert(fm_drive_root("/mnt/a") && !fm_drive_root("/mnt/c") && fm_volume("/mnt/a/x") == 'A' && fm_volume("/mnt/c") == 'C');
    assert(fm_posix_path(out, "relative") == -EINVAL && fm_posix_path(out, NULL) == -EINVAL);
    char long_path[FM_PATH + 4]; memset(long_path, 'x', sizeof(long_path));
    long_path[0] = '/'; long_path[sizeof(long_path) - 1] = 0;
    assert(fm_posix_path(out, long_path) == -ENAMETOOLONG);
    long_path[FM_PATH - 2] = 0; assert(fm_display_path(out, long_path) == -ENAMETOOLONG);
    strcpy(out, "D:\\Games\\"); assert(!fm_posix_path(out, out) && !strcmp(out, "/mnt/d/Games"));
    assert(!fm_display_path(out, out) && !strcmp(out, "D:\\Games"));
    assert(fm_hidden("/", "DEV") && fm_hidden("/", "mnt") && fm_hidden("/", "tmp"));
    assert(!fm_hidden("/", ".config") && !fm_hidden("/Games", "tmp"));

    fake_reset(); fake_create("/Programs", FM_DIR); fake_create("/Documents", FM_DIR);
    fake_create("/dev", FM_DIR); fake_create("/tmp", FM_DIR); fake_create("/.config", FM_DIR);
    struct fm_model m; fm_init(&m, &fake_vfs);
    assert(!fm_navigate(&m, "Computer") && !m.path[0] && m.count == 1 && !strcmp(m.entries[0].name, "C:"));
    assert(!fm_entry_path(&m, 0, out) && !strcmp(out, "/"));
    fm_select(&m, 0, 0);
    assert(fm_create(&m, "No", 1) == -EPERM && fm_rename(&m, 0, "No") == -EPERM && fm_clipboard(&m, 1) == -EPERM);
    assert(!fm_navigate(&m, "C:\\") && m.count == 3);
    assert(!strcmp(m.entries[0].name, ".config"));
    assert(!fm_up(&m) && !m.path[0] && m.entries[m.active].selected && !strcmp(m.entries[m.active].name, "C:"));
    assert(!fm_history(&m, -1) && !strcmp(m.path, "/"));
    fake_create("/mnt", FM_DIR); fake_create("/mnt/d", FM_DIR); fake_create("/mnt/d/Games", FM_DIR);
    fake_create("/mnt/d/Games/tmp", FM_DIR); fake_create("/mnt/not-a-drive", FM_DIR); fake_file("/mnt/e", 1, 1);
    assert(!fm_navigate(&m, FM_COMPUTER) && m.count == 2);
    assert(!strcmp(m.entries[0].name, "C:") && !strcmp(m.entries[1].name, "D:"));
    assert(!fm_entry_path(&m, 1, out) && !strcmp(out, "/mnt/d"));
    assert(!fm_navigate(&m, "D:\\Games\\") && !strcmp(m.path, "/mnt/d/Games") && m.count == 1);
    assert(!fm_up(&m) && !strcmp(m.path, "/mnt/d"));
    assert(!fm_up(&m) && !m.path[0] && !strcmp(m.entries[m.active].name, "D:"));
    assert(!fm_navigate(&m, "/mnt") && !m.path[0]);
    fake_nodes[fake_lookup("/mnt/d")].live = 0;
    assert(!fm_refresh(&m) && m.count == 1);
    fake_create("/mnt/z", FM_DIR); fake_create("/mnt/D", FM_DIR);
    assert(!fm_refresh(&m) && m.count == 3 && !strcmp(m.entries[1].name, "D:") && !strcmp(m.entries[2].name, "Z:"));
    assert(!fm_navigate(&m, "D:\\") && !strcmp(m.path, "/mnt/D"));
    assert(!fm_up(&m) && !m.path[0]);
    /* fx-17b: an empty floppy drive is listed from its slot (dev 0, no
     * medium); with a diskette it is the /mnt/a volume, browsable. */
    m.drive_slots = 1;
    assert(!fm_refresh(&m) && m.count == 4 && !strcmp(m.entries[0].name, "A:") && !m.entries[0].meta.dev && m.entries[0].meta.type == FM_DIR);
    assert(fm_navigate(&m, "A:\\") == -ENOENT && !m.path[0]);
    fake_create("/mnt/a", FM_DIR); fake_file("/mnt/a/README.TXT", 10, 1);
    assert(!fm_refresh(&m) && m.count == 4 && m.entries[0].meta.dev);
    assert(!fm_navigate(&m, "A:\\") && !strcmp(m.path, "/mnt/a") && m.count == 1);
    assert(!fm_up(&m) && !m.path[0] && !strcmp(m.entries[m.active].name, "A:"));
    fake_nodes[fake_lookup("/mnt/a/README.TXT")].live = 0; fake_nodes[fake_lookup("/mnt/a")].live = 0; m.drive_slots = 0;
    assert(!fm_refresh(&m) && m.count == 3);
    fm_destroy(&m); fake_assert_clean();
    puts("PASS Files Computer model: one/two volumes, drive paths both ways, case/separators/limits, hidden nodes, drive Up/history, protected drives, floppy A: with and without a diskette");
}

static void read_accounting(void)
{
    fake_reset(); fake_file("/read.bin",4097,5);
    int fd=fake_vfs.open(NULL,"/read.bin",0); assert(fd>=0);
    unsigned char buffer[4096];
    assert(fake_vfs.read(NULL,fd,buffer,sizeof(buffer))==4096 && buffer[0]==5);
    assert(fake_vfs.read(NULL,fd,buffer,sizeof(buffer))==1);
    assert(fake_vfs.read(NULL,fd,buffer,sizeof(buffer))==0);
    assert(fake_reads.calls==3 && fake_reads.bytes==4097 && fake_reads.requested==12288 && fake_reads.max_request==4096);
    assert(!fake_vfs.close(NULL,fd)); fd=fake_vfs.open(NULL,"/read.bin",0); assert(fd>=0);
    fake_short_io=1; assert(fake_vfs.read(NULL,fd,buffer,sizeof(buffer))==113);
    fake_countdown=1; assert(fake_vfs.read(NULL,fd,buffer,sizeof(buffer))==-EIO);
    assert(fake_reads.calls==5 && fake_reads.bytes==4210 && fake_reads.requested==20480);
    assert(!fake_vfs.close(NULL,fd)); fake_assert_clean();
    fake_reset(); assert(!fake_reads.calls && !fake_reads.bytes && !fake_reads.max_request);
    puts("PASS VFS read accounting: requested/returned bytes, sizes, short reads, EOF, errors, reset");
}
/* fx-17: the T23 trial of e831e685. A song dragged from a USB stick (E:,
 * published read-only) to the desktop folder on C: ran as a move; the copy
 * completed, deleting the source failed with EROFS and the job reported a
 * failed move ("errors=1"). A move whose source cannot be deleted keeps
 * both copies (the drop rule itself is fx-19's fm_drop_effect). */
static void stick(void)
{
    fixture(); fake_create("/mnt/e", FM_DIR); fake_create("/mnt/e/Music", FM_DIR);
    fake_file("/mnt/e/song.mp3", 30000, 3); fake_file("/mnt/e/Music/one.mp3", 9000, 4); fake_file("/mnt/e/Music/two.mp3", 7000, 5);
    fake_create("/Documents/Desktop", FM_DIR);
}
static void report(const struct fm_job *j, char *title, char *text, int *icon)
{ title[0] = text[0] = 0; *icon = 0; fm_job_report(j, title, 64, text, 320, icon); }
static void fx17_tests(void)
{
    /* Drive of a path (fx-19 owns the drop-effect rule, fm_drop_effect). */
    assert(fm_drive_letter("/mnt/e/song.mp3") == 'E' && fm_drive_letter("/Documents") == 'C' && fm_drive_letter("/mnt/E") == 'E');
    assert(fm_drive_letter("/mnt/data/x") == 'C' && !fm_drive_letter("") && !fm_drive_letter("relative"));
    struct fm_job j; char title[64], text[320]; int icon;
    char *song[] = {"/mnt/e/song.mp3"}, *music[] = {"/mnt/e/Music"};
    /* The trial: a move from the read-only stick keeps both copies. */
    stick(); snprintf(fake_readonly, sizeof(fake_readonly), "/mnt/e");
    assert(!fm_job_start(&j, &fake_vfs, FM_MOVE, song, 1, "/Documents/Desktop")); finish(&j);
    assert(j.state == FM_DONE && !j.error && j.kept == 1 && j.kept_error == EROFS && j.completed == 1 && j.bytes == 30000);
    same("/mnt/e/song.mp3", "/Documents/Desktop/song.mp3");
    report(&j, title, text, &icon);
    assert(icon == DU_DLG_ICON_WARNING && strstr(text, "'song.mp3' was copied to C:\\Documents\\Desktop") && strstr(text, "Both copies are kept"));
    assert(strstr(text, "on E:") && strstr(text, "read-only") && !strstr(text, "Cannot") && !strstr(title, "Error"));
    fm_job_destroy(&j); fake_assert_clean();
    /* The same when the source volume reports read-only before EXDEV. */
    stick(); snprintf(fake_readonly, sizeof(fake_readonly), "/mnt/e"); fake_rofs_first = 1;
    assert(!fm_job_start(&j, &fake_vfs, FM_MOVE, song, 1, "/Documents/Desktop")); finish(&j);
    assert(j.state == FM_DONE && j.kept == 1 && fake_lookup("/Documents/Desktop/song.mp3") >= 0 && fake_lookup(song[0]) >= 0);
    fm_job_destroy(&j); fake_assert_clean();
    /* A folder move from it: every file copied, files and folders kept. */
    stick(); snprintf(fake_readonly, sizeof(fake_readonly), "/mnt/e");
    assert(!fm_job_start(&j, &fake_vfs, FM_MOVE, music, 1, "/Documents")); finish(&j);
    assert(j.state == FM_DONE && j.kept == 2 && j.kept_folders == 1 && fake_lookup("/mnt/e/Music/one.mp3") >= 0);
    same("/mnt/e/Music/one.mp3", "/Documents/Music/one.mp3"); same("/mnt/e/Music/two.mp3", "/Documents/Music/two.mp3");
    report(&j, title, text, &icon); assert(strstr(text, "2 original file(s) and 1 folder(s) on E:") && strstr(text, "Both copies are kept"));
    fm_job_destroy(&j); fake_assert_clean();
    /* A move within the read-only stick changes nothing and says why. */
    stick(); snprintf(fake_readonly, sizeof(fake_readonly), "/mnt/e");
    assert(!fm_job_start(&j, &fake_vfs, FM_MOVE, song, 1, "/mnt/e/Music")); finish(&j);
    assert(j.state == FM_FAILED && j.error == EROFS && !j.gone && fake_lookup("/mnt/e/song.mp3") >= 0 && fake_lookup("/mnt/e/Music/song.mp3") < 0);
    report(&j, title, text, &icon);
    assert(icon == DU_DLG_ICON_ERROR && !strcmp(title, "Error Moving File or Folder") && strstr(text, "Cannot move 'song.mp3':\nThe disk is read-only or write-protected."));
    fm_job_destroy(&j); fake_assert_clean();
    /* Delete, copy to and rename on a write-protected medium name the file. */
    stick(); snprintf(fake_readonly, sizeof(fake_readonly), "/mnt/e");
    assert(!fm_job_start(&j, &fake_vfs, FM_DELETE, song, 1, NULL)); finish(&j);
    assert(j.state == FM_FAILED && j.error == EROFS && fake_lookup(song[0]) >= 0);
    report(&j, title, text, &icon); assert(strstr(text, "Cannot delete 'song.mp3':\nThe disk is read-only")); fm_job_destroy(&j);
    char *readme[] = {"/system/readme.txt"};
    assert(!fm_job_start(&j, &fake_vfs, FM_COPY, readme, 1, "/mnt/e")); finish(&j);
    assert(j.state == FM_FAILED && j.error == EROFS && fake_lookup("/mnt/e/readme.txt") < 0);
    report(&j, title, text, &icon); assert(strstr(text, "Cannot copy 'readme.txt'")); fm_job_destroy(&j); fake_assert_clean();
    struct fm_model m; fm_init(&m, &fake_vfs); assert(!fm_navigate(&m, "/mnt/e"));
    assert(fm_rename(&m, index_of(&m, "song.mp3"), "renamed.mp3") == -EROFS && fake_lookup(song[0]) >= 0); fm_destroy(&m);
    /* A writable stick: move off it deletes the source; to it, from C:. */
    stick();
    assert(!fm_job_start(&j, &fake_vfs, FM_MOVE, song, 1, "/Documents/Desktop")); finish(&j);
    assert(j.state == FM_DONE && !j.kept && fake_lookup(song[0]) < 0 && fake_lookup("/Documents/Desktop/song.mp3") >= 0);
    report(&j, title, text, &icon); assert(!text[0]); fm_job_destroy(&j);
    char *back[] = {"/Documents/Desktop/song.mp3"};
    assert(!fm_job_start(&j, &fake_vfs, FM_MOVE, back, 1, "/mnt/e/Music")); finish(&j);
    assert(j.state == FM_DONE && fake_lookup("/mnt/e/Music/song.mp3") >= 0 && fake_lookup(back[0]) < 0); fm_job_destroy(&j);
    fm_init(&m, &fake_vfs); assert(!fm_navigate(&m, "/mnt/e/Music"));
    assert(!fm_rename(&m, index_of(&m, "song.mp3"), "renamed.mp3") && fake_lookup("/mnt/e/Music/renamed.mp3") >= 0); fm_destroy(&m);
    char *renamed[] = {"/mnt/e/Music/renamed.mp3"};
    assert(!fm_job_start(&j, &fake_vfs, FM_DELETE, renamed, 1, NULL)); finish(&j);
    assert(j.state == FM_DONE && fake_lookup(renamed[0]) < 0); fm_job_destroy(&j); fake_assert_clean();
    /* Removal mid-copy: the job names the drive, the partial copy is gone. */
    stick(); snprintf(fake_gone, sizeof(fake_gone), "/mnt/e"); fake_gone_after = 2;
    assert(!fm_job_start(&j, &fake_vfs, FM_COPY, song, 1, "/Documents/Desktop")); j.chunk = 4096; finish(&j);
    assert(j.state == FM_FAILED && j.error == EIO && j.gone == 'E' && fake_lookup("/Documents/Desktop/song.mp3") < 0);
    report(&j, title, text, &icon);
    assert(strstr(text, "Cannot copy 'song.mp3': the disk in drive E: was removed.") && strstr(text, "The incomplete copy was deleted."));
    fm_job_destroy(&j); fake_assert_clean();
    /* ...and while copying to the stick. */
    stick(); snprintf(fake_gone, sizeof(fake_gone), "/mnt/e"); fake_gone_after = -1;
    assert(!fm_job_start(&j, &fake_vfs, FM_COPY, readme, 1, "/mnt/e")); j.chunk = 4096;
    for (unsigned i = 0; i < 6 && j.state == FM_RUNNING; i++) fm_job_step(&j);
    fake_gone_after = 0; finish(&j);
    assert(j.state == FM_FAILED && j.gone == 'E' && fake_lookup(readme[0]) >= 0);
    report(&j, title, text, &icon); assert(strstr(text, "drive E: was removed") && strstr(text, "may remain"));
    fm_job_destroy(&j);
    /* An I/O error on a present medium is not a removal. */
    stick(); fake_countdown = 0;
    assert(!fm_job_start(&j, &fake_vfs, FM_COPY, song, 1, "/Documents/Desktop")); j.chunk = 4096;
    while (j.counting && j.state == FM_RUNNING) fm_job_step(&j);
    fake_countdown = 4; finish(&j); fake_countdown = 0;
    assert(j.state == FM_FAILED && j.error == EIO && !j.gone);
    report(&j, title, text, &icon); assert(strstr(text, "Cannot copy 'song.mp3':\nA disk error occurred (I/O error)."));
    fm_job_destroy(&j); fake_assert_clean();
    puts("PASS Files fx-17: drive letters, move from a read-only stick keeps both copies, removal mid-copy names the drive, read-only delete/copy/rename name the file");
}

struct sort_probe { int key, original; };
static int probe_order(const void *a, const void *b, const void *context)
{
    const struct sort_probe *x=a,*y=b;
    return *(const int *)context*(x->key<y->key ? -1 : x->key>y->key);
}
static void sort_stability_tests(void)
{
    const struct sort_probe input[]={{2,0},{1,1},{2,2},{0,3},{1,4},{0,5},{2,6}};
    struct sort_probe values[7];
    for (int direction=-1;direction<=1;direction+=2) {
        memcpy(values,input,sizeof(values));
        assert(fm_stable_sort(values,7,sizeof(*values),probe_order,&direction));
        for (unsigned i=1;i<7;i++) {
            assert(direction*(values[i-1].key-values[i].key)<=0);
            if (values[i-1].key==values[i].key) assert(values[i-1].original<values[i].original);
        }
    }
    int direction=1;
    assert(fm_stable_sort(NULL,0,sizeof(*values),probe_order,&direction));
    assert(fm_stable_sort(values,1,sizeof(*values),probe_order,&direction));
    memcpy(values,input,sizeof(values));
    assert(!fm_stable_sort(values,SIZE_MAX,sizeof(*values),probe_order,&direction));
    assert(!memcmp(values,input,sizeof(values)));
}

int main(void)
{
    sort_stability_tests();
    fx17_tests();
    read_accounting();
    computer_tests();
    /* Paths and names. */
    char out[FM_PATH];
    assert(!fm_join(out, "/", "Programs") && !strcmp(out, "/Programs") && !fm_join(out, "/Programs", "x") && !strcmp(out, "/Programs/x"));
    assert(fm_join(out, "/", "a/b") == -EINVAL && fm_join(out, "/", "..") == -EINVAL);
    assert(fm_beneath("/System/x", "/system") && !fm_beneath("/systemx", "/system") && fm_beneath("/a", "/"));
    assert(!fm_parent(out, "/a/b") && !strcmp(out, "/a") && !fm_parent(out, "/a") && !strcmp(out, "/"));
    assert(!fm_valid_name("New Folder") && fm_valid_name("a?b") == -EINVAL && fm_valid_name("x.") == -EINVAL && fm_valid_name("") == -EINVAL);
    struct fm_entry e = {.meta = {.type = FM_FILE}}; char buf[40];
    strcpy(e.name, "BOOT.CFG"); assert(!strcmp(fm_type_name(&e, buf, sizeof(buf)), "Configuration Settings") && fm_kind(&e) == FM_KIND_TEXT);
    strcpy(e.name, "game.xyz"); assert(!strcmp(fm_type_name(&e, buf, sizeof(buf)), "XYZ File") && fm_kind(&e) == FM_KIND_FILE);
    strcpy(e.name, "lua"); assert(!strcmp(fm_type_name(&e, buf, sizeof(buf)), "File"));
    e.meta.exec = 1; assert(!strcmp(fm_type_name(&e, buf, sizeof(buf)), "Application") && fm_kind(&e) == FM_KIND_APP);
    e.meta.type = FM_DIR; assert(!strcmp(fm_type_name(&e, buf, sizeof(buf)), "File Folder") && fm_kind(&e) == FM_KIND_FOLDER);

    /* Navigation: canonical spelling, ".." and history. */
    fixture(); struct fm_model m; fm_init(&m, &fake_vfs);
    assert(!fm_canonical(&fake_vfs, "/SYSTEM/./SUB/..//", out) && !strcmp(out, "/system"));
    assert(fm_canonical(&fake_vfs, "/system/readme.txt", out) == -ENOENT || fm_canonical(&fake_vfs, "/system/readme.txt", out) == -ENOTDIR);
    assert(fm_canonical(&fake_vfs, "relative", out) == -ENOENT);
    assert(!fm_navigate(&m, "/")); assert(!fm_navigate(&m, "/System"));
    assert(!strcmp(m.path, "/system") && m.count == 3 && !strcmp(m.entries[0].name, "sub"));
    assert(fm_navigate(&m, "/missing") == -ENOENT && !strcmp(m.path, "/system"));
    assert(!fm_navigate(&m, "/system/sub")); assert(!fm_up(&m));
    assert(!strcmp(m.path, "/system") && !strcmp(m.entries[m.active].name, "sub") && m.entries[m.active].selected);
    assert(!fm_history(&m, -1) && !strcmp(m.path, "/system/sub"));
    assert(!fm_history(&m, -1) && !strcmp(m.path, "/system"));
    assert(fm_history(&m, -5) == -ENOENT);
    assert(!fm_history(&m, 1) && !strcmp(m.path, "/system/sub"));
    assert(!fm_navigate(&m, "/Documents") && m.history_at + 1 == m.history_count);   /* new branch drops forward entries */
    assert(!fm_navigate(&m, "/system"));

    /* Selection: click, Shift range, Ctrl toggle, Ctrl+Shift add, invert, focus. */
    fm_select(&m, 1, 0); fm_select(&m, 2, 1);
    assert(!m.entries[0].selected && m.entries[1].selected && m.entries[2].selected && fm_selected(&m, NULL) == 2);
    fm_select(&m, 1, 2); assert(!m.entries[1].selected && m.anchor == 1);
    fm_select(&m, 0, 3); assert(m.entries[0].selected && m.entries[1].selected && m.entries[2].selected);
    fm_select_all(&m, 1); assert(!fm_selected(&m, NULL));
    fm_focus(&m, 2); assert(m.active == 2 && !fm_selected(&m, NULL));
    fm_select_all(&m, 0); uint64_t bytes; assert(fm_selected(&m, &bytes) == 3 && bytes == 33001);
    fm_select_none(&m); assert(fm_select_name(&m, "Z.TXT", 0) == 2 && m.entries[2].selected);

    /* Sorting keeps folders first and is stable; focus follows its entry. */
    fm_select(&m, index_of(&m, "readme.txt"), 0);
    fm_sort(&m, 1, 1); assert(!strcmp(m.entries[0].name, "sub") && !strcmp(m.entries[1].name, "readme.txt") && m.active == 1);
    fm_sort(&m, 1, 0); assert(!strcmp(m.entries[1].name, "Z.txt") && !strcmp(m.entries[m.active].name, "readme.txt"));
    fm_sort(&m, 2, 0); assert(!strcmp(m.entries[1].name, "readme.txt") && !strcmp(m.entries[2].name, "Z.txt"));
    fm_sort(&m, 3, 1); assert(!strcmp(m.entries[0].name, "sub"));
    fm_sort(&m, 0, 0);

    /* Type to find: prefix within one second, repeated letters cycle. */
    assert(fm_find(&m, 'z', 10) == 2); assert(fm_find(&m, 'r', 3000000000ull) == 1);
    assert(fm_find(&m, 'e', 3100000000ull) == 1);          /* "re" */
    assert(fm_find(&m, 's', 9000000000ull) == 0);
    assert(fm_find(&m, 's', 9100000000ull) == 0);           /* "ss": cycles, only one match */
    assert(fm_find(&m, 'q', 20000000000ull) == -ENOENT);

    /* Clipboard, create with unique names, rename with conflicts. */
    fm_select(&m, 1, 0); assert(!fm_clipboard(&m, 1) && m.clipboard_count == 1 && m.cut);
    fm_clipboard_clear(&m); assert(!m.clipboard_count);
    assert(!fm_navigate(&m, "/Documents"));
    char name[FM_NAME];
    assert(!fm_unique(&m, "New Folder", "", name) && !strcmp(name, "New Folder"));
    assert(!fm_create(&m, name, 1) && fake_lookup("/Documents/New Folder") >= 0 && m.entries[m.active].selected);
    assert(!fm_unique(&m, "New Folder", "", name) && !strcmp(name, "New Folder (2)"));
    assert(!fm_unique(&m, "New Text Document", ".txt", name) && !fm_create(&m, name, 0) && fake_lookup("/Documents/New Text Document.txt") >= 0);
    assert(fm_create(&m, "bad/name", 1) == -EINVAL && fm_create(&m, "New Folder", 1) == -EEXIST);
    unsigned folder = index_of(&m, "New Folder");
    assert(fm_rename(&m, folder, "New Text Document.txt") == -EEXIST);
    assert(fm_rename(&m, folder, "a:b") == -EINVAL);
    assert(!fm_rename(&m, index_of(&m, "New Folder"), "Renamed") && fake_lookup("/Documents/Renamed") >= 0 && !strcmp(m.entries[m.active].name, "Renamed"));
    assert(!fm_rename(&m, index_of(&m, "Renamed"), "RENAMED") && fake_lookup("/Documents/RENAMED") >= 0);   /* case-only */
    fake_countdown = 1; assert(fm_refresh(&m) == -EIO); fake_countdown = 0;
    fm_destroy(&m);

    /* Recursive copy with short I/O; metadata adapter called per item. */
    struct fm_job j; char *system[] = {"/system"}, *file[] = {"/system/readme.txt"};
    fixture(); fake_short_io = 1;
    assert(!fm_job_start(&j, &fake_vfs, FM_COPY, system, 1, "/Documents"));
    while (j.counting && j.state==FM_RUNNING) fm_job_step(&j);
    assert(j.total_bytes==33018 && j.total_items==5 && !j.bytes && !j.items_done);
    assert(!fake_reads.calls && fake_lookup("/Documents/system")<0);
    finish(&j);
    assert(j.bytes==j.total_bytes && j.items_done==j.total_items);
    assert(j.state == FM_DONE && !j.metadata_warnings && fake_metadata_count == 5 && j.created_count == 1 && !strcmp(j.created[0], "system"));
    same("/system/readme.txt", "/Documents/system/readme.txt"); same("/system/sub/a.txt", "/Documents/system/sub/a.txt");
    fm_job_destroy(&j); fake_assert_clean();
    fixture(); assert(fm_job_start(&j, &fake_vfs, FM_COPY, system, 1, "/system/sub") == -EINVAL);
    char *root[] = {"/"}; assert(fm_job_start(&j, &fake_vfs, FM_DELETE, root, 1, NULL) == -EBUSY);

    /* Same-volume directory rename credits the counted subtree, with no
     * content reads or per-file durability calls. */
    fixture(); assert(!fm_job_start(&j,&fake_vfs,FM_MOVE,system,1,"/Documents")); finish(&j);
    assert(j.state==FM_DONE && j.bytes==33018 && j.total_bytes==33018);
    assert(j.items_done==5 && j.total_items==5 && !fake_reads.calls);
    assert(fake_lookup("/system")<0 && fake_lookup("/Documents/system/sub/a.txt")>=0);
    fm_job_destroy(&j); fake_assert_clean();

    /* Cancel mid-copy removes the unfinished copy and keeps the source. */
    fixture(); assert(!fm_job_start(&j, &fake_vfs, FM_COPY, file, 1, "/Documents"));
    fm_job_step(&j); fm_job_step(&j); fm_job_step(&j); fm_job_cancel(&j); finish(&j);
    assert(j.state == FM_CANCELLED && fake_lookup(file[0]) >= 0 && fake_lookup("/Documents/readme.txt") < 0);
    fm_job_destroy(&j); fake_assert_clean();

    /* Replace prompt: No keeps the old file, Yes to All replaces. */
    fixture(); int old = fake_file("/Documents/readme.txt", 3, 'o');
    assert(!fm_job_start(&j, &fake_vfs, FM_COPY, file, 1, "/Documents")); finish(&j);
    assert(j.state == FM_REPLACE && !j.prompt_dir && j.frames[0].existing.size == 3);
    fm_job_answer(&j, FM_NO); finish(&j);
    assert(j.state == FM_DONE && j.skipped == 1 && fake_nodes[old].meta.size == 3 && !j.created_count);
    fm_job_destroy(&j); fake_assert_clean();
    assert(!fm_job_start(&j, &fake_vfs, FM_COPY, file, 1, "/Documents")); finish(&j); fm_job_answer(&j, FM_YES_ALL); finish(&j);
    assert(j.state == FM_DONE); same(file[0], "/Documents/readme.txt"); fm_job_destroy(&j); fake_assert_clean();
    assert(!fm_job_start(&j, &fake_vfs, FM_COPY, file, 1, "/Documents")); finish(&j); fm_job_answer(&j, FM_CANCEL); finish(&j);
    assert(j.state == FM_CANCELLED); fm_job_destroy(&j); fake_assert_clean();

    /* Existing folders prompt before a merge; merges keep unrelated files. */
    fixture(); fake_create("/Documents/system", FM_DIR); fake_file("/Documents/system/keep.txt", 4, 'k');
    assert(!fm_job_start(&j, &fake_vfs, FM_COPY, system, 1, "/Documents")); finish(&j);
    assert(j.state == FM_REPLACE && j.prompt_dir); fm_job_answer(&j, FM_YES); finish(&j);
    assert(j.state == FM_DONE && fake_lookup("/Documents/system/keep.txt") >= 0 && fake_lookup("/Documents/system/sub/a.txt") >= 0);
    fm_job_destroy(&j); fake_assert_clean();

    /* Copy beside the source makes "Copy of"; moving onto itself is a no-op. */
    fixture(); assert(!fm_job_start(&j, &fake_vfs, FM_COPY, file, 1, "/system")); finish(&j);
    assert(j.state == FM_DONE && fake_lookup("/system/Copy of readme.txt") >= 0 && !strcmp(j.created[0], "Copy of readme.txt"));
    fm_job_destroy(&j);
    assert(!fm_job_start(&j, &fake_vfs, FM_COPY, file, 1, "/system")); finish(&j);
    assert(fake_lookup("/system/Copy (2) of readme.txt") >= 0); fm_job_destroy(&j);
    assert(!fm_job_start(&j, &fake_vfs, FM_MOVE, file, 1, "/system")); finish(&j);
    assert(j.state == FM_DONE && j.completed == 1 && fake_lookup(file[0]) >= 0); fm_job_destroy(&j); fake_assert_clean();

    /* Moves: same volume renames, other volume (EXDEV) copies then deletes. */
    fixture(); assert(!fm_job_start(&j, &fake_vfs, FM_MOVE, file, 1, "/Documents")); finish(&j);
    assert(j.state == FM_DONE && fake_lookup("/Documents/readme.txt") >= 0 && fake_lookup(file[0]) < 0); fm_job_destroy(&j);
    fixture(); assert(!fm_job_start(&j, &fake_vfs, FM_MOVE, system, 1, "/mnt/d")); finish(&j);
    assert(j.state == FM_DONE && fake_lookup("/system") < 0 && fake_lookup("/mnt/d/system/sub/a.txt") >= 0); fm_job_destroy(&j); fake_assert_clean();
    char *moved[] = {"/mnt/d/system"}; assert(!fm_job_start(&j, &fake_vfs, FM_DELETE, moved, 1, NULL)); finish(&j);
    assert(j.state == FM_DONE && fake_lookup(moved[0]) < 0 && j.files == 3 && j.folders == 2); fm_job_destroy(&j); fake_assert_clean();

    /* Properties counts contents recursively. */
    fixture(); assert(!fm_job_start(&j, &fake_vfs, FM_PROPERTIES, system, 1, NULL)); finish(&j);
    assert(j.bytes == 33018 && j.files == 3 && j.folders == 1); fm_job_destroy(&j);

    /* Every injected failure leaves the source intact and no temporary file. */
    fixture(); assert(!fm_job_start(&j, &fake_vfs, FM_COPY, file, 1, "/Documents")); finish(&j);
    int stages = fake_operations; fm_job_destroy(&j);
    for (int failure = 1; failure <= stages; failure++) {
        fixture(); fake_countdown = failure;
        assert(!fm_job_start(&j, &fake_vfs, FM_COPY, file, 1, "/Documents")); finish(&j);
        assert(j.state == FM_FAILED || j.state == FM_DONE);
        if (j.state == FM_FAILED) assert(j.error > 0);
        fake_countdown = 0; fm_job_destroy(&j); fake_assert_clean();
        int n = fake_lookup(file[0]); assert(n >= 0 && fake_nodes[n].meta.size == 33001);
        for (unsigned i = 0; i < 33001; i++) assert(fake_nodes[n].data[i] == i % 251);
    }
    fixture(); fake_countdown = 3; assert(!fm_job_start(&j, &fake_vfs, FM_DELETE, system, 1, NULL)); finish(&j);
    assert(j.state == FM_FAILED); fm_job_destroy(&j);

    /* Without a metadata setter (the F2 ABI) copies warn instead. */
    fixture(); struct fm_vfs plain = fake_vfs; plain.metadata = NULL;
    assert(!fm_job_start(&j, &plain, FM_COPY, file, 1, "/Documents")); finish(&j);
    assert(j.state == FM_DONE && j.metadata_warnings == 1); fm_job_destroy(&j); fake_assert_clean();
    /* fx-19 shared drop-effect rule (Raymond Chen, "Will dragging a file result in a move or a copy?"). */
    assert(fm_volume("/")=='C' && fm_volume("/Documents/x")=='C' && fm_volume("/mnt/e")=='E' && fm_volume("/mnt/E/a")=='E' &&
           fm_volume("/mnt/ex")=='C' && !fm_volume("") && !fm_volume("C:"));
    const unsigned cm=FM_EFFECT_COPY|FM_EFFECT_MOVE;
    assert(fm_drop_effect('C','C',0,cm)==FM_EFFECT_MOVE && fm_drop_effect('E','C',0,cm)==FM_EFFECT_COPY);
    assert(fm_drop_effect('E','C',FM_KEY_SHIFT,cm)==FM_EFFECT_MOVE && fm_drop_effect('C','C',FM_KEY_CTRL,cm)==FM_EFFECT_COPY);
    assert(fm_drop_effect('C','C',FM_KEY_CTRL|FM_KEY_SHIFT,cm)==FM_EFFECT_NONE && fm_drop_effect('C','C',FM_KEY_ALT,FM_EFFECTS)==FM_EFFECT_LINK);
    assert(fm_drop_effect('C','C',FM_KEY_CTRL|FM_KEY_SHIFT,FM_EFFECTS)==FM_EFFECT_LINK);
    assert(fm_drop_effect('C','C',0,FM_EFFECT_COPY)==FM_EFFECT_COPY && fm_drop_effect('E','C',0,FM_EFFECT_MOVE)==FM_EFFECT_MOVE);
    assert(fm_drop_effect('C','C',FM_KEY_SHIFT,FM_EFFECT_COPY)==FM_EFFECT_NONE && fm_drop_effect('C',0,0,cm)==FM_EFFECT_NONE && fm_drop_effect('C','C',0,0)==FM_EFFECT_NONE);
    char *drop_items[]={"/Documents/a.txt","/Documents/Sub"};
    assert(!fm_drop_check(drop_items,2,"/Documents/Other",FM_EFFECT_MOVE) && fm_drop_check(drop_items,2,"/Documents/Sub/In",FM_EFFECT_COPY)==-EINVAL);
    assert(fm_drop_check(drop_items,2,"/Documents",FM_EFFECT_MOVE)==-EEXIST && !fm_drop_check(drop_items,2,"/Documents",FM_EFFECT_COPY));
    puts("PASS fx-19 drop effect rule: volumes, same/other drive default, Shift/Ctrl/Ctrl+Shift/Alt, allowed-set fallback, folder into itself, same-folder move");
    puts("PASS Files model: names/types, canonical case, history, selection, sorting, find, create/rename, copy/move/delete/properties, prompts, Copy of, EXDEV, Cancel, injected errors");
    return 0;
}
