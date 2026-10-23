/* SPDX-License-Identifier: MIT
 * G2 Recycle Bin service against the fake FAT-like tree: delete, restore,
 * empty, Shift+Delete, the size limit, removable media, the bin as a Files
 * folder view, orphans, legacy Trash migration and INFO2 contents. */
#include "../../../apps/libdeskui/deskui.h" /* before <time.h>: the CiukiOS ABI header defines CLOCKS_PER_SEC */
#include "fake_vfs.h"
#include "../../../apps/files/transfer_ui.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t now_value = 1792000000;
static int64_t clock_now(void) { return now_value; }
static uint64_t capacity_value = 10000;
static uint64_t capacity_of(char drive) { (void)drive; return capacity_value; }

static void fixture(void)
{
    fake_image_tree();
    fake_create("/mnt/d", FM_DIR); fake_create("/mnt/e", FM_DIR);
    rb_clock_hook = clock_now; rb_capacity_hook = capacity_of; now_value = 1792000000; capacity_value = 1000000;
}
static void run(struct fm_job *j)
{
    for (unsigned i = 0; i < 200000 && (j->state == FM_RUNNING); i++) fm_job_step(j);
    assert(j->state != FM_RUNNING);
}
static struct rb_policy defaults(void) { struct rb_policy p; rb_policy_default(&p); return p; }
static int delete_paths(struct fm_job *j, unsigned flags, uint32_t fixed, const struct rb_policy *policy, unsigned n, ...)
{
    char *paths[8]; va_list ap; va_start(ap, n);
    for (unsigned i = 0; i < n; i++) paths[i] = va_arg(ap, char *);
    va_end(ap);
    fm_job_destroy(j);
    int r = rb_delete_job(j, &fake_vfs, paths, n, policy, fixed, flags);
    if (!r) run(j);
    return r;
}
static unsigned u32(int node, unsigned offset)
{
    const unsigned char *p = fake_nodes[node].data + offset;
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}
static struct fm_model *open_bin(uint32_t fixed)
{
    struct fm_model *m = calloc(1, sizeof(*m)); assert(m);
    fm_init(m, &fake_vfs); m->fixed = fixed;
    assert(!fm_navigate(m, "Recycle Bin") && !strcmp(m->path, FM_RECYCLE));
    return m;
}
static int entry_of(struct fm_model *m, const char *original_name)
{
    for (unsigned i = 0; i < m->count; i++) {
        const struct rb_item *item = rb_entry_item(m, &m->entries[i]); char name[FM_NAME];
        if (item) { rb_item_name(item, name); if (!strcmp(name, original_name)) return (int)i; }
    }
    return -1;
}

static void policy_tests(void)
{
    fixture();
    struct rb_policy p, q; rb_policy_default(&p);
    assert(p.confirm && !p.nuke && !p.independent && p.percent == RB_DEFAULT_PERCENT);
    assert(!rb_policy_load(&fake_vfs, &q) && q.percent == 10 && q.confirm);                /* absent: defaults */
    p.confirm = 0; p.independent = 1; p.nuke = 1; p.percent = 37; p.drive_percent[3] = 5; p.drive_nuke[3] = 1; p.drive_percent[2] = 200;
    assert(!rb_policy_save(&fake_vfs, &p));
    assert(fake_lookup("/system/settings/recycle.cfg") >= 0 && fake_lookup("/system/settings/recycle.cfg.new") < 0);
    assert(!rb_policy_load(&fake_vfs, &q));
    assert(!q.confirm && q.independent && q.nuke && q.percent == 37 && q.drive_percent[3] == 5 && q.drive_nuke[3] && q.drive_percent[2] == 100);
    assert(rb_policy_percent(&q, 'D') == 5 && rb_policy_nuke(&q, 'D') && rb_policy_nuke(&q, 'c') == 0);
    q.independent = 0;
    assert(rb_policy_percent(&q, 'D') == 37 && rb_policy_nuke(&q, 'D') == 1);
    capacity_value = 100000;
    assert(rb_limit(&q, 'C') == 37000);
    rb_capacity_hook = NULL; assert(rb_capacity('C') == RB_ASSUMED_CAPACITY);
    puts("PASS recycle policy: defaults, save/load round trip, clamping, per-drive and global values, limit");
}

