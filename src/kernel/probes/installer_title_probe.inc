/* f5-15C: kernel observer of the QEMU `installers` tier, one registered
 * probe per acceptance title (`f5:win32-<alias> run=<id>`, aliases from
 * tests/win32/workloads.json). The runner attaches the title's medium as the
 * CD and a parameter disk carrying CIUKI-ID.TXT (identity, scene, tier) and
 * CIUKI-OP.TXT (the operator program); neither enters the image. The
 * desktop's installer driver (apps/desktop/installer_title.c) runs the
 * original installer through the desktop's input path and reports its
 * position; this file publishes the records of the f5-17A/B serial schema
 * (scripts/test/win32_evidence.py) from facts the kernel verifies itself:
 * the process identity and Win32 exit status, and the C: inventory delta
 * (paths, sizes, SHA-256, shell-link targets) against a snapshot taken
 * after w32srv answered and before the installer started. Window titles,
 * the Uninstall key names and the uninstall command come from the
 * authenticated desktop's read-only w32srv queries, never from the
 * installer. Included by f2_probes_desktop.c (native report hook).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/ata.h>
#include <ciuki/sha256.h>
#include "../../probes/win32_installers/title_controller.h"

#define TITLE_SNAPSHOT_PAGES 96u   /* 16,384 entries of 24 bytes */
#define TITLE_DEPTH 24u
#define TITLE_WALK_PAGES 6u
struct title_frame { struct vfs_find find; unsigned length; bool open; };
static struct {
    bool armed, done, failed, snapshot_ok, ready, installed, uninstall_open, uninstall_done, root_exit_seen;
    const char *name, *failure;
    char scene[32], payload[65], image[65], tier[16];
    uint32_t server, serial, segment, root, ids, driver_failure;
    uint32_t snapshot_phys, snapshot_count, walk_phys;
    struct vfs_table table;
    bool table_ok;
    uint8_t buffer[4096];
} title_obs;
static struct title_entry *title_entries(void) { return P2V(title_obs.snapshot_phys); }
static unsigned title_capacity(void) { return TITLE_SNAPSHOT_PAGES * PAGE_SIZE / sizeof(struct title_entry); }
static void title_fail(const char *reason)
{
    if (!title_obs.failed) title_obs.failure = reason;
    title_obs.failed = true;
}
static const char *title_segment(void) { return title_obs.segment ? "uninstall" : "install"; }
static bool title_terminated(const char *s, unsigned cap)
{
    for (unsigned i = 0; i < cap; i++) if (!s[i]) return true;
    return false;
}
/* Ordered UTF-8 chunks of one long name: the `name` DATA case, 40 bytes each. */
static void title_name(uint32_t id, const char *field, const char *text)
{
    static const char digits[] = "0123456789abcdef";
    unsigned n = (unsigned)strlen(text);
    for (unsigned at = 0; at < n; at += 40) {
        unsigned bytes = n - at < 40 ? n - at : 40;
        char hex[81];
        for (unsigned i = 0; i < bytes; i++) {
            hex[i * 2] = digits[(uint8_t)text[at + i] >> 4];
            hex[i * 2 + 1] = digits[(uint8_t)text[at + i] & 15];
        }
        hex[bytes * 2] = 0;
        rec_emit(title_obs.name, "DATA", "case=name segment=%s id=%u field=%s offset=%u bytes=%u hex=%s",
                 title_segment(), id, field, at, bytes, hex);
    }
}
static void title_segment_record(const char *segment, const char *op)
{
    rec_emit(title_obs.name, "DATA", "case=segment segment=%s op=%s guest_ms=%llu icount=0", segment, op, g_ticks);
}
/* ---- C: inventory ----------------------------------------------------- */
static uint32_t title_stamp(const struct fat_entry *e) { return (uint32_t)e->times.write_date << 16 | e->times.write_time; }
typedef int (*title_visit)(const char *path, const struct fat_entry *entry, void *ctx);
/* Depth-first walk of every C: entry, directories before their contents.
 * Pattern buffers live in kernel pages, never on the kernel stack. */
