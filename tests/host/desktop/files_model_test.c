/* SPDX-License-Identifier: MIT
 * Files model against the fake tree: navigation, canonical case, history,
 * selection, sorting, type names, find, create/rename, and incremental
 * copy/move/delete/properties jobs with short I/O and injected errors. */
#include "fake_vfs.h"
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

int main(void)
{
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
    assert(!fm_job_start(&j, &fake_vfs, FM_COPY, system, 1, "/Documents")); finish(&j);
    assert(j.state == FM_DONE && !j.metadata_warnings && fake_metadata_count == 5 && j.created_count == 1 && !strcmp(j.created[0], "system"));
    same("/system/readme.txt", "/Documents/system/readme.txt"); same("/system/sub/a.txt", "/Documents/system/sub/a.txt");
    fm_job_destroy(&j); fake_assert_clean();
    fixture(); assert(fm_job_start(&j, &fake_vfs, FM_COPY, system, 1, "/system/sub") == -EINVAL);
    char *root[] = {"/"}; assert(fm_job_start(&j, &fake_vfs, FM_DELETE, root, 1, NULL) == -EBUSY);

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
    puts("PASS Files model: names/types, canonical case, history, selection, sorting, find, create/rename, copy/move/delete/properties, prompts, Copy of, EXDEV, Cancel, injected errors");
    return 0;
}