static void delete_restore_tests(void)
{
    fixture();
    struct rb_policy policy = defaults();
    fake_create("/Documents/Desktop", FM_DIR); fake_file("/Documents/Desktop/a.txt", 100, 3);
    fake_create("/Documents/Desktop/Proj.v2", FM_DIR); fake_file("/Documents/Desktop/Proj.v2/b.bin", 250, 4);
    fake_create("/Documents/Desktop/Proj.v2/deep", FM_DIR); fake_file("/Documents/Desktop/Proj.v2/deep/c.dat", 50, 5);
    struct fm_job *j = calloc(1, sizeof(*j)); assert(j);
    char file[] = "/Documents/Desktop/a.txt", folder[] = "/Documents/Desktop/Proj.v2";
    assert(rb_classify(&policy, 0, 0, (char *[]){file, folder}, 2) == 2);
    assert(!delete_paths(j, 0, 0, &policy, 2, file, folder));
    assert(j->state == FM_DONE && j->completed == 2 && !j->skipped && j->bin->recycled == 2);
    assert(fake_lookup(file) < 0 && fake_lookup(folder) < 0);
    int a = fake_lookup("/RECYCLED/Dc1.txt"), p = fake_lookup("/RECYCLED/Dc2.v2"), info = fake_lookup("/RECYCLED/INFO2");
    assert(a >= 0 && p >= 0 && info >= 0 && fake_lookup("/RECYCLED/Dc2.v2/deep/c.dat") >= 0);   /* the folder moved whole, with its extension-like suffix */
    assert(fake_lookup("/RECYCLED/INFO2.lck") < 0 && fake_lookup("/RECYCLED/INFO2.tmp") < 0);
    /* INFO2: Windows 9x layout, version 4, 280-byte records. */
    assert(fake_nodes[info].meta.size == RB_HEADER + 2 * RB_RECORD);
    assert(u32(info, 0) == 4 && u32(info, 4) == 3 && u32(info, 8) == 2 && u32(info, 12) == RB_RECORD);
    const unsigned char *rec = fake_nodes[info].data + RB_HEADER;
    assert(!strcmp((const char *)rec, "C:\\Documents\\Desktop\\a.txt") && u32(info, RB_HEADER + 260) == 1 && u32(info, RB_HEADER + 264) == 2 && u32(info, RB_HEADER + 276) == 100);
    uint64_t ft = (uint64_t)u32(info, RB_HEADER + 268) | (uint64_t)u32(info, RB_HEADER + 272) << 32;
    assert(ft == (uint64_t)(1792000000ll + 11644473600ll) * 10000000ull);
    assert(u32(info, RB_HEADER + RB_RECORD + 276) == 300 && !strcmp((const char *)rec + RB_RECORD, "C:\\Documents\\Desktop\\Proj.v2"));
    puts("PASS recycle delete: files and folders move into RECYCLED/D<drive><n>, INFO2 has the Windows 9x records");

    /* The bin as a folder: original names, locations, sizes, dates, sorts. */
    now_value += 60;
    struct fm_model *m = open_bin(0);
    assert(m->count == 2 && m->bin_count == 2);
    int ia = entry_of(m, "a.txt"), ip = entry_of(m, "Proj.v2");
    assert(ia >= 0 && ip >= 0 && m->entries[ip].meta.type == FM_DIR && m->entries[ip].meta.size == 300 && m->entries[ia].meta.size == 100);
    char location[FM_PATH], path[FM_PATH];
    assert(!strcmp(m->entries[ia].name, "Dc1.txt") && !fm_entry_path(m, (unsigned)ia, path) && !strcmp(path, "/RECYCLED/Dc1.txt"));
    rb_item_location(rb_entry_item(m, &m->entries[ia]), location); assert(!strcmp(location, "C:\\Documents\\Desktop"));
    assert(rb_entry_item(m, &m->entries[ia])->deleted == 1792000000);
    fm_sort(m, 4, 0); assert(m->sort == 4); fm_sort(m, 5, 1); assert(m->sort == 5 && m->descending);
    fm_sort(m, 1, 0); assert(m->entries[0].meta.type == FM_DIR);                                   /* folders first, as in Explorer */
    assert(fm_clipboard(m, 0) == -EPERM && fm_create(m, "x", 0) == -EPERM && fm_rename(m, 0, "y") == -EPERM);
    assert(!fm_display_path(location, m->path) && !strcmp(location, "Recycle Bin"));
    assert(!fm_posix_path(location, "recycle bin") && !strcmp(location, FM_RECYCLE));
    assert(!fm_up(m) && !m->path[0]);                                                              /* Up leaves the bin */
    fm_navigate(m, "Recycle Bin"); fm_sort(m, 4, 0); fm_navigate(m, "/Documents"); assert(m->sort == 0);   /* its sort keys do not leak */
    fm_destroy(m); free(m);
    puts("PASS recycle bin view: virtual location, original name/location/date/size, sort keys, read-only");

    /* Restore the file to its place. */
    char stored_a[] = "/RECYCLED/Dc1.txt";
    fm_job_destroy(j);
    assert(!rb_restore_job(j, &fake_vfs, (char *[]){stored_a}, 1)); run(j);
    assert(j->state == FM_DONE && !j->skipped && fake_lookup(file) >= 0 && fake_lookup(stored_a) < 0);
    assert(fake_nodes[fake_lookup(file)].meta.size == 100 && fake_nodes[fake_lookup(file)].data[0] == 3);
    info = fake_lookup("/RECYCLED/INFO2"); assert(fake_nodes[info].meta.size == RB_HEADER + RB_RECORD && u32(info, 8) == 1);
    /* Restore the folder after its parent was removed: the path is recreated. */
    assert(!delete_paths(j, 0, 0, &policy, 1, file));
    int moved = fake_lookup("/Documents/Desktop/a.txt"); assert(moved < 0);
    /* remove Desktop entirely (the file is in the bin, the folder is in the bin) */
    fake_nodes[fake_lookup("/Documents/Desktop")].live = 0;
    char stored_p[] = "/RECYCLED/Dc2.v2", stored_a2[] = "/RECYCLED/Dc3.txt";
    fm_job_destroy(j);
    assert(!rb_restore_job(j, &fake_vfs, (char *[]){stored_p, stored_a2}, 2)); run(j);
    assert(j->state == FM_DONE && j->completed == 2 && fake_lookup("/Documents/Desktop") >= 0);
    assert(fake_lookup("/Documents/Desktop/Proj.v2/deep/c.dat") >= 0 && fake_lookup("/Documents/Desktop/a.txt") >= 0);
    info = fake_lookup("/RECYCLED/INFO2"); assert(fake_nodes[info].meta.size == RB_HEADER && u32(info, 8) == 0);
    puts("PASS recycle restore: back to the original path, missing folders recreated, records removed");

    /* Restoring onto an existing name asks (the engine's replace prompt). */
    assert(!delete_paths(j, 0, 0, &policy, 1, file));
    fake_file(file, 7, 9);                                                    /* a new a.txt appeared meanwhile */
    fm_job_destroy(j);
    assert(!rb_restore_job(j, &fake_vfs, (char *[]){"/RECYCLED/Dc4.txt"}, 1));
    for (unsigned i = 0; i < 1000 && j->state == FM_RUNNING; i++) fm_job_step(j);
    assert(j->state == FM_REPLACE);
    fm_job_answer(j, FM_NO); run(j);
    assert(j->state == FM_DONE && j->skipped == 1 && fake_lookup("/RECYCLED/Dc4.txt") >= 0);   /* kept in the bin */
    info = fake_lookup("/RECYCLED/INFO2"); assert(u32(info, 8) == 1);
    fm_job_destroy(j);
    assert(!rb_restore_job(j, &fake_vfs, (char *[]){"/RECYCLED/Dc4.txt"}, 1));
    for (unsigned i = 0; i < 1000 && j->state == FM_RUNNING; i++) fm_job_step(j);
    assert(j->state == FM_REPLACE); fm_job_answer(j, FM_YES); run(j);
    assert(j->state == FM_DONE && fake_nodes[fake_lookup(file)].meta.size == 100 && fake_lookup("/RECYCLED/Dc4.txt") < 0);
    puts("PASS recycle restore: an existing name raises the replace prompt, No keeps the item in the bin, Yes replaces");
    fm_job_destroy(j); free(j);
}