static int title_walk(title_visit visit, void *ctx)
{
    struct title_frame *stack = P2V(title_obs.walk_phys);
    char *path = (char *)P2V(title_obs.walk_phys) + TITLE_DEPTH * sizeof(struct title_frame);
    _Static_assert(TITLE_DEPTH * sizeof(struct title_frame) + FS_PATH_BYTES * 2 <= TITLE_WALK_PAGES * PAGE_SIZE, "walk pages");
    char *pattern = path + FS_PATH_BYTES;
    memcpy(path, "C:", 3);
    unsigned depth = 0;
    stack[0].length = 2;
    stack[0].open = false;
    for (;;) {
        struct title_frame *f = &stack[depth];
        struct fat_entry entry;
        int e;
        path[f->length] = 0;
        if (!f->open) {
            ksnprintf(pattern, FS_PATH_BYTES, "%s\\*", path);
            e = vfs_find_first(&title_obs.table, pattern, 0x37, &f->find, &entry);
            f->open = true;
        } else e = vfs_find_next(&title_obs.table, &f->find, &entry);
        if (e == -FS_ENOENT) {
            if (!depth) return 0;
            depth--;
            continue;
        }
        if (e) return e;
        if (!strncmp(entry.name, ".", 2) || !strncmp(entry.name, "..", 3) || (entry.attr & FAT_ATTR_VOLUME)) continue;
        unsigned n = (unsigned)strlen(entry.name);
        if (f->length + 1 + n + 3 >= FS_PATH_BYTES) return -FS_ENAMETOOLONG;
        path[f->length] = '\\';
        memcpy(path + f->length + 1, entry.name, n + 1);
        e = visit(path, &entry, ctx);
        if (e) return e;
        if ((entry.attr & FAT_ATTR_DIR) && depth + 1 < TITLE_DEPTH) {
            depth++;
            stack[depth].length = f->length + 1 + n;
            stack[depth].open = false;
        } else if (entry.attr & FAT_ATTR_DIR) return -FS_ENAMETOOLONG;
    }
}
static int title_snapshot_visit(const char *path, const struct fat_entry *entry, void *ctx)
{
    (void)ctx;
    if (title_obs.snapshot_count == title_capacity()) return -FS_ENOSPC;
    title_entries()[title_obs.snapshot_count++] = (struct title_entry){title_path_hash(path), entry->size,
        title_stamp(entry), entry->first, entry->attr};
    return 0;
}
static int title_digest(const char *path, uint32_t size, char hex[65])
{
    int fd = vfs_open(&title_obs.table, path, VFS_READ, VFS_DENY_NONE, 0);
    if (fd < 0) return fd;
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    uint32_t total = 0;
    int e = 0;
    for (;;) {
        size_t n = 0;
        e = vfs_read(&title_obs.table, fd, title_obs.buffer, sizeof(title_obs.buffer), &n);
        if (e || !n) break;
        sha256_update(&ctx, title_obs.buffer, n);
        total += (uint32_t)n;
    }
    int closed = vfs_close(&title_obs.table, fd);
    if (!e && (closed || total != size)) e = closed ? closed : -FS_EIO;
    uint8_t digest[32];
    sha256_final(&ctx, digest);
    sha256_hex(digest, hex);
    return e;
}
static int title_link_text(const char *path, uint32_t size, char *target, unsigned cap)
{
    if (size > sizeof(title_obs.buffer)) return -FS_EFBIG;
    int fd = vfs_open(&title_obs.table, path, VFS_READ, VFS_DENY_NONE, 0);
    if (fd < 0) return fd;
    size_t n = 0;
    int e = vfs_read(&title_obs.table, fd, title_obs.buffer, size, &n), closed = vfs_close(&title_obs.table, fd);
    if (e || closed || n != size) return e ? e : closed ? closed : -FS_EIO;
    const uint8_t *t;
    unsigned length;
    if (!title_link_target(title_obs.buffer, size, &t, &length) || length >= cap) return -FS_EINVAL;
    memcpy(target, t, length);
    target[length] = 0;
    return 0;
}
struct title_delta {
    bool residue; /* uninstall: count what the install added and is still there */
    uint32_t files, directories, links, changed, removed, temp, excluded, system, menu;
    uint64_t bytes;
    int error;
};
static int title_delta_visit(const char *path, const struct fat_entry *entry, void *ctx)
{
    struct title_delta *d = ctx;
    int index = title_find(title_entries(), title_obs.snapshot_count, title_path_hash(path));
    bool dir = entry->attr & FAT_ATTR_DIR;
    if (index >= 0) {
        struct title_entry *old = &title_entries()[index];
        old->attr |= 0x100u; /* seen */
        if (dir || (old->size == entry->size && old->stamp == title_stamp(entry) && old->first == entry->first)) return 0;
        if (d->residue) return 0; /* a pre-existing file rewritten: the host delta asserts it exactly */
        d->changed++;
    }
    int kind = title_path_class(path);
    if (kind == TITLE_PATH_EXCLUDED) { d->excluded++; return 0; }
    if (kind == TITLE_PATH_TEMP) { d->temp++; return 0; }
    if (kind == TITLE_PATH_SYSTEM) d->system++;
    if (title_menu_link(path)) d->menu++;
    if (dir) { d->directories++; if (d->residue) return 0; }
    else d->files++;
    if (d->residue) { if (title_link(path)) d->links++; return 0; }
    uint32_t id = ++title_obs.ids;
    if (dir) {
        rec_emit(title_obs.name, "DATA", "case=installed segment=install id=%u kind=directory", id);
        title_name(id, "path", path);
        return 0;
    }
    char hex[65];
    int e = title_digest(path, entry->size, hex);
    if (e) { d->error = e; return 0; }
    d->bytes += entry->size;
    rec_emit(title_obs.name, "DATA", "case=installed segment=install id=%u kind=file size=%u sha256=%s", id, entry->size, hex);
    title_name(id, "path", path);
    if (title_link(path)) {
        static char target[FS_PATH_BYTES];
        e = title_link_text(path, entry->size, target, sizeof(target));
        if (e == -FS_EINVAL || e == -FS_EFBIG || (!e && !target[0])) {
            /* Not a CiukiOS v3 link (an installer's own MS-SHLLINK file): the
             * same token as the host inventory (win32_inventory.FOREIGN_LINK). */
            memcpy(target, "<foreign>", 10);
            e = 0;
        }
        if (e) { d->error = e; return 0; }
        uint32_t link = ++title_obs.ids;
        d->links++;
        rec_emit(title_obs.name, "DATA", "case=installed segment=install id=%u kind=link", link);
        title_name(link, "path", path);
        title_name(link, "target", target);
    }
    return 0;
}
static int title_delta(struct title_delta *d)
{
    for (unsigned i = 0; i < title_obs.snapshot_count; i++) title_entries()[i].attr &= 0xffu;
    int e = vfs_flush_drive(&title_obs.table, 2);
    if (!e) e = title_walk(title_delta_visit, d);
    if (!e) e = d->error;
    for (unsigned i = 0; i < title_obs.snapshot_count; i++)
        if (!(title_entries()[i].attr & 0x100u)) d->removed++;
    return e;
}
/* ---- driver reports --------------------------------------------------- */
static bool title_ready(void)
{
    title_obs.snapshot_count = 0;
    int e = title_walk(title_snapshot_visit, 0);
    if (e) { rec_emit(title_obs.name, "DATA", "case=snapshot error=%d", e); return false; }
    title_sort(title_entries(), title_obs.snapshot_count);
    for (unsigned i = 1; i < title_obs.snapshot_count; i++)
        if (title_entries()[i].hash == title_entries()[i - 1].hash) return false; /* path-hash collision */
    title_obs.snapshot_ok = true;
    return true;
}
static bool title_started(struct process *server, const struct title_result *r)
{
    struct process *root = proc_find(r->root_pid);
    if (!root || root->ppid != server->pid || root->personality != PROC_WIN32) return false;
    if (r->segment == 1) {
        if (!title_obs.installed || title_obs.uninstall_open) return false;
        title_obs.segment = 1;
        title_obs.uninstall_open = true;
        title_segment_record("uninstall", "begin");
    }
    title_obs.root = r->root_pid;
    title_obs.root_exit_seen = false;
    rec_emit(title_obs.name, "DATA", "case=process segment=%s pid=%u op=start guest_ms=%llu", title_segment(), r->root_pid, g_ticks);
    return true;
}
static bool title_ui(const struct title_result *r)
{
    if (r->ui_owner) {
        struct process *owner = proc_find(r->ui_owner);
        if (owner && owner->personality != PROC_WIN32) return false;
    }
    if (!title_terminated(r->ui_title, sizeof(r->ui_title)) || !title_terminated(r->ui_class, sizeof(r->ui_class))) return false;
    uint32_t id = ++title_obs.ids;
    rec_emit(title_obs.name, "DATA", "case=ui segment=%s step=%u action=%s owner=%u hwnd=%u shot=%u id=%u",
             title_segment(), r->ui_step, title_action_names[r->ui_kind], r->ui_owner, r->ui_hwnd, r->ui_shot ? 1u : 0u, id);
    if (r->ui_title[0]) title_name(id, "title", r->ui_title);
    if (r->ui_class[0]) title_name(id, "class", r->ui_class);
    return true;
}
static bool title_root_exited(const struct title_result *r)
{
    struct process *root = proc_find(title_obs.root);
    if (r->root_pid != title_obs.root || !root || root->state != PROC_ZOMBIE || root->personality != PROC_WIN32) return false;
    title_obs.root_exit_seen = true;
    rec_emit(title_obs.name, "DATA", "case=process segment=%s pid=%u op=exit guest_ms=%llu code=%d",
             title_segment(), title_obs.root, g_ticks, (int32_t)root->raw_exit);
    return true;
}
static void title_keys(const struct title_result *r)
{
    for (unsigned i = 0; i < r->new_keys && i < TITLE_KEYS; i++) {
        const char *key = r->keys[i];
        if (!title_terminated(key, TITLE_KEY_BYTES)) continue;
        bool user = !strncmp(key, "HKCU/", 5);
        static char full[TITLE_KEY_BYTES + 80];
        ksnprintf(full, sizeof(full), "%s\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%s",
                  user ? "HKCU" : "HKLM", user ? key + 5 : key);
        uint32_t id = ++title_obs.ids;
        rec_emit(title_obs.name, "DATA", "case=installed segment=install id=%u kind=registry", id);
        title_name(id, "key", full);
    }
}
static bool title_installed(const struct title_result *r)
{
    if (!title_obs.snapshot_ok || title_obs.installed || title_obs.segment || !title_obs.root_exit_seen) return false;
    struct title_delta d = {0};
    int e = title_delta(&d);
    title_keys(r);
    rec_emit(title_obs.name, "DATA", "case=inventory segment=install files=%u directories=%u links=%u changed=%u removed=%u temp=%u excluded=%u bytes=%llu",
             d.files, d.directories, d.links, d.changed, d.removed, d.temp, d.excluded, d.bytes);
    if (e) { rec_emit(title_obs.name, "DATA", "case=snapshot error=%d", e); return false; }
    rec_emit(title_obs.name, "DATA", "case=install segment=install default_choices=%u system_writes=%u menu_entry=%u uninstall_registered=%u",
             r->choices_default ? 1u : 0u, d.system, d.menu ? 1u : 0u, r->new_keys ? 1u : 0u);
    title_segment_record("install", "end");
    title_obs.installed = true;
    return true;
}
static bool title_uninstalled(const struct title_result *r)
{
    if (!title_obs.uninstall_open || title_obs.uninstall_done || !title_obs.root_exit_seen) return false;
    struct title_delta d = {.residue = true};
    int e = title_delta(&d);
    if (e) { rec_emit(title_obs.name, "DATA", "case=snapshot error=%d", e); return false; }
    rec_emit(title_obs.name, "DATA", "case=residue segment=uninstall files=%u directories=%u links=%u", d.files, d.directories, d.links);
    rec_emit(title_obs.name, "DATA", "case=uninstall segment=uninstall key_removed=%u menu_removed=%u",
             r->key_removed ? 1u : 0u, d.menu ? 0u : 1u);
    title_segment_record("uninstall", "end");
    title_obs.uninstall_done = true;
    return true;
}
/* Called from native_report for `case=w32-title ...` of the armed desktop. */
static bool title_report(struct process *server, const char *line)
{
    uint32_t phase, serial;
    if (!report_unsigned(line, "phase", &phase) || !report_unsigned(line, "serial", &serial)) return false;
    static struct title_result r;
    if (ua_read(server->memory, &r, TITLE_RESULT_VA, sizeof(r)) || !title_valid(&r) ||
        r.phase != phase || r.serial != serial || serial != title_obs.serial + 1 || title_obs.done) return false;
    title_obs.serial = serial;
    switch (phase) {
    case TITLE_READY:
        if (title_obs.ready) return false;
        title_obs.ready = true;
        if (!title_ready()) title_fail("snapshot");
        return true;
    case TITLE_STARTED: return title_obs.ready && title_started(server, &r);
    case TITLE_UI: return title_obs.root && title_ui(&r);
    case TITLE_ROOT_EXITED: return title_root_exited(&r);
    case TITLE_INSTALLED:
        if (!title_installed(&r)) title_fail("inventory");
        return true;
    case TITLE_UNINSTALLED:
        if (!title_uninstalled(&r)) title_fail("uninstall_inventory");
        return true;
    case TITLE_DONE:
        title_obs.driver_failure = r.failure;
        if (r.failure) title_fail(title_failure_names[r.failure]);
        else if (!title_obs.installed || (r.uninstall_planned && r.new_keys && !title_obs.uninstall_done)) title_fail("incomplete");
        title_obs.done = true;
        return true;
    default: return false;
    }
}
/* ---- run parameters on the parameter disk ----------------------------- */
static bool title_value(const char *text, const char *key, char *out, unsigned cap)
{
    unsigned k = (unsigned)strlen(key);
    for (const char *line = text; *line;) {
        const char *end = line;
        while (*end && *end != '\r' && *end != '\n') end++;
        if ((unsigned)(end - line) > k && !strncmp(line, key, k) && line[k] == '=') {
            unsigned n = (unsigned)(end - line) - k - 1;
            if (n >= cap) return false;
            memcpy(out, line + k + 1, n);
            out[n] = 0;
            return true;
        }
        line = end;
        while (*line == '\r' || *line == '\n') line++;
    }
    return false;
}
static bool title_hex64(const char *s)
{
    if (strlen(s) != 64) return false;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return false;
    return true;
}
/* Mount the optical drive like the f4 optical probe; returns its letter. */
static int title_optical(struct storage *s)
{
    for (unsigned slot = 0; slot < 4; slot++) {
        struct ata_device *d = ata_device_get(slot / 2, slot % 2);
        if (!d || !d->atapi) continue;
        for (unsigned i = 3; i < 26; i++)
            if (s->vfs.ro_volumes[i] && s->vfs.ro_volumes[i]->dev == &d->block) return (int)i;
        int e = atapi_ready(d);
        if (!e) e = storage_add_ro(s, slot, &d->block, d->session_lba);
        return e;
    }
    return -FS_ENOENT;
}
/* The parameter disk: the first fixed volume D:..Z: carrying CIUKI-ID.TXT. */
static int title_parameters(char *text, unsigned cap)
{
    for (unsigned drive = 3; drive < 26; drive++) {
        if (!title_obs.table.vfs->volumes[drive]) continue;
        char path[32];
        ksnprintf(path, sizeof(path), "%c:\\%s", 'A' + drive, TITLE_IDENTITY_FILE);
        int fd = vfs_open(&title_obs.table, path, VFS_READ, VFS_DENY_NONE, 0);
        if (fd < 0) continue;
        size_t n = 0;
        int e = vfs_read(&title_obs.table, fd, text, cap - 1, &n), closed = vfs_close(&title_obs.table, fd);
        if (e || closed || n == cap - 1) return -FS_EIO;
        text[n] = 0;
        return (int)drive;
    }
    return -FS_ENOENT;
}
static int probe_title(const char *name)
{
    memset(&title_obs, 0, sizeof(title_obs));
    title_obs.name = name;
    struct storage *storage = storage_get();
    static char identity[1024];
    int run_drive = -1, cd_drive = -1;
    if (!vfs_table_init(&storage->vfs, &title_obs.table, 2)) {
        title_obs.table_ok = true;
        run_drive = title_parameters(identity, sizeof(identity));
    }
    char probe[32] = "", tier[16] = "";
    bool identity_ok = run_drive >= 0 && title_value(identity, "probe", probe, sizeof(probe)) && !strncmp(probe, name, sizeof(probe)) &&
        title_value(identity, "image_sha256", title_obs.image, sizeof(title_obs.image)) && title_hex64(title_obs.image) &&
        title_value(identity, "payload_sha256", title_obs.payload, sizeof(title_obs.payload)) && title_hex64(title_obs.payload) &&
        title_value(identity, "scene", title_obs.scene, sizeof(title_obs.scene)) && title_value(identity, "tier", tier, sizeof(tier));
    if (!identity_ok) {
        rec_emit(name, "BEGIN", 0);
        rec_emit(name, "END", "status=FAIL reason=%s", run_drive < 0 ? "parameter_disk_absent" : "parameter_identity");
        if (title_obs.table_ok) vfs_table_destroy(&title_obs.table);
        return 1;
    }
    rec_emit(name, "BEGIN", "image_sha256=%s", title_obs.image);
    if (strncmp(tier, "installers", sizeof(tier))) {
        /* The f5-17B titles tier shares this probe name; its guest title
         * driver is the f5-17 continuation's work. Never a pass. */
        rec_emit(name, "END", "status=NOT_RUN reason=title_driver_pending");
        vfs_table_destroy(&title_obs.table);
        return 0;
    }
    rec_emit(name, "DATA", "case=identity payload_sha256=%s scene=%s", title_obs.payload, title_obs.scene);
    title_segment_record("install", "begin");
    rec_emit(name, "ARM", "segment=install action=script");
    const struct fb_device *display = fbdev_get();
    struct desktop_ledger initial, final;
    struct proc_ledger before, after;
    struct process *owner = 0, *server = 0;
    uint32_t generation = 0;
    bool pass = false;
    const char *failure = "setup";
    desktop_snapshot(&initial);
    proc_snapshot(&before);
    if (!display->present || (g_boot.flags & CBI_F_SAFE_MODE) || !native_payload_present()) goto finished;
    failure = "write_gate";
    if (storage_enable_write(storage_get(),2)) goto finished;
    failure = "optical";
    cd_drive = title_optical(storage);
    if (cd_drive < 3) goto finished;
    failure = "pages";
    title_obs.snapshot_phys = pmm_alloc_contig(TITLE_SNAPSHOT_PAGES);
    title_obs.walk_phys = pmm_alloc_contig(TITLE_WALK_PAGES);
    if (!title_obs.snapshot_phys || !title_obs.walk_phys) goto finished;
    failure = "desktop_launch";
    memset(&native_reports, 0, sizeof(native_reports));
    if (proc_prepare(proc_supervisor(), &owner)) goto finished;
    proc_publish(owner, 0);
    if (supervisor_spawn_desktop_probe(&server) < 0) goto cleanup;
    server->ppid = owner->pid;
    native_reports.active = true;
    native_reports.server = server->pid;
    title_obs.server = server->pid;
    uint64_t deadline = g_ticks + 10000;
    while (native_live(server) && !native_reports.survivor_stage && g_ticks < deadline) task_sleep_ms(1);
    if (!native_live(server) || native_reports.survivor_stage != 2) goto cleanup;
    failure = "result_page";
    if (ua_map_at(server->memory, TITLE_RESULT_VA, TITLE_RESULT_PAGES * PAGE_SIZE, PROT_READ | PROT_WRITE,
                  PROT_READ | PROT_WRITE, UA_ANON, 0)) goto cleanup;
    char argument[32];
    ksnprintf(argument, sizeof(argument), "%s%c%c", TITLE_FIXTURE_PREFIX, 'a' + run_drive, 'a' + cd_drive);
    title_obs.armed = true;
    native_reports.title = true;
    failure = "gate";
    if (!native_command(server, GATE_SNAPSHOT, &generation) ||
        !native_command_argument(server, GATE_W32_FIXTURE, &generation, argument)) goto cleanup;
    /* The case bound is 600 s on the host (boot included); the operator
     * program's segments total at most 480 s (operator_program.py). */
    deadline = g_ticks + 560000;
    while (native_live(server) && !title_obs.done && g_ticks < deadline) task_sleep_ms(5);
    if (native_reports.invalid) failure = native_reports.invalid_check;
    else if (!title_obs.done) failure = native_live(server) ? "driver_timeout" : "desktop_exit";
    else failure = title_obs.failure;
    pass = title_obs.done && !title_obs.failed && !native_reports.invalid;
cleanup:
    native_reports.active = false;
    native_reports.title = false;
    title_obs.armed = false;
    if (server) {
        static struct ciuki_proc_record children[CIUKI_PROCESS_MAX];
        /* Descendants of the installer tree first (adopted orphans included),
         * then the desktop's own children, then the desktop. */
        unsigned listed = proc_list_snapshot(children, CIUKI_PROCESS_MAX, 0);
        for (unsigned i = 0; i < listed; i++)
            if (children[i].ppid == server->pid) desktop_remove(server, proc_find(children[i].pid));
        desktop_remove(owner, server);
    }
    if (owner) { proc_stop(owner, 0, 0); proc_collect(); }
finished:
    /* Everything the installer, w32srv and the desktop wrote reaches the
     * disk before END: the runner's host inventory reads the overlay. */
    if (title_obs.table_ok && vfs_flush_drive(&title_obs.table, 2) && pass) { pass = false; failure = "final_flush"; }
    if (title_obs.snapshot_phys) for (unsigned i = 0; i < TITLE_SNAPSHOT_PAGES; i++) pmm_free(title_obs.snapshot_phys + i * PAGE_SIZE);
    if (title_obs.walk_phys) for (unsigned i = 0; i < TITLE_WALK_PAGES; i++) pmm_free(title_obs.walk_phys + i * PAGE_SIZE);
    vfs_table_destroy(&title_obs.table);
    desktop_snapshot(&final);
    proc_snapshot(&after);
    bool clean = desktop_ledgers_equal(&initial, &final) && native_proc_equal(&before, &after);
    rec_emit(name, "DATA", "case=ledger_summary clean=%u driver_failure=%u", clean, title_obs.driver_failure);
    if (pass && !clean) { pass = false; failure = "cleanup_ledger"; }
    if (pass) rec_emit(name, "END", "status=PASS");
    else rec_emit(name, "END", "status=FAIL reason=%s", failure ? failure : "unknown");
    return pass ? 0 : 1;
}
/* One registered probe per acceptance title; the registrations
 * (CIUKI_F5_PROBE) follow the #include in f2_probes_desktop.c. */
static int probe_title_diablo(void) { return probe_title("win32-diablo-shareware"); }
static int probe_title_starcraft(void) { return probe_title("win32-sc"); }
static int probe_title_aoe(void) { return probe_title("win32-aoe-trial"); }
static int probe_title_half_life(void) { return probe_title("win32-half-life-uplink"); }
static int probe_title_ut(void) { return probe_title("win32-ut-demo-348"); }
static int probe_title_worms(void) { return probe_title("win32-wa"); }
static int probe_title_heroes3(void) { return probe_title("win32-heroes3-demo"); }
static int probe_title_rct(void) { return probe_title("win32-rct-demo"); }
static int probe_title_jazz2(void) { return probe_title("win32-jazz2-shareware"); }
static int probe_title_ta(void) { return probe_title("win32-ta-demo"); }
