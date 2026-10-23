/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "fake_vfs.h"
#include "cleanup.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define NOW 1800000000ll
#define AGE (7ll * 24 * 60 * 60)
static int old_file(const char *path, unsigned size)
{
    int n = fake_file(path, size, 1);
    fake_nodes[n].meta.modified = NOW - AGE;
    return n;
}
static void finish(struct fm_cleanup *p)
{
    unsigned steps = 0;
    while (fm_cleanup_step(p)) assert(++steps < 10000);
}
static struct fm_cleanup *scan(char drive)
{
    struct fm_cleanup *p = NULL;
    assert(!fm_cleanup_scan(&fake_vfs, drive, NOW, &p)); assert(p); return p;
}
static void job_finish(struct fm_job *j)
{
    unsigned steps = 0;
    while (j->state == FM_RUNNING) { fm_job_step(j); assert(++steps < 10000); }
    assert(j->state == FM_DONE); fm_job_destroy(j);
}
static void temp_selection_and_safety(void)
{
    fake_reset(); fake_create("/tmp", FM_DIR); fake_create("/tmp/sub", FM_DIR);
    old_file("/tmp/old", 10); old_file("/tmp/sub/old", 20);
    int recent = old_file("/tmp/recent", 30); fake_nodes[recent].meta.modified++;
    int future = old_file("/tmp/future", 30); fake_nodes[future].meta.modified = NOW + 1;
    int unknown = old_file("/tmp/unknown", 30); fake_nodes[unknown].meta.modified = 0;
    int ro = old_file("/tmp/readonly", 30); fake_nodes[ro].meta.attributes = 1;
    int sys = old_file("/tmp/system", 30); fake_nodes[sys].meta.attributes = 4;
    int special = fake_create("/tmp/link", FM_OTHER); fake_nodes[special].meta.mode = 0120777;
    int cross = old_file("/tmp/foreign", 30); fake_nodes[cross].meta.dev = 90;
    fake_create("/TEMP", FM_DIR); old_file("/TEMP/old", 40);
    fake_create("/system", FM_DIR); fake_create("/system/TEMP", FM_DIR); old_file("/system/TEMP/old", 50);
    struct fm_cleanup *p = scan('C');
    const struct fm_cleanup_category *c = fm_cleanup_category(p, FM_CLEAN_TEMP);
    assert(c->available && c->items == 4 && c->bytes == 120);
    assert(!fm_cleanup_category(p, FM_CLEAN_INTERNET)->available);
    assert(fm_cleanup_start(p, FM_CLEAN_MASK(FM_CLEAN_INTERNET)) == -EINVAL);
    assert(!fm_cleanup_start(p, 0)); finish(p);
    assert(fake_lookup("/tmp/old") >= 0); fm_cleanup_destroy(p);
    p = scan('c');
    assert(!fm_cleanup_start(p, FM_CLEAN_MASK(FM_CLEAN_TEMP))); finish(p);
    struct fm_cleanup_progress progress; fm_cleanup_progress(p, &progress);
    assert(progress.state == FM_CLEAN_DONE && progress.bytes == 120 && progress.items == 4 && !progress.errors);
    assert(fake_lookup("/tmp/old") < 0 && fake_lookup("/tmp/sub/old") < 0);
    assert(fake_lookup("/tmp/sub") >= 0 && fake_lookup("/tmp/recent") >= 0);
    assert(fake_lookup("/tmp/future") >= 0 && fake_lookup("/tmp/unknown") >= 0);
    assert(fake_lookup("/tmp/readonly") >= 0 && fake_lookup("/tmp/system") >= 0);
    assert(fake_lookup("/tmp/link") >= 0 && fake_lookup("/tmp/foreign") >= 0);
    fm_cleanup_destroy(p); fake_assert_clean();
}
static void snapshot_and_cancel(void)
{
    fake_reset(); fake_create("/tmp", FM_DIR);
    int change = old_file("/tmp/changed", 10), replace = old_file("/tmp/replaced", 11);
    int attr = old_file("/tmp/protected", 12); old_file("/tmp/unchanged", 13);
    struct fm_cleanup *p = scan('C');
    old_file("/tmp/new", 14); fake_nodes[change].meta.modified++;
    fake_nodes[replace].meta.ino++; fake_nodes[attr].meta.attributes = 1;
    assert(!fm_cleanup_start(p, FM_CLEAN_MASK(FM_CLEAN_TEMP))); finish(p);
    struct fm_cleanup_progress progress; fm_cleanup_progress(p, &progress);
    assert(progress.items == 1 && progress.bytes == 13 && progress.skipped == 3);
    assert(fake_lookup("/tmp/new") >= 0 && fake_lookup("/tmp/changed") >= 0);
    fm_cleanup_destroy(p);
    p = scan('C'); assert(!fm_cleanup_start(p, FM_CLEAN_MASK(FM_CLEAN_TEMP)));
    fm_cleanup_cancel(p); assert(!fm_cleanup_step(p)); fm_cleanup_progress(p, &progress);
    assert(progress.state == FM_CLEAN_CANCELLED && !progress.items); fm_cleanup_destroy(p);
    p = scan('C'); fake_nodes[fake_lookup("/tmp")].meta.ino++;
    assert(!fm_cleanup_start(p, FM_CLEAN_MASK(FM_CLEAN_TEMP))); finish(p);
    fm_cleanup_progress(p, &progress); assert(!progress.items && progress.skipped); fm_cleanup_destroy(p);
    fake_assert_clean();
}
static int cancel_scan(void *ctx) { unsigned *calls = ctx; return ++*calls >= 6; }
static void scan_failures(void)
{
    fake_reset(); fake_create("/tmp", FM_DIR); old_file("/tmp/old", 10);
    unsigned calls = 0; struct fm_cleanup *p = (void *)1;
    assert(fm_cleanup_scan_cancelable(&fake_vfs, 'C', NOW, cancel_scan, &calls, &p) == -ECANCELED && !p);
    assert(fm_cleanup_scan(&fake_vfs, 'A', NOW, &p) == -EINVAL && !p);
    assert(fm_cleanup_scan(&fake_vfs, 'C', 0, &p) == -EINVAL && !p);
    fake_countdown = 1; assert(fm_cleanup_scan(&fake_vfs, 'C', NOW, &p) == -EIO && !p);
    p = scan('C'); strcpy(fake_readonly, "/tmp");
    assert(!fm_cleanup_start(p, 1)); finish(p);
    struct fm_cleanup_progress progress; fm_cleanup_progress(p, &progress);
    assert(progress.errors == 1 && progress.first_error == -EROFS && !progress.items);
    fm_cleanup_destroy(p); fake_assert_clean();
}
static void drive_isolation(void)
{
    fake_reset(); fake_create("/tmp", FM_DIR); old_file("/tmp/c", 5);
    fake_create("/mnt", FM_DIR); fake_create("/mnt/d", FM_DIR); fake_create("/mnt/d/TEMP", FM_DIR);
    old_file("/mnt/d/TEMP/d", 15);
    struct fm_cleanup *p = scan('D');
    assert(fm_cleanup_category(p, FM_CLEAN_TEMP)->bytes == 15);
    assert(!fm_cleanup_start(p, 1)); finish(p);
    assert(fake_lookup("/mnt/d/TEMP/d") < 0 && fake_lookup("/tmp/c") >= 0);
    fm_cleanup_destroy(p); fake_assert_clean();
}
static void recycle_snapshot(void)
{
    fake_reset(); fake_create("/Documents", FM_DIR); fake_create("/Documents/folder", FM_DIR);
    old_file("/Documents/folder/a", 10); old_file("/Documents/folder/b", 20);
    old_file("/Documents/file.txt", 30);
    char *sources[] = {"/Documents/folder", "/Documents/file.txt"};
    struct rb_policy policy; rb_policy_default(&policy);
    struct fm_job *j = calloc(1, sizeof(*j)); assert(j);
    assert(!rb_delete_job(j, &fake_vfs, sources, 2, &policy, 1u << 2, 0)); job_finish(j);
    struct rb_usage usage; assert(!rb_usage(&fake_vfs, 'C', &usage) && usage.items == 2);
    struct fm_cleanup *p = scan('C');
    assert(fm_cleanup_category(p, FM_CLEAN_RECYCLE)->bytes == 60);
    int dir = -1;
    for (int i = 0; i < FAKE_NODES; i++) if (fake_nodes[i].live && fake_nodes[i].meta.type == FM_DIR &&
        !strncmp(fake_nodes[i].path, "/RECYCLED/D", 11)) dir = i;
    assert(dir >= 0); char newcomer[FM_PATH]; assert(!fm_join(newcomer, fake_nodes[dir].path, "new")); old_file(newcomer, 40);
    assert(!fm_cleanup_start(p, FM_CLEAN_MASK(FM_CLEAN_RECYCLE))); finish(p);
    struct fm_cleanup_progress progress; fm_cleanup_progress(p, &progress);
    assert(progress.bytes == 60 && progress.items == 3 && progress.errors == 1 && progress.first_error == -ENOTEMPTY);
    assert(fake_lookup(newcomer) >= 0 && fake_lookup("/RECYCLED/INFO2") >= 0);
    assert(!rb_usage(&fake_vfs, 'C', &usage) && usage.items == 1);
    fm_cleanup_destroy(p);
    p = scan('C'); assert(!fm_cleanup_start(p, 2)); finish(p); fm_cleanup_destroy(p);
    assert(!rb_usage(&fake_vfs, 'C', &usage) && !usage.items);
    free(j); fake_assert_clean();
}
static void low_disk(void)
{
    uint32_t seen = 0; const uint64_t mib = 1024ull * 1024;
    assert(fm_cleanup_low_threshold(512 * mib) == 25 * mib);
    assert(fm_cleanup_low_threshold(1280 * mib) == 45 * mib);
    assert(fm_cleanup_low_threshold(2048 * mib) == 65 * mib);
    assert(fm_cleanup_low_threshold(UINT64_MAX) == 65 * mib);
    assert(fm_cleanup_space_warning(&seen, 'C', 512 * mib, 25 * mib) == FM_SPACE_OK);
    assert(fm_cleanup_space_warning(&seen, 'C', 512 * mib, 24 * mib) == FM_SPACE_LOW);
    assert(fm_cleanup_space_warning(&seen, 'C', 512 * mib, 1) == FM_SPACE_OK);
    assert(fm_cleanup_space_warning(&seen, 'D', 512 * mib, 1) == FM_SPACE_LOW);
    assert(fm_cleanup_space_warning(&seen, 'C', 512 * mib, 0) == FM_SPACE_FULL);
    assert(fm_cleanup_space_warning(&seen, 'C', 512 * mib, 0) == FM_SPACE_FULL);
    assert(fm_cleanup_space_warning(&seen, 'C', 0, 0) == FM_SPACE_OK);
    assert(fm_cleanup_space_warning(&seen, '?', 512 * mib, 0) == FM_SPACE_OK);
}
static void nofollow_adapter(void)
{
    char dir[] = "/tmp/ciuki-cleanup-test-XXXXXX"; assert(mkdtemp(dir));
    char target[FM_PATH], link[FM_PATH];
    assert(!fm_join(target, dir, "target") && !fm_join(link, dir, "link"));
    FILE *f = fopen(target, "w"); assert(f); assert(!fclose(f));
    assert(!symlink(target, link)); struct fm_meta m;
    const struct fm_vfs *fs = fm_native_cleanup_vfs();
    assert(!fs->stat_path(fs->ctx, link, &m) && m.type == FM_OTHER);
    assert(!fs->stat_path(fs->ctx, target, &m) && m.type == FM_FILE);
    assert(!unlink(link) && !unlink(target) && !rmdir(dir));
}
int main(void)
{
    temp_selection_and_safety(); snapshot_and_cancel(); scan_failures(); drive_isolation();
    recycle_snapshot(); low_disk(); nofollow_adapter();
    puts("PASS G29 Disk Cleanup model"); return 0;
}