static void permanent_tests(void)
{
    fixture();
    struct rb_policy policy = defaults();
    struct fm_job *j = calloc(1, sizeof(*j)); assert(j);
    fake_create("/Documents/Desktop", FM_DIR);
    fake_file("/Documents/Desktop/sh.txt", 10, 1);
    char path[] = "/Documents/Desktop/sh.txt";
    assert(rb_classify(&policy, 0, RB_PERMANENT, (char *[]){path}, 1) == 0);
    assert(!delete_paths(j, RB_PERMANENT, 0, &policy, 1, path));                       /* Shift+Delete */
    assert(j->state == FM_DONE && fake_lookup(path) < 0 && fake_lookup("/RECYCLED") < 0);

    fake_file("/Documents/Desktop/nuke.txt", 10, 1);
    policy.nuke = 1;                                                                    /* "Do not move files to the Recycle Bin" */
    char nuke[] = "/Documents/Desktop/nuke.txt";
    assert(!delete_paths(j, 0, 0, &policy, 1, nuke));
    assert(fake_lookup(nuke) < 0 && fake_lookup("/RECYCLED/Dc1.txt") < 0);
    policy.nuke = 0; policy.independent = 1; policy.drive_nuke[2] = 1;                  /* per drive: C: set */
    fake_file(nuke, 10, 1);
    assert(!delete_paths(j, 0, 0, &policy, 1, nuke) && fake_lookup(nuke) < 0 && fake_lookup("/RECYCLED") < 0);
    policy.drive_nuke[2] = 0; policy.drive_nuke[3] = 1;                                  /* D: set, C: not */
    fake_file(nuke, 10, 1);
    assert(!delete_paths(j, 0, 0, &policy, 1, nuke) && fake_lookup("/RECYCLED/Dc1.txt") >= 0);
    puts("PASS recycle permanent: Shift+Delete, global and per-drive 'do not move files', nothing recorded");

    /* Removable media (E: is not a fixed drive) delete at once, as Windows 98 does. */
    policy = defaults();
    fake_file("/mnt/e/usb.txt", 20, 2);
    char usb[] = "/mnt/e/usb.txt";
    assert(rb_classify(&policy, 0, 0, (char *[]){usb}, 1) == 0);
    assert(!delete_paths(j, 0, 0, &policy, 1, usb));
    assert(fake_lookup(usb) < 0 && fake_lookup("/mnt/e/RECYCLED") < 0);
    /* A fixed second drive has its own bin. */
    fake_file("/mnt/d/data.bin", 30, 2);
    char data[] = "/mnt/d/data.bin";
    uint32_t fixed = 1u << 3;
    assert(rb_classify(&policy, fixed, 0, (char *[]){data}, 1) == 1);
    assert(!delete_paths(j, 0, fixed, &policy, 1, data));
    assert(fake_lookup(data) < 0 && fake_lookup("/mnt/d/RECYCLED/Dd1.bin") >= 0 && fake_lookup("/mnt/d/RECYCLED/INFO2") >= 0);
    struct fm_model *m = open_bin(fixed);
    int d = entry_of(m, "data.bin"); assert(d >= 0 && m->entries[d].meta.dev == 'D' && rb_entry_item(m, &m->entries[d])->drive == 'D');
    char where[FM_PATH]; rb_item_location(rb_entry_item(m, &m->entries[d]), where); assert(!strcmp(where, "D:\\"));
    fm_destroy(m); free(m);
    /* Mixed request: the removable item goes for good, the fixed one into the bin. */
    fake_file("/Documents/Desktop/mix.txt", 5, 1); fake_file("/mnt/e/mix2.txt", 5, 1);
    char mixa[] = "/Documents/Desktop/mix.txt", mixb[] = "/mnt/e/mix2.txt";
    assert(!delete_paths(j, 0, fixed, &policy, 2, mixa, mixb));
    assert(fake_lookup(mixa) < 0 && fake_lookup(mixb) < 0 && fake_lookup("/RECYCLED/Dc2.txt") >= 0 && j->completed == 2);
    puts("PASS recycle removable media: permanent delete; a fixed second drive keeps its own bin; mixed requests split per item");

    /* Items inside a bin and drive roots are never recycled again. */
    char inside[] = "/RECYCLED/Dc2.txt";
    assert(!rb_would_recycle(&policy, 0, 0, inside) && !rb_would_recycle(&policy, 0, 0, "/mnt/d/RECYCLED/x") && !rb_would_recycle(&policy, 0, 0, RB_LEGACY_TRASH "/x"));
    fm_job_destroy(j);
    assert(rb_delete_job(j, &fake_vfs, (char *[]){"/"}, 1, &policy, 0, 0) == -EBUSY);
    assert(rb_delete_job(j, &fake_vfs, (char *[]){"/mnt/d"}, 1, &policy, fixed, 0) == -EBUSY);
    assert(fm_hidden("/", "RECYCLED") && fm_hidden("/mnt/d", "recycled") && !fm_hidden("/Documents", "RECYCLED"));
    free(j);
}

