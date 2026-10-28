/* SPDX-License-Identifier: MIT
 * H11 sec-user-05: the Windows XP merged desktop view (apps/desktop/
 * desktop_union.h) over the production Files model, and the session's
 * per-user shell folders (apps/desktop/profile.h), on a host directory. */
#define _DEFAULT_SOURCE 1
#include "../../../apps/desktop/desktop_union.h"
#include "../../../apps/desktop/profile.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "H11 shell folders: %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
/* A POSIX table as fm_native_vfs (apps/files/vfs.c) without its sniffing. */
static int p_list(void *c, const char *path, unsigned flags, fm_emit emit, void *arg)
{
    (void)c; (void)flags; DIR *d = opendir(path); if (!d) return -errno;
    int r = 0; struct dirent *e;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[FM_PATH]; struct stat s; struct fm_meta m = {.type = FM_OTHER};
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (!stat(full, &s)) { m.type = S_ISDIR(s.st_mode) ? FM_DIR : FM_FILE; m.size = (uint64_t)s.st_size; m.modified = s.st_mtime; }
        r = emit(e->d_name, &m, arg);
    }
    closedir(d); return r;
}
static int p_stat(void *c, const char *path, struct fm_meta *m)
{
    (void)c; struct stat s; if (stat(path, &s)) return -errno;
    *m = (struct fm_meta){S_ISDIR(s.st_mode) ? FM_DIR : FM_FILE, (uint64_t)s.st_size, (uint64_t)s.st_ino, 0, 0, s.st_mtime, 0, 0, 0, 0};
    return 0;
}
static int p_mkdir(void *c, const char *p) { (void)c; return mkdir(p, 0700) ? -errno : 0; }
static int p_rename(void *c, const char *a, const char *b) { (void)c; return rename(a, b) ? -errno : 0; }
static int p_unlink(void *c, const char *p) { (void)c; return unlink(p) ? -errno : 0; }
static int p_rmdir(void *c, const char *p) { (void)c; return rmdir(p) ? -errno : 0; }
static int p_open(void *c, const char *p, int create)
{ (void)c; int fd = open(p, create ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY, 0600); return fd < 0 ? -errno : fd; }
static int p_read(void *c, int fd, void *b, unsigned n) { (void)c; int r = (int)read(fd, b, n); return r < 0 ? -errno : r; }
static int p_write(void *c, int fd, const void *b, unsigned n) { (void)c; int r = (int)write(fd, b, n); return r < 0 ? -errno : r; }
static int p_sync(void *c, int fd) { (void)c; return fsync(fd) ? -errno : 0; }
static int p_close(void *c, int fd) { (void)c; return close(fd) ? -errno : 0; }
static const struct fm_vfs posix = {NULL, p_list, p_stat, p_mkdir, p_rename, p_unlink, p_rmdir, p_open, p_read, p_write, p_sync, p_close, NULL, NULL};
static void touch(const char *dir, const char *name)
{ char p[FM_PATH]; snprintf(p, sizeof(p), "%s/%s", dir, name); int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600); CHECK(fd >= 0); close(fd); }
static int present(const char *dir, const char *name)
{ char p[FM_PATH]; struct stat s; snprintf(p, sizeof(p), "%s/%s", dir, name); return !stat(p, &s); }
static int listed(const struct fm_model *m, const char *name, unsigned *times)
{ unsigned n = 0; for (unsigned i = 0; i < m->count; i++) n += !strcmp(m->entries[i].name, name); if (times) *times = n; return n > 0; }
static int index_of(const struct fm_model *m, const char *name)
{ for (unsigned i = 0; i < m->count; i++) if (!strcmp(m->entries[i].name, name)) return (int)i; return -1; }
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    char root[512], shared[FM_PATH], user[FM_PATH], path[FM_PATH], want[FM_PATH];
    snprintf(root, sizeof(root), "%s", argv[1]);
    snprintf(shared, sizeof(shared), "%s/Documents", root); CHECK(!mkdir(shared, 0700));
    snprintf(shared, sizeof(shared), "%s/Documents/Desktop", root); CHECK(!mkdir(shared, 0700));
    snprintf(path, sizeof(path), "%s/Quake III Arena Demo", shared); CHECK(!mkdir(path, 0700));
    touch(shared, "readme.txt"); touch(shared, "same.txt");
    snprintf(user, sizeof(user), "%s/Profiles", root); CHECK(!mkdir(user, 0700));
    snprintf(user, sizeof(user), "%s/Profiles/u00000003", root); CHECK(!mkdir(user, 0700));
    snprintf(user, sizeof(user), "%s/Profiles/u00000003/Documents/Desktop", root);   /* not made yet */

    CHECK(!desk_union_create(&posix, shared, shared) && !desk_union_create(&posix, "relative", shared));
    struct desk_union *u = desk_union_create(&posix, user, shared); CHECK(u);
    struct fm_model m; fm_init(&m, &u->vfs);
    /* 1. Before the user's Desktop exists the view shows the All Users items
     * (folder.c refreshes the view's path when it cannot be navigated yet). */
    CHECK(fm_navigate(&m, user) == -ENOENT);
    snprintf(m.path, sizeof(m.path), "%s", user);
    CHECK(!fm_refresh(&m) && m.count == 3 && !present(user, ""));
    CHECK(!desk_union_ensure(u) && present(user, "") && !desk_union_ensure(u));
    CHECK(!fm_navigate(&m, user) && !strcmp(m.path, user) && m.count == 3);
    CHECK(listed(&m, "Quake III Arena Demo", NULL) && listed(&m, "readme.txt", NULL));
    int q = index_of(&m, "Quake III Arena Demo"); CHECK(q >= 0);
    snprintf(want, sizeof(want), "%s/Quake III Arena Demo", shared);
    CHECK(!fm_entry_path(&m, (unsigned)q, path) && !strcmp(path, want));   /* physical, for launches and clipboards */
    struct fm_meta meta; snprintf(path, sizeof(path), "%s/readme.txt", user);
    CHECK(!u->vfs.stat_path(u, path, &meta) && meta.type == FM_FILE);
    puts("H11 merged desktop: All Users items visible before the personal Desktop exists, physical entry paths PASS");

    /* 2. New items go to the user's own Desktop, made on first use. */
    CHECK(!fm_create(&m, "note.txt", 0) && present(user, "note.txt") && !present(shared, "note.txt"));
    CHECK(!fm_create(&m, "Folder", 1) && present(user, "Folder") && !present(shared, "Folder"));
    snprintf(path, sizeof(path), "%s/Folder/inside.txt", user);
    int fd = u->vfs.open(u, path, 1); CHECK(fd >= 0 && !u->vfs.close(u, fd) && present(user, "Folder/inside.txt"));
    /* The user's item wins a name both folders hold; each name is listed once. */
    touch(user, "same.txt"); unsigned times = 0;
    CHECK(!fm_refresh(&m) && listed(&m, "same.txt", &times) && times == 1 && m.count == 5);
    snprintf(want, sizeof(want), "%s/same.txt", user);
    CHECK(!fm_entry_path(&m, (unsigned)index_of(&m, "same.txt"), path) && !strcmp(path, want));
    puts("H11 merged desktop: new files and folders in the personal Desktop, personal item wins a shared name PASS");

    /* 3. Renaming and deleting a shared item act in its own folder. */
    CHECK(!fm_rename(&m, (unsigned)index_of(&m, "readme.txt"), "readme2.txt"));
    CHECK(present(shared, "readme2.txt") && !present(shared, "readme.txt") && !present(user, "readme2.txt"));
    CHECK(listed(&m, "readme2.txt", NULL));
    snprintf(path, sizeof(path), "%s/readme2.txt", user);
    CHECK(!u->vfs.unlink(u, path) && !present(shared, "readme2.txt"));
    snprintf(path, sizeof(path), "%s/note.txt", user); snprintf(want, sizeof(want), "%s/renamed.txt", user);
    CHECK(!u->vfs.rename(u, path, want) && present(user, "renamed.txt") && !present(shared, "renamed.txt"));
    /* A shared item moved into the view's spelling elsewhere stays physical. */
    snprintf(path, sizeof(path), "%s/Quake III Arena Demo", user);
    CHECK(!u->vfs.stat_path(u, path, &meta) && meta.type == FM_DIR);
    puts("H11 merged desktop: rename and delete of All Users items in place, personal renames stay personal PASS");

    /* 4. An absent or unreadable shared folder adds nothing and is no error. */
    snprintf(path, sizeof(path), "%s/Quake III Arena Demo", shared); CHECK(!rmdir(path));
    snprintf(path, sizeof(path), "%s/same.txt", shared); CHECK(!unlink(path)); CHECK(!rmdir(shared));
    CHECK(!fm_refresh(&m) && m.count == 3 && !listed(&m, "Quake III Arena Demo", NULL));
    snprintf(want, sizeof(want), "%s/same.txt", user);
    CHECK(!fm_entry_path(&m, (unsigned)index_of(&m, "same.txt"), path) && !strcmp(path, want));
    fm_destroy(&m); desk_union_destroy(u);
    puts("H11 merged desktop: absent All Users folder lists the personal items only PASS");

    /* 5. The session's per-user shell folders come from USERPROFILE. */
    CHECK(!unsetenv("USERPROFILE") && !profile_folder(path, sizeof(path), "Start Menu/Programs"));
    CHECK(!setenv("USERPROFILE", "/Windows/Profiles/u00000003", 1));
    CHECK(profile_folder(path, sizeof(path), "Start Menu/Programs") && !strcmp(path, "/Windows/Profiles/u00000003/Start Menu/Programs"));
    CHECK(profile_folder(path, sizeof(path), "Documents/Desktop") && !strcmp(path, "/Windows/Profiles/u00000003/Documents/Desktop"));
    CHECK(!profile_folder(path, 20, "Documents/Desktop"));   /* never truncated */
    CHECK(!setenv("USERPROFILE", "/Windows/Profiles/../System", 1) && !profile_folder(path, sizeof(path), "Documents/Desktop"));
    CHECK(!setenv("USERPROFILE", "/Documents", 1) && !profile_folder(path, sizeof(path), "Documents/Desktop"));
    puts("H11 session shell folders: per-user Programs and Desktop from USERPROFILE, no profile, no traversal, no truncation PASS");
    printf("H11 shell folders: checks=%u PASS\n", checks);
    return 0;
}