static void limit_tests(void)
{
    fixture();
    struct rb_policy policy = defaults();                    /* 10 % */
    capacity_value = 10000;                                  /* limit 1000 bytes */
    struct fm_job *j = calloc(1, sizeof(*j)); assert(j);
    fake_create("/Documents/Desktop", FM_DIR);
    const char *names[] = {"/Documents/Desktop/one.txt", "/Documents/Desktop/two.txt", "/Documents/Desktop/three.txt"};
    for (unsigned i = 0; i < 3; i++) fake_file(names[i], 400, (unsigned char)(i + 1));
    for (unsigned i = 0; i < 2; i++) { now_value += 10; char p[FM_PATH]; strcpy(p, names[i]); assert(!delete_paths(j, 0, 0, &policy, 1, p)); }
    assert(fake_lookup("/RECYCLED/Dc1.txt") >= 0 && fake_lookup("/RECYCLED/Dc2.txt") >= 0);
    now_value += 10;
    char third[FM_PATH]; strcpy(third, names[2]);
    assert(!delete_paths(j, 0, 0, &policy, 1, third));       /* 1200 > 1000: the oldest goes for good */
    assert(j->state == FM_DONE && fake_lookup("/RECYCLED/Dc1.txt") < 0 && fake_lookup("/RECYCLED/Dc2.txt") >= 0 && fake_lookup("/RECYCLED/Dc3.txt") >= 0);
    int info = fake_lookup("/RECYCLED/INFO2"); assert(u32(info, 8) == 2 && fake_nodes[info].meta.size == RB_HEADER + 2 * RB_RECORD);
    puts("PASS recycle size limit: the oldest items are purged when the bin exceeds its percentage");

    /* An item larger than the whole bin is refused, never lost or deleted. */
    fake_file("/Documents/Desktop/huge.dat", 1500, 7);
    char huge[] = "/Documents/Desktop/huge.dat";
    assert(!delete_paths(j, 0, 0, &policy, 1, huge));
    assert(j->state == FM_DONE && j->skipped == 1 && j->bin->refused_count == 1 && j->bin->refused_reason == RB_REFUSE_LARGE);
    assert(fake_lookup(huge) >= 0 && fake_lookup("/RECYCLED/Dc4.dat") < 0 && !strcmp(j->bin->refused_name, "huge.dat"));
    char title[64], text[320]; int icon = 0;
    assert(fm_job_report(j, title, sizeof(title), text, sizeof(text), &icon) && strstr(text, "huge.dat") && strstr(text, "too large") && strstr(text, "Shift"));
    /* A batch that does not fit as a whole: the first items go in, the rest are refused. */
    fake_file("/Documents/Desktop/b1.dat", 600, 1); fake_file("/Documents/Desktop/b2.dat", 600, 2);
    char b1[] = "/Documents/Desktop/b1.dat", b2[] = "/Documents/Desktop/b2.dat";
    assert(!delete_paths(j, 0, 0, &policy, 2, b1, b2));
    assert(fake_lookup(b1) < 0 && fake_lookup(b2) >= 0 && j->bin->refused_count == 1);
    /* Purged records disappear from the list; a bigger limit keeps more. */
    capacity_value = 100000; policy.percent = 50;
    for (unsigned i = 0; i < 3; i++) { char p[FM_PATH]; snprintf(p, sizeof(p), "/Documents/Desktop/f%u.dat", i); fake_file(p, 900, 1); assert(!delete_paths(j, 0, 0, &policy, 1, p)); }
    struct rb_usage u; assert(!rb_usage(&fake_vfs, 'C', &u) && u.items == 5 && u.bytes == 400 + 600 + 2700);
    puts("PASS recycle size limit: an item larger than the bin is refused with a report, partial batches, usage totals");
    fm_job_destroy(j); free(j);
}

static void empty_and_purge_tests(void)
{
    fixture();
    struct rb_policy policy = defaults();
    struct fm_job *j = calloc(1, sizeof(*j)); assert(j);
    fake_create("/Documents/Desktop", FM_DIR);
    for (unsigned i = 0; i < 3; i++) { char p[FM_PATH]; snprintf(p, sizeof(p), "/Documents/Desktop/e%u.txt", i); fake_file(p, 10 + i, 1); assert(!delete_paths(j, 0, 0, &policy, 1, p)); }
    fake_create("/Documents/Desktop/fold", FM_DIR); fake_file("/Documents/Desktop/fold/in.txt", 4, 1);
    assert(!delete_paths(j, 0, 0, &policy, 1, "/Documents/Desktop/fold"));
    fake_file("/RECYCLED/Dc99.zzz", 3, 1);                                         /* an orphan: no record */
    struct fm_model *m = open_bin(0);
    assert(m->count == 5);
    int orphan = -1; for (unsigned i = 0; i < m->count; i++) if (!strcmp(m->entries[i].name, "Dc99.zzz")) orphan = (int)i;
    assert(orphan >= 0 && !rb_entry_item(m, &m->entries[orphan])->original[0]);
    char name[FM_NAME]; rb_item_name(rb_entry_item(m, &m->entries[orphan]), name); assert(!strcmp(name, "Dc99.zzz"));
    /* An orphan cannot be restored, but it can be deleted. */
    fm_job_destroy(j);
    assert(!rb_restore_job(j, &fake_vfs, (char *[]){"/RECYCLED/Dc99.zzz"}, 1)); run(j);
    assert(j->state == FM_DONE && j->skipped == 1 && j->bin->refused_reason == RB_REFUSE_UNKNOWN && fake_lookup("/RECYCLED/Dc99.zzz") >= 0);
    char rt[64], rx[320]; int ri = 0;
    assert(fm_job_report(j, rt, sizeof(rt), rx, sizeof(rx), &ri) && !strcmp(rt, "Restore") && strstr(rx, "Dc99.zzz") && strstr(rx, "cannot be restored"));
    /* Delete in the bin: permanent, records follow. */
    fm_job_destroy(j);
    assert(!rb_purge_job(j, &fake_vfs, (char *[]){"/RECYCLED/Dc1.txt", "/RECYCLED/Dc99.zzz"}, 2)); run(j);
    assert(j->state == FM_DONE && fake_lookup("/RECYCLED/Dc1.txt") < 0 && fake_lookup("/RECYCLED/Dc99.zzz") < 0 && fake_lookup("/RECYCLED/Dc2.txt") >= 0);
    int info = fake_lookup("/RECYCLED/INFO2"); assert(u32(info, 8) == 3);
    assert(rb_purge_job(j, &fake_vfs, (char *[]){"/Documents/Desktop/x"}, 1) == -EINVAL);          /* only bin items */
    fm_destroy(m); free(m);
    /* Empty Recycle Bin. */
    fm_job_destroy(j);
    assert(rb_full(&fake_vfs, 0) == 1);
    assert(!rb_empty_job(j, &fake_vfs, 0)); run(j);
    assert(j->state == FM_DONE && j->completed == 3 && !j->skipped);
    info = fake_lookup("/RECYCLED/INFO2"); assert(fake_nodes[info].meta.size == RB_HEADER && u32(info, 8) == 0);
    for (int i = 0; i < FAKE_NODES; i++) if (fake_nodes[i].live) assert(strncmp(fake_nodes[i].path, "/RECYCLED/", 10) || !strcmp(fake_nodes[i].path, "/RECYCLED/INFO2"));
    assert(rb_full(&fake_vfs, 0) == 0);
    fm_job_destroy(j);
    assert(rb_empty_job(j, &fake_vfs, 0) == -ENOENT);                                              /* already empty */
    /* Numbers keep growing after an empty (Windows keeps counting). */
    fake_file("/Documents/Desktop/after.txt", 1, 1);
    assert(!delete_paths(j, 0, 0, &policy, 1, "/Documents/Desktop/after.txt") && fake_lookup("/RECYCLED/Dc5.txt") >= 0);
    puts("PASS recycle empty and Delete in the bin: permanent, orphans handled, INFO2 reset, numbering continues");
    fm_job_destroy(j); free(j);
}

static void recovery_tests(void)
{
    fixture();
    struct rb_policy policy = defaults();
    struct fm_job *j = calloc(1, sizeof(*j)); assert(j);
    fake_create("/Documents/Desktop", FM_DIR);
    for (unsigned i = 0; i < 3; i++) { char p[FM_PATH]; snprintf(p, sizeof(p), "/Documents/Desktop/r%u.txt", i); fake_file(p, 10, 1); assert(!delete_paths(j, 0, 0, &policy, 1, p)); }
    /* A stored item removed behind the bin's back: its record is dropped on the next listing. */
    fake_nodes[fake_lookup("/RECYCLED/Dc2.txt")].live = 0;
    struct fm_model *m = open_bin(0);
    assert(m->count == 2 && entry_of(m, "r1.txt") < 0 && entry_of(m, "r0.txt") >= 0);
    fm_destroy(m); free(m);
    /* A damaged INFO2 (truncated header): the stored items are adopted, nothing is lost. */
    int info = fake_lookup("/RECYCLED/INFO2"); fake_nodes[info].meta.size = 7;
    m = open_bin(0); assert(m->count == 2);
    for (unsigned i = 0; i < m->count; i++) assert(!rb_entry_item(m, &m->entries[i])->original[0]);
    fm_destroy(m); free(m);
    /* A stale lock left by a crash is broken; a fresh one delays and then gives up. */
    fake_file("/RECYCLED/INFO2.lck", 0, 0); fake_nodes[fake_lookup("/RECYCLED/INFO2.lck")].meta.modified = now_value - 100;
    fake_file("/Documents/Desktop/lock.txt", 1, 1);
    assert(!delete_paths(j, 0, 0, &policy, 1, "/Documents/Desktop/lock.txt") && j->state == FM_DONE && fake_lookup("/RECYCLED/INFO2.lck") < 0);
    fake_file("/Documents/Desktop/lock2.txt", 1, 1);
    fake_file("/RECYCLED/INFO2.lck", 0, 0); fake_nodes[fake_lookup("/RECYCLED/INFO2.lck")].meta.modified = now_value;
    fm_job_destroy(j);
    assert(rb_delete_job(j, &fake_vfs, (char *[]){"/Documents/Desktop/lock2.txt"}, 1, &policy, 0, 0) == 0); run(j);
    assert(j->state == FM_FAILED && j->error == EBUSY && fake_lookup("/Documents/Desktop/lock2.txt") >= 0);   /* the item stays */
    fake_nodes[fake_lookup("/RECYCLED/INFO2.lck")].live = 0;
    /* A path too long for INFO2's 260 bytes is refused. */
    char deep[FM_PATH] = "/Documents/Desktop";
    for (unsigned i = 0; i < 2; i++) { strcat(deep, "/"); for (unsigned k = 0; k < 120; k++) strcat(deep, "d"); fake_create(deep, FM_DIR); }
    strcat(deep, "/x.txt"); fake_file(deep, 3, 1);
    assert(!delete_paths(j, 0, 0, &policy, 1, deep));
    assert(j->skipped == 1 && j->bin->refused_reason == RB_REFUSE_LONG && fake_lookup(deep) >= 0);
    /* Only the completed items are recorded when a job is cancelled. */
    fake_file("/Documents/Desktop/c1.txt", 1, 1); fake_file("/Documents/Desktop/c2.txt", 1, 1); fake_file("/Documents/Desktop/c3.txt", 1, 1);
    fm_job_destroy(j);
    assert(!rb_delete_job(j, &fake_vfs, (char *[]){"/Documents/Desktop/c1.txt", "/Documents/Desktop/c2.txt", "/Documents/Desktop/c3.txt"}, 3, &policy, 0, 0));
    for (unsigned i = 0; i < 100000 && j->completed < 1; i++) fm_job_step(j);
    fm_job_cancel(j); run(j);
    assert(j->state == FM_CANCELLED);
    unsigned moved = 0; for (unsigned i = 1; i <= 3; i++) { char p[FM_PATH]; snprintf(p, sizeof(p), "/Documents/Desktop/c%u.txt", i); moved += fake_lookup(p) < 0; }
    m = open_bin(0); unsigned listed = 0;
    for (unsigned i = 0; i < m->count; i++) { char n[FM_NAME]; rb_item_name(rb_entry_item(m, &m->entries[i]), n); listed += n[0] == 'c' && n[1] >= '1' && n[1] <= '3'; }
    assert(moved >= 1 && moved < 3 && listed == moved);
    fm_destroy(m); free(m);
    puts("PASS recycle recovery: vanished items dropped, damaged INFO2 adopted, stale lock broken, busy lock fails safe, long paths refused, cancel records only finished items");
    fm_job_destroy(j); free(j);
}

static void migration_tests(void)
{
    fixture();
    fake_create(RB_LEGACY_TRASH, FM_DIR);
    fake_file(RB_LEGACY_TRASH "/old.txt", 11, 1); fake_create(RB_LEGACY_TRASH "/Old Folder", FM_DIR); fake_file(RB_LEGACY_TRASH "/Old Folder/inner.bin", 22, 1);
    assert(rb_full(&fake_vfs, 0) == 1);                                                  /* the icon is full before the first migration */
    struct fm_model *m = open_bin(0);
    assert(m->count == 2 && fake_lookup(RB_LEGACY_TRASH) < 0);
    int f = entry_of(m, "old.txt"), d = entry_of(m, "Old Folder"); assert(f >= 0 && d >= 0 && m->entries[d].meta.size == 22 && m->entries[d].meta.type == FM_DIR);
    char where[FM_PATH]; rb_item_location(rb_entry_item(m, &m->entries[f]), where); assert(!strcmp(where, "C:\\Documents\\Desktop"));
    assert(fake_lookup("/RECYCLED/Dc1.txt") >= 0 && fake_lookup("/RECYCLED/Dc2/inner.bin") >= 0);
    fm_destroy(m); free(m);
    /* Restore puts a migrated item back on the Desktop, recreating the folder. */
    fake_create("/Documents/Desktop", FM_DIR); fake_nodes[fake_lookup("/Documents/Desktop")].live = 0;
    struct fm_job *j = calloc(1, sizeof(*j)); assert(j);
    assert(!rb_restore_job(j, &fake_vfs, (char *[]){"/RECYCLED/Dc1.txt"}, 1)); run(j);
    assert(j->state == FM_DONE && fake_lookup("/Documents/Desktop/old.txt") >= 0);
    fm_job_destroy(j); free(j);
    /* The service call alone migrates, too (desktop start). */
    fixture(); fake_create(RB_LEGACY_TRASH, FM_DIR); fake_file(RB_LEGACY_TRASH "/z.txt", 1, 1);
    assert(!rb_migrate(&fake_vfs) && fake_lookup("/RECYCLED/Dc1.txt") >= 0 && rb_full(&fake_vfs, 0));
    puts("PASS recycle migration: the old Trash folder's items become bin records with the Desktop as their location");
}

static void prompt_tests(void)
{
    char title[64], text[320];
    rb_delete_prompt(1, 0, "a.txt", 1, title, sizeof(title), text, sizeof(text));
    assert(!strcmp(title, "Confirm File Delete") && !strcmp(text, "Are you sure you want to send 'a.txt' to the Recycle Bin?"));
    rb_delete_prompt(1, 0, "a.txt", 0, title, sizeof(title), text, sizeof(text));
    assert(!strcmp(text, "Are you sure you want to delete 'a.txt'?"));
    rb_delete_prompt(1, 1, "Dir", 1, title, sizeof(title), text, sizeof(text));
    assert(!strcmp(title, "Confirm Folder Delete") && strstr(text, "remove the folder 'Dir' and move all its contents to the Recycle Bin"));
    rb_delete_prompt(1, 1, "Dir", 0, title, sizeof(title), text, sizeof(text));
    assert(strstr(text, "remove the folder 'Dir' and all its contents?") && !strstr(text, "Recycle"));
    rb_delete_prompt(5, 0, "", 1, title, sizeof(title), text, sizeof(text));
    assert(!strcmp(title, "Confirm Multiple File Delete") && !strcmp(text, "Are you sure you want to send these 5 items to the Recycle Bin?"));
    rb_delete_prompt(5, 0, "", 0, title, sizeof(title), text, sizeof(text));
    assert(!strcmp(text, "Are you sure you want to delete these 5 items?"));
    puts("PASS recycle confirmations: Windows 98 wording for one file, a folder and several items, in the bin and permanent");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    policy_tests(); delete_restore_tests(); permanent_tests(); limit_tests(); empty_and_purge_tests(); recovery_tests(); migration_tests(); prompt_tests();
    fake_assert_clean();
    puts("PASS Recycle Bin model");
    return 0;
}
