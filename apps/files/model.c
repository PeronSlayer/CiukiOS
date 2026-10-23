#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* SPDX-License-Identifier: MIT
 * Files model. Paths are absolute POSIX paths; FAT names compare without case. */
#include "model.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>

static unsigned fold(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int rb_job_begin(struct fm_job *);
static void rb_job_finish(struct fm_job *);
static void rb_job_free(struct fm_job *);
static int compare(const char *a, const char *b)
{
    while (*a && fold((unsigned char)*a) == fold((unsigned char)*b)) { a++; b++; }
    return (int)fold((unsigned char)*a) - (int)fold((unsigned char)*b);
}
int fm_same_path(const char *a, const char *b) { return !compare(a, b); }
const char *fm_basename(const char *path) { const char *p = strrchr(path, '/'); return p ? p + 1 : path; }

/* fx-17b: /mnt/<letter> is a drive for A:, B: (floppies) and D:..Z:;
 * C: is "/", and /mnt/c is never published by the kernel. */
static int mnt_letter(const char *path)
{
    if (strncmp(path, "/mnt/", 5)) return 0;
    unsigned c = fold((unsigned char)path[5]);
    return c >= 'a' && c <= 'z' && c != 'c' && (!path[6] || path[6] == '/') ? (int)c : 0;
}
int fm_drive_root(const char *path)
{
    return !strcmp(path, "/") || (mnt_letter(path) && !path[6]);
}

int fm_volume(const char *path)
{
    if (!path || path[0] != '/') return 0;
    int c = mnt_letter(path);
    return c ? c - 'a' + 'A' : 'C';
}

unsigned fm_drop_effect(int source_volume, int target_volume, unsigned keys, unsigned allowed)
{
    allowed &= FM_EFFECTS;
    if (!target_volume) return FM_EFFECT_NONE;
    unsigned wanted;
    if ((keys & (FM_KEY_CTRL | FM_KEY_SHIFT)) == (FM_KEY_CTRL | FM_KEY_SHIFT) || (keys & FM_KEY_ALT)) wanted = FM_EFFECT_LINK;
    else if (keys & FM_KEY_SHIFT) wanted = FM_EFFECT_MOVE;
    else if (keys & FM_KEY_CTRL) wanted = FM_EFFECT_COPY;
    else {
        wanted = source_volume && source_volume == target_volume ? FM_EFFECT_MOVE : FM_EFFECT_COPY;
        if (!(allowed & wanted)) wanted = allowed & FM_EFFECT_COPY ? FM_EFFECT_COPY : allowed & FM_EFFECT_MOVE ? FM_EFFECT_MOVE : allowed & FM_EFFECT_LINK;
        return wanted;
    }
    return allowed & wanted ? wanted : FM_EFFECT_NONE;
}

int fm_drop_check(char *const *sources, unsigned count, const char *destination, unsigned effect)
{
    if (!count || !destination || destination[0] != '/') return -EINVAL;
    unsigned already = 0;
    for (unsigned i = 0; i < count; i++) {
        if (sources[i][0] != '/' || !strcmp(sources[i], "/")) return -EINVAL;
        if ((effect & (FM_EFFECT_COPY | FM_EFFECT_MOVE)) && fm_beneath(destination, sources[i])) return -EINVAL;
        char parent[FM_PATH];
        if (!fm_parent(parent, sources[i]) && fm_same_path(parent, destination)) already++;
    }
    return effect == FM_EFFECT_MOVE && already == count ? -EEXIST : 0;
}

int fm_hidden(const char *parent, const char *name)
{
    static const char *const hidden[] = {"dev", "mnt", "tmp"};
    /* G2: the Recycle Bin's folder is a system folder at every volume root. */
    if ((!strcmp(parent, "/") || fm_drive_root(parent)) && !compare(name, RB_DIR)) return 1;
    if (strcmp(parent, "/")) return 0;
    for (unsigned i = 0; i < sizeof(hidden) / sizeof(hidden[0]); i++)
        if (!compare(name, hidden[i])) return 1;
    return 0;
}

/* Presentation conversion only: no filesystem calls or case folding of names.
 * Drive-relative paths (C:folder) are unsupported; C: alone denotes its root. */
int fm_posix_path(char *out, const char *in)
{
    if (!in) return -EINVAL;
    char path[FM_PATH]; size_t used = 0;
    const char *p = in;
    if (!*in || !compare(in, "Computer")) { out[0] = 0; return 0; }
    if (!compare(in, "Recycle Bin") || !compare(in, FM_RECYCLE)) { strcpy(out, FM_RECYCLE); return 0; }
    if (in[0] != '/' && in[1] == ':') {
        unsigned drive = fold((unsigned char)in[0]);
        if (drive < 'a' || drive > 'z' || (in[2] && in[2] != '/' && in[2] != '\\')) return -EINVAL;
        if (drive != 'c') { memcpy(path, "/mnt/", 5); path[5] = (char)drive; used = 6; }
        p += 2;
    } else if (in[0] != '/') return -EINVAL;
    path[used++] = '/';
    for (; *p; p++) {
        char c = *p == '\\' ? '/' : *p;
        if (c == '/' && path[used - 1] == '/') continue;
        if (used + 1 >= sizeof(path)) return -ENAMETOOLONG;
        path[used++] = c;
    }
    while (used > 1 && path[used - 1] == '/') used--;
    path[used] = 0;
    memcpy(out, path, used + 1);
    return 0;
}

int fm_display_path(char *out, const char *in)
{
    char path[FM_PATH];
    int r = fm_posix_path(path, in);
    if (r) return r;
    if (!path[0]) { strcpy(out, "Computer"); return 0; }
    if (!compare(path, FM_RECYCLE)) { strcpy(out, "Recycle Bin"); return 0; }
    const char *p = path + 1; char drive = 'C';
    if (mnt_letter(path)) {
        drive = (char)(fold((unsigned char)path[5]) - 'a' + 'A');
        p = path + 6; if (*p == '/') p++;
    }
    size_t n = strlen(p);
    if (n + 4 > FM_PATH) return -ENAMETOOLONG;
    out[0] = drive; out[1] = ':'; out[2] = '\\';
    for (size_t i = 0; i <= n; i++) out[i + 3] = p[i] == '/' ? '\\' : p[i];
    return 0;
}

/* fx-17: the drive a POSIX path lives on, by the same /mnt/<letter> rule as
 * fm_display_path. Pure string work: no filesystem call on the UI thread. */
char fm_drive_letter(const char *path)
{
    if (!path || path[0] != '/') return 0;
    return (char)fm_volume(path);
}
struct volumes { struct fm_meta meta[26]; uint32_t seen; };
static int collect_volume(const char *name, const struct fm_meta *meta, void *ctx)
{
    struct volumes *v = ctx;
    unsigned letter = fold((unsigned char)name[0]);
    if (meta->type != FM_DIR || !name[0] || name[1] || letter < 'a' || letter > 'z' || letter == 'c') return 0;
    uint32_t bit = 1u << (letter - 'a');
    if (v->seen & bit) return 0;
    v->seen |= bit;
    v->meta[letter - 'a'] = *meta;
    return 0;
}

/* fx-17b: slots lists drives that exist without a mounted medium (a floppy
 * drive with no diskette: Windows 98 My Computer always shows A:). Such an
 * entry has dev 0. Order: A:, B:, C:, then D:..Z:. */
int fm_volumes_slots(const struct fm_vfs *fs, uint32_t slots, fm_emit emit, void *ctx)
{
    struct fm_meta meta;
    int r = fs->stat_path(fs->ctx, "/", &meta);
    if (r) return r;
    struct volumes v = {0};
    r = fs->list(fs->ctx, "/mnt", FM_LIST_NAMES_ONLY, collect_volume, &v);
    if (r && r != -ENOENT) return r;
    for (unsigned letter = 'a'; letter <= 'z'; letter++) {
        char drive[] = {(char)(letter - 'a' + 'A'), ':', 0};
        if (letter == 'c') { if ((r = emit("C:", &meta, ctx))) return r; continue; }
        uint32_t bit = 1u << (letter - 'a');
        if (v.seen & bit) { if ((r = emit(drive, &v.meta[letter - 'a'], ctx))) return r; }
        else if (slots & bit) { struct fm_meta absent = {.type = FM_DIR}; if ((r = emit(drive, &absent, ctx))) return r; }
    }
    return 0;
}
int fm_volumes(const struct fm_vfs *fs, fm_emit emit, void *ctx) { return fm_volumes_slots(fs, 0, emit, ctx); }

int fm_entry_path(const struct fm_model *m, unsigned index, char *out)
{
    if (index >= m->count) return -ENOENT;
    if (m->entries[index].bin) {   /* G2: a Recycle Bin item is addressed by its stored path */
        const struct rb_item *item = rb_entry_item(m, &m->entries[index]);
        char dir[FM_PATH];
        if (!item || rb_dir(item->drive, dir)) return -ENOENT;
        return fm_join(out, dir, item->stored);
    }
    return m->path[0] ? fm_join(out, m->path, m->entries[index].name) : fm_posix_path(out, m->entries[index].name);
}

int fm_valid_name(const char *name)
{
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) return -EINVAL;
    for (const char *p = name; *p; p++)
        if (strchr("\\/:*?\"<>|", *p) || (unsigned char)*p < 32) return -EINVAL;
    size_t n = strlen(name);
    if (name[n - 1] == ' ' || name[n - 1] == '.') return -EINVAL;
    return n >= 256 ? -ENAMETOOLONG : 0;
}

int fm_join(char *out, const char *parent, const char *name)
{
    if (!*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) return -EINVAL;
    size_t n = strlen(parent), m = strlen(name);
    if (n + m + 2 > FM_PATH || m >= FM_NAME) return -ENAMETOOLONG;
    memmove(out, parent, n);
    if (!n || parent[n - 1] != '/') out[n++] = '/';
    memcpy(out + n, name, m + 1);
    return 0;
}

int fm_parent(char *out, const char *path)
{
    size_t n = strlen(path);
    if (n >= FM_PATH) return -ENAMETOOLONG;
    memmove(out, path, n + 1);
    char *slash = strrchr(out, '/');
    if (!slash) return -EINVAL;
    if (slash == out) out[1] = 0; else *slash = 0;
    return 0;
}

int fm_beneath(const char *path, const char *parent)
{
    size_t n = strlen(parent);
    if (n == 1 && parent[0] == '/') return path[0] == '/';
    for (size_t i = 0; i < n; i++) if (fold((unsigned char)path[i]) != fold((unsigned char)parent[i])) return 0;
    return !path[n] || path[n] == '/';
}

static const char *suffix(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && dot != name && dot[1] ? dot + 1 : NULL;
}

static const struct { const char *ext, *type; enum fm_kind kind; } types[] = {
    {"txt", "Text Document", FM_KIND_TEXT}, {"md", "Markdown Document", FM_KIND_TEXT},
    {"log", "Log File", FM_KIND_TEXT}, {"cfg", "Configuration Settings", FM_KIND_TEXT},
    {"ini", "Configuration Settings", FM_KIND_TEXT}, {"conf", "Configuration Settings", FM_KIND_TEXT},
    {"csv", "CSV Document", FM_KIND_TEXT}, {"json", "JSON Document", FM_KIND_TEXT},
    {"lua", "Lua Script", FM_KIND_SCRIPT}, {"c", "C Source File", FM_KIND_SCRIPT},
    {"h", "C Header File", FM_KIND_SCRIPT}, {"asm", "Assembly Source File", FM_KIND_SCRIPT},
    {"bat", "MS-DOS Batch File", FM_KIND_SCRIPT}, {"sh", "Shell Script", FM_KIND_SCRIPT},
    {"htm", "HTML Document", FM_KIND_TEXT}, {"html", "HTML Document", FM_KIND_TEXT},
    {"bmp", "Bitmap Image", FM_KIND_IMAGE}, {"png", "PNG Image", FM_KIND_IMAGE},
    {"xrgb", "XRGB Image", FM_KIND_IMAGE}, {"pcx", "PCX Image", FM_KIND_IMAGE},
    {"mp3", "MP3 Audio", FM_KIND_AUDIO}, {"ogg", "Ogg Vorbis Audio", FM_KIND_AUDIO},
    {"m3u", "Music Playlist", FM_KIND_AUDIO},
    {"wav", "Wave Sound", FM_KIND_AUDIO}, {"mid", "MIDI Sequence", FM_KIND_AUDIO},
    {"com", "MS-DOS Application", FM_KIND_APP}, {"exe", "Application", FM_KIND_APP},
    {"elf", "ELF Program", FM_KIND_FILE}, {"img", "Disk Image", FM_KIND_FILE},
    {"iso", "CD-ROM Image", FM_KIND_FILE}, {"zip", "ZIP Archive", FM_KIND_FILE},
};

enum fm_kind fm_kind(const struct fm_entry *e)
{
    if (e->meta.type == FM_DIR) return FM_KIND_FOLDER;
    if (e->meta.exec) return FM_KIND_APP;
    const char *ext = suffix(e->name);
    if (ext) for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) if (!compare(ext, types[i].ext)) return types[i].kind;
    return FM_KIND_FILE;
}

const char *fm_type_name(const struct fm_entry *e, char *buffer, size_t size)
{
    if (e->meta.type == FM_DIR) return "File Folder";
    if (e->meta.type != FM_FILE) return "Device";
    if (e->meta.exec) return "Application";
    const char *ext = suffix(e->name);
    if (!ext) return "File";
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) if (!compare(ext, types[i].ext)) return types[i].type;
    size_t n = 0;
    for (; ext[n] && n + 6 < size && n < 8; n++) buffer[n] = (char)(ext[n] >= 'a' && ext[n] <= 'z' ? ext[n] - 32 : ext[n]);
    snprintf(buffer + n, size - n, " File");
    return buffer;
}

struct listing { struct fm_entry *entries; unsigned count, capacity; struct rb_item *bin; unsigned bin_count; };
static int rb_list_into(const struct fm_vfs *fs, uint32_t fixed, struct listing *l);
static int collect(const char *name, const struct fm_meta *meta, void *ctx)
{
    struct listing *l = ctx;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    if (!*name || strlen(name) >= FM_NAME || strchr(name, '/')) return -EIO;
    if (l->count == FM_ENTRIES) return -E2BIG;
    if (l->count == l->capacity) {
        unsigned cap = l->capacity ? l->capacity * 2 : 32;
        struct fm_entry *p = realloc(l->entries, cap * sizeof(*p));
        if (!p) return -ENOMEM;
        l->entries = p; l->capacity = cap;
    }
    struct fm_entry *e = &l->entries[l->count++];
    memset(e, 0, sizeof(*e)); strcpy(e->name, name); e->meta = *meta;
    return 0;
}
static int listing(struct fm_vfs *fs, const char *path, struct listing *l)
{
    memset(l, 0, sizeof(*l));
    int r = fs->list(fs->ctx, path, FM_LIST_METADATA, collect, l);
    if (r) { free(l->entries); memset(l, 0, sizeof(*l)); }
    return r;
}

/* Jobs use raw listing(): hiding belongs only to the browser, not to copies
 * or recursive deletion/properties. */
static int browser_listing(struct fm_vfs *fs, const char *path, uint32_t fixed, struct listing *l, uint32_t slots, int show_hidden, int hide_system)
{
    int r;
    if (fm_is_recycle(path)) {
        memset(l, 0, sizeof(*l));
        return rb_list_into(fs, fixed, l);   /* its entries are never hidden */
    }
    if (!path[0]) {
        memset(l, 0, sizeof(*l));
        r = fm_volumes_slots(fs, slots, collect, l);
        if (r) { free(l->entries); memset(l, 0, sizeof(*l)); }
    } else {
        memset(l, 0, sizeof(*l));
        r = fs->list(fs->ctx, path, FM_LIST_NAMES_ONLY, collect, l);
        if (r) { free(l->entries); memset(l, 0, sizeof(*l)); }
    }
    if (!r) {
        unsigned count = 0;
        for (unsigned i = 0; i < l->count; i++)
            if (!fm_hidden(path, l->entries[i].name) &&
                (show_hidden || !(l->entries[i].meta.attributes & 2u)) &&
                (!hide_system || !(l->entries[i].meta.attributes & 4u))) l->entries[count++] = l->entries[i];
        l->count = count;
    }
    return r;
}

struct resolve { const char *want; char *found; };
static int match(const char *name, const struct fm_meta *meta, void *ctx)
{
    struct resolve *r = ctx;
    if (meta->type == FM_DIR && !compare(name, r->want) && !r->found[0]) snprintf(r->found, FM_NAME, "%s", name);
    return 0;
}

/* Lexical normalisation, then each component takes its stored spelling. */
int fm_canonical(const struct fm_vfs *fs, const char *in, char *out)
{
    if (in && fm_is_recycle(in)) { strcpy(out, FM_RECYCLE); return 0; }
    if (!in || in[0] != '/' || strlen(in) >= FM_PATH) return in && in[0] && in[0] != '/' ? -ENOENT : -EINVAL;
    char result[FM_PATH] = "/";
    for (const char *p = in; *p;) {
        while (*p == '/') p++;
        size_t n = strcspn(p, "/");
        if (!n) break;
        char part[FM_NAME];
        if (n >= sizeof(part)) return -ENAMETOOLONG;
        memcpy(part, p, n); part[n] = 0; p += n;
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) { fm_parent(result, result); continue; }
        char found[FM_NAME] = ""; struct resolve r = {part, found};
        int e = fs->list(fs->ctx, result, FM_LIST_NAMES_ONLY, match, &r);
        if (e) return e;
        if (!found[0]) return -ENOENT;
        char next[FM_PATH];
        if ((e = fm_join(next, result, found))) return e;
        strcpy(result, next);
    }
    struct fm_meta meta;
    int e = fs->stat_path(fs->ctx, result, &meta);
    if (e) return e;
    if (meta.type != FM_DIR) return -ENOTDIR;
    strcpy(out, result);
    return 0;
}

void fm_init(struct fm_model *m, const struct fm_vfs *fs) { memset(m, 0, sizeof(*m)); m->fs = *fs; strcpy(m->path, "/"); }

void fm_clipboard_clear(struct fm_model *m)
{
    for (unsigned i = 0; i < m->clipboard_count; i++) free(m->clipboard[i]);
    free(m->clipboard); m->clipboard = NULL; m->clipboard_count = 0; m->cut = 0;
}
void fm_destroy(struct fm_model *m) { free(m->entries); free(m->bin); fm_clipboard_clear(m); memset(m, 0, sizeof(*m)); }

/* G2: in the Recycle Bin the name is the original one and two more keys exist:
 * 4 the original location, 5 the deletion time. */
static int order(const struct fm_model *m, const struct fm_entry *a, const struct fm_entry *b, unsigned column, int descending)
{
    if (a->meta.type == FM_DIR && b->meta.type != FM_DIR) return -1;
    if (a->meta.type != FM_DIR && b->meta.type == FM_DIR) return 1;
    int result = 0;
    if (a->bin && b->bin) {
        const struct rb_item *ia = rb_entry_item(m, a), *ib = rb_entry_item(m, b);
        if (!ia || !ib) return compare(a->name, b->name);
        if (column == 4) result = rb_item_compare_location(ia, ib);
        else if (column == 5) result = ia->deleted < ib->deleted ? -1 : ia->deleted > ib->deleted;
        else if (column == 1) result = a->meta.size < b->meta.size ? -1 : a->meta.size > b->meta.size;
        else if (column == 2) { char x[32], y[32]; result = compare(fm_type_name(a, x, sizeof(x)), fm_type_name(b, y, sizeof(y))); }
        else if (column == 3) result = a->meta.modified < b->meta.modified ? -1 : a->meta.modified > b->meta.modified;
        if (!result) result = compare(rb_item_leaf(ia), rb_item_leaf(ib));
        if (!result) result = (int)a->bin - (int)b->bin;
        return descending ? -result : result;
    }
    if (column == 1) result = a->meta.size < b->meta.size ? -1 : a->meta.size > b->meta.size;
    else if (column == 2) { char x[32], y[32]; result = compare(fm_type_name(a, x, sizeof(x)), fm_type_name(b, y, sizeof(y))); }
    else if (column == 4) result = a->meta.attributes < b->meta.attributes ? -1 : a->meta.attributes > b->meta.attributes;
    else if (column == 3) result = a->meta.modified < b->meta.modified ? -1 : a->meta.modified > b->meta.modified;
    if (!result) result = compare(a->name, b->name);
    if (!result) result = strcmp(a->name, b->name);
    return descending ? -result : result;
}

void fm_sort(struct fm_model *m, unsigned column, int descending)
{
    if (column > 5) return;
    if (column == 5 && !fm_is_recycle(m->path)) column = 0;
    m->sort = column; m->descending = descending;
    if (m->count < 2) return;
    /* Stable bottom-up merge sort; focus/anchor follow entries, not indices. */
    struct fm_entry *tmp = malloc(m->count * sizeof(*tmp));
    if (!tmp) return;
    char active[FM_NAME] = "", anchor[FM_NAME] = "";
    if (m->active < m->count) strcpy(active, m->entries[m->active].name);
    if (m->anchor < m->count) strcpy(anchor, m->entries[m->anchor].name);
    for (unsigned span = 1; span < m->count; span *= 2) for (unsigned start = 0; start < m->count; start += span * 2) {
        unsigned middle = start + span, end = start + span * 2;
        if (middle > m->count) middle = m->count;
        if (end > m->count) end = m->count;
        unsigned left = start, right = middle;
        for (unsigned i = start; i < end; i++)
            tmp[i] = left < middle && (right == end || order(m, &m->entries[left], &m->entries[right], column, descending) <= 0)
                     ? m->entries[left++] : m->entries[right++];
        memcpy(m->entries + start, tmp + start, (end - start) * sizeof(*tmp));
    }
    for (unsigned i = 0; i < m->count; i++) {
        if (!strcmp(active, m->entries[i].name)) m->active = i;
        if (!strcmp(anchor, m->entries[i].name)) m->anchor = i;
    }
    free(tmp);
}

static int load(struct fm_model *m, const char *path)
{
    struct listing l;
    int r = browser_listing(&m->fs, path, m->fixed, &l, m->drive_slots, m->show_hidden, m->hide_system);
    if (r) return r;
    free(m->entries); free(m->bin);
    m->entries = l.entries; m->count = l.count; m->capacity = l.capacity; m->bin = l.bin; m->bin_count = l.bin_count;
    m->active = m->anchor = 0; m->find_length = 0; m->find[0] = 0;
    /* Key 4 changes meaning across the bin boundary; do not carry it over. */
    if (m->sort > 3 && fm_is_recycle(m->path) != fm_is_recycle(path)) m->sort = 0;
    if (path != m->path) snprintf(m->path, sizeof(m->path), "%s", path);
    fm_sort(m, m->sort, m->descending);
    return 0;
}

int fm_navigate(struct fm_model *m, const char *path)
{
    char canonical[FM_PATH];
    int r = fm_posix_path(canonical, path);
    if (r) return r;
    if (!canonical[0] || fm_same_path(canonical, "/mnt")) canonical[0] = 0;
    else if (fm_is_recycle(canonical)) strcpy(canonical, FM_RECYCLE);
    else r = fm_canonical(&m->fs, canonical, canonical);
    if (r) return r;
    if (fm_same_path(canonical, "/mnt")) canonical[0] = 0;
    char previous[FM_PATH]; strcpy(previous, m->path);
    if ((r = load(m, canonical))) return r;
    /* Going up selects the folder that was left, as Explorer does. */
    if (!canonical[0] && previous[0]) {
        char drive[FM_PATH];
        if (!fm_display_path(drive, previous)) { drive[2] = 0; fm_select_name(m, drive, 0); }
    } else if (fm_beneath(previous, canonical) && !fm_same_path(previous, canonical)) {
        const char *rest = previous + strlen(canonical) + (strlen(canonical) > 1);
        char child[FM_NAME]; size_t n = strcspn(rest, "/");
        if (n < sizeof(child)) { memcpy(child, rest, n); child[n] = 0; fm_select_name(m, child, 0); }
    }
    if (m->history_count && !strcmp(m->history[m->history_at], canonical)) return 0;
    m->history_count = m->history_count ? m->history_at + 1 : 0;
    if (m->history_count == FM_HISTORY) { memmove(m->history, m->history + 1, (FM_HISTORY - 1) * FM_PATH); m->history_count--; }
    m->history_at = m->history_count;
    strcpy(m->history[m->history_count++], canonical);
    return 0;
}

int fm_refresh(struct fm_model *m)
{
    struct listing l;
    int r = browser_listing(&m->fs, m->path, m->fixed, &l, m->drive_slots, m->show_hidden, m->hide_system);
    if (r) return r;
    char active[FM_NAME] = "";
    if (m->active < m->count) strcpy(active, m->entries[m->active].name);
    for (unsigned i = 0; i < l.count; i++) for (unsigned j = 0; j < m->count; j++)
        if (!strcmp(l.entries[i].name, m->entries[j].name)) l.entries[i].selected = m->entries[j].selected;
    free(m->entries); free(m->bin);
    m->entries = l.entries; m->count = l.count; m->capacity = l.capacity; m->bin = l.bin; m->bin_count = l.bin_count; m->active = m->anchor = 0;
    for (unsigned i = 0; i < m->count; i++) if (!strcmp(active, m->entries[i].name)) m->active = m->anchor = i;
    fm_sort(m, m->sort, m->descending);
    return 0;
}

int fm_history(struct fm_model *m, int delta)
{
    int n = (int)m->history_at + delta;
    if (n < 0 || (unsigned)n >= m->history_count) return -ENOENT;
    char previous[FM_PATH]; strcpy(previous, m->path);
    int r = load(m, m->history[n]);
    if (r) return r;
    m->history_at = (unsigned)n;
    if (!m->path[0] && previous[0]) {
        char drive[FM_PATH];
        if (!fm_display_path(drive, previous)) { drive[2] = 0; fm_select_name(m, drive, 0); }
    } else if (fm_beneath(previous, m->path) && !fm_same_path(previous, m->path)) {
        const char *rest = previous + strlen(m->path) + (strlen(m->path) > 1);
        char child[FM_NAME]; size_t k = strcspn(rest, "/");
        if (k < sizeof(child)) { memcpy(child, rest, k); child[k] = 0; fm_select_name(m, child, 0); }
    }
    return 0;
}

int fm_up(struct fm_model *m)
{
    if (!m->path[0]) return 0;
    if (fm_drive_root(m->path) || fm_is_recycle(m->path)) return fm_navigate(m, FM_COMPUTER);
    char path[FM_PATH];
    fm_parent(path, m->path);
    return fm_navigate(m, path);
}

void fm_select(struct fm_model *m, unsigned index, unsigned modifiers)
{
    if (index >= m->count) return;
    m->active = index;
    if (modifiers & 1) {
        if (m->anchor >= m->count) m->anchor = index;
        unsigned first = index < m->anchor ? index : m->anchor, last = index > m->anchor ? index : m->anchor;
        for (unsigned i = 0; i < m->count; i++)
            if (!(modifiers & 2) || (i >= first && i <= last)) m->entries[i].selected = i >= first && i <= last;
    } else if (modifiers & 2) { m->entries[index].selected = !m->entries[index].selected; m->anchor = index; }
    else { for (unsigned i = 0; i < m->count; i++) m->entries[i].selected = i == index; m->anchor = index; }
}

void fm_focus(struct fm_model *m, unsigned index) { if (index < m->count) m->active = index; }
void fm_select_all(struct fm_model *m, int invert)
{ for (unsigned i = 0; i < m->count; i++) m->entries[i].selected = invert ? !m->entries[i].selected : 1; }
void fm_select_none(struct fm_model *m) { for (unsigned i = 0; i < m->count; i++) m->entries[i].selected = 0; }

int fm_select_name(struct fm_model *m, const char *name, int add)
{
    for (unsigned i = 0; i < m->count; i++) if (!compare(m->entries[i].name, name)) {
        if (add) { m->entries[i].selected = 1; m->active = i; if (m->anchor >= m->count) m->anchor = i; }
        else fm_select(m, i, 0);
        return (int)i;
    }
    return -ENOENT;
}

unsigned fm_selected(const struct fm_model *m, uint64_t *bytes)
{
    unsigned n = 0; uint64_t total = 0;
    for (unsigned i = 0; i < m->count; i++) if (m->entries[i].selected) {
        n++; if (m->entries[i].meta.type == FM_FILE) total += m->entries[i].meta.size;
    }
    if (bytes) *bytes = total;
    return n;
}

int fm_find(struct fm_model *m, unsigned ch, uint64_t now)
{
    if (ch < 32 || ch > 126 || !m->count) return -EINVAL;
    if (now > m->find_deadline) m->find_length = 0;
    if (m->find_length + 1 >= sizeof(m->find)) m->find_length = 0;
    m->find[m->find_length++] = (char)ch; m->find[m->find_length] = 0;
    m->find_deadline = now + 1000000000u;
    int repeat = 1;
    for (size_t i = 1; i < m->find_length; i++) if (fold((unsigned char)m->find[i]) != fold((unsigned char)m->find[0])) repeat = 0;
    /* Repeating one letter cycles through the names that start with it. */
    size_t need = repeat && m->find_length > 1 ? 1 : m->find_length;
    unsigned start = repeat && m->find_length > 1 ? m->active + 1 : 0;
    for (unsigned k = 0; k < m->count; k++) {
        unsigned i = (start + k) % m->count; size_t j = 0;
        while (j < need && fold((unsigned char)m->find[j]) == fold((unsigned char)m->entries[i].name[j])) j++;
        if (j == need) { fm_select(m, i, 0); return (int)i; }
    }
    return -ENOENT;
}

int fm_clipboard(struct fm_model *m, int cut)
{
    if (!m->path[0] || fm_is_recycle(m->path)) return -EPERM;
    unsigned count = fm_selected(m, NULL);
    if (!count) return -ENOENT;
    char **paths = calloc(count, sizeof(*paths));
    if (!paths) return -ENOMEM;
    unsigned next = 0; int r = 0;
    for (unsigned i = 0; i < m->count && !r; i++) if (m->entries[i].selected) {
        paths[next] = malloc(FM_PATH);
        if (!paths[next]) { r = -ENOMEM; break; }
        r = fm_join(paths[next++], m->path, m->entries[i].name);
    }
    if (r) { for (unsigned i = 0; i < count; i++) free(paths[i]); free(paths); return r; }
    fm_clipboard_clear(m);
    m->clipboard = paths; m->clipboard_count = count; m->cut = cut;
    return 0;
}

int fm_unique(struct fm_model *m, const char *base, const char *ext, char *out)
{
    if (!m->path[0] || fm_is_recycle(m->path)) return -EPERM;
    for (unsigned n = 1; n < 1000; n++) {
        char name[FM_NAME], path[FM_PATH]; struct fm_meta meta;
        if (n == 1) snprintf(name, sizeof(name), "%s%s", base, ext);
        else snprintf(name, sizeof(name), "%s (%u)%s", base, n, ext);
        int r = fm_join(path, m->path, name);
        if (r) return r;
        r = m->fs.stat_path(m->fs.ctx, path, &meta);
        if (r == -ENOENT) { strcpy(out, name); return 0; }
        if (r) return r;
    }
    return -EEXIST;
}

int fm_create(struct fm_model *m, const char *name, int directory)
{
    if (!m->path[0] || fm_is_recycle(m->path)) return -EPERM;
    char path[FM_PATH];
    int r = fm_valid_name(name);
    if (!r) r = fm_join(path, m->path, name);
    if (r) return r;
    if (directory) r = m->fs.mkdir(m->fs.ctx, path);
    else {
        int fd = m->fs.open(m->fs.ctx, path, 1);
        if (fd < 0) return fd;
        r = m->fs.sync(m->fs.ctx, fd);
        int closed = m->fs.close(m->fs.ctx, fd);
        if (!r) r = closed;
        if (r) m->fs.unlink(m->fs.ctx, path);
    }
    if (!r) { r = fm_refresh(m); if (!r) fm_select_name(m, name, 0); }
    return r;
}

int fm_rename(struct fm_model *m, unsigned index, const char *name)
{
    if (!m->path[0] || fm_is_recycle(m->path)) return -EPERM;
    if (index >= m->count) return -ENOENT;
    char source[FM_PATH], target[FM_PATH];
    int r = fm_valid_name(name);
    if (!r) r = fm_join(source, m->path, m->entries[index].name);
    if (!r) r = fm_join(target, m->path, name);
    if (r) return r;
    if (!strcmp(m->entries[index].name, name)) return 0;
    struct fm_meta meta;
    r = m->fs.stat_path(m->fs.ctx, target, &meta);
    if (!r && compare(m->entries[index].name, name)) return -EEXIST;
    if (r && r != -ENOENT) return r;
    r = m->fs.rename(m->fs.ctx, source, target);
    if (!r) { r = fm_refresh(m); if (!r) fm_select_name(m, name, 0); }
    return r;
}

/* ---- incremental jobs ---- */
static void free_frame(struct fm_frame *f) { free(f->entries); memset(f, 0, sizeof(*f)); }
static int cleanup(struct fm_job *j)
{
    int error = 0, r;
    if (j->input >= 0) { r = j->fs.close(j->fs.ctx, j->input); j->input = -1; if (r) error = r; }
    if (j->output >= 0) { r = j->fs.close(j->fs.ctx, j->output); j->output = -1; if (r && !error) error = r; }
    if (*j->temporary) {
        r = j->fs.unlink(j->fs.ctx, j->temporary);
        if (r && r != -ENOENT && !error) error = r;
        if (!r || r == -ENOENT) j->temporary[0] = 0;
    }
    return error;
}
/* fx-17: a medium removed mid-job surfaces as EIO/ENOENT on its paths; its
 * drive root then disappears once the device worker unpublishes it (the
 * USB port scan runs every 250 ms). Name that drive instead of a generic
 * disk error. Only /mnt/<letter> drives are removable; C: never vanishes. */
static int root_gone(struct fm_job *j, const char *path)
{
    char letter = fm_drive_letter(path);
    if (!letter || letter == 'C') return 0;
    char root[8]; snprintf(root, sizeof(root), "/mnt/%c", (char)(letter - 'A' + 'a'));
    struct fm_meta meta;
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        int r = j->fs.stat_path(j->fs.ctx, root, &meta);
        if (r == -ENOENT || r == -ENODEV || r == -ENXIO) return letter;
        if (!r) return 0;
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&delay, &delay) && errno == EINTR) {}
    }
    return letter; /* the root itself keeps failing: the medium is unusable */
}
static void classify(struct fm_job *j, int r)
{
    if (r != -EIO && r != -ENOENT && r != -ENODEV && r != -ENXIO
#ifdef ESTALE
        && r != -ESTALE
#endif
#ifdef ENOMEDIUM
        && r != -ENOMEDIUM
#endif
        ) return;
    const char *source = j->depth ? j->frames[j->depth - 1].source : j->next ? j->sources[j->next - 1] : NULL;
    const char *target = j->depth ? j->frames[j->depth - 1].target : NULL;
    int gone = source ? root_gone(j, source) : 0;
    if (!gone && target && target[0]) gone = root_gone(j, target);
    j->gone = (char)gone;
}
static int fail(struct fm_job *j, int r)
{
    j->error = -r; classify(j, r);
    j->cleanup_error = -cleanup(j); j->state = FM_FAILED; return r;
}
/* The complete copy is already published: failing to remove the source is
 * reported as a kept original, never as a failed copy (Windows 98 Explorer
 * performs a cross-volume move as copy, then delete). */
static void keep_source(struct fm_job *j, const char *source, int r, int folder)
{
    if (folder) j->kept_folders++; else j->kept++;
    if (!j->kept_error) { j->kept_error = -r; snprintf(j->kept_source, sizeof(j->kept_source), "%s", source); }
}
static void pop(struct fm_job *j)
{
    struct fm_frame *f = &j->frames[j->depth - 1];
    if (j->depth == 1 && (j->kind == FM_COPY || j->kind == FM_MOVE) && j->created_count < FM_CREATED && f->target[0])
        snprintf(j->created[j->created_count++], FM_NAME, "%s", fm_basename(f->target));
    free_frame(&j->frames[--j->depth]);
    j->items_done++;
    if (!j->depth) {
        j->completed++;
        /* G2: an item that finished without anything skipped inside it. */
        if (j->bin && j->bin->done && j->next && j->skipped == j->bin->start_skips) j->bin->done[j->next - 1] = 1;
    }
}
static int push(struct fm_job *j, const char *source, const char *target)
{
    if (j->depth == FM_DEPTH) return -ENAMETOOLONG;
    struct fm_frame *f = &j->frames[j->depth++];
    memset(f, 0, sizeof(*f)); strcpy(f->source, source);
    if (target) strcpy(f->target, target);
    return 0;
}

int fm_job_start(struct fm_job *j, const struct fm_vfs *fs, enum fm_job_kind kind, char *const *paths, unsigned n, const char *destination)
{
    memset(j, 0, sizeof(*j)); j->input = j->output = -1; j->fs = *fs; j->kind = kind;
    j->chunk = FM_CHUNK;
    if (!n || n > FM_ENTRIES) return -EINVAL;
    if ((kind == FM_COPY || kind == FM_MOVE) && (!destination || strlen(destination) >= FM_PATH)) return -EINVAL;
    if (destination) strcpy(j->destination, destination);
    j->sources = calloc(n, sizeof(*j->sources));
    if (!j->sources) return -ENOMEM;
    j->totals = calloc(n, sizeof(*j->totals));
    if (!j->totals) { fm_job_destroy(j); return -ENOMEM; }
    j->count = n;
    for (unsigned i = 0; i < n; i++) {
        if (paths[i][0] != '/' || strlen(paths[i]) >= FM_PATH) { fm_job_destroy(j); return -EINVAL; }
        j->sources[i] = malloc(strlen(paths[i]) + 1);
        if (!j->sources[i]) { fm_job_destroy(j); return -ENOMEM; }
        strcpy(j->sources[i], paths[i]);
        /* Never copy or move a folder into itself or a descendant. */
        if ((kind == FM_COPY || kind == FM_MOVE) && fm_beneath(destination, paths[i])) { fm_job_destroy(j); return -EINVAL; }
        if (!strcmp(paths[i], "/") && kind != FM_PROPERTIES) { fm_job_destroy(j); return -EBUSY; }
    }
    if (kind == FM_COPY || kind == FM_MOVE) {
        j->buffer = malloc(FM_WRITE_BATCH);
        if (!j->buffer) { fm_job_destroy(j); return -ENOMEM; }
    }
    j->counting = kind != FM_PROPERTIES;
    j->state = FM_RUNNING;
    return 0;
}

static void metadata(struct fm_job *j, struct fm_frame *f)
{
    if (!j->fs.metadata) { j->metadata_warnings++; return; }
    if (j->fs.metadata(j->fs.ctx, f->target, &f->meta)) j->metadata_warnings++;
}

static int begin_file(struct fm_job *j, struct fm_frame *f)
{
    j->input = j->fs.open(j->fs.ctx, f->source, 0);
    if (j->input < 0) { int r = j->input; j->input = -1; return r; }
    char parent[FM_PATH];
    fm_parent(parent, f->target);
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        char name[64]; snprintf(name, sizeof(name), ".ciuki-copy-%u.tmp", ++j->serial);
        int r = fm_join(j->temporary, parent, name);
        if (r) { j->temporary[0] = 0; return r; }
        j->output = j->fs.open(j->fs.ctx, j->temporary, 1);
        if (j->output >= 0) break;
        r = j->output; j->output = -1; j->temporary[0] = 0;
        if (r != -EEXIST) return r;
    }
    if (j->output < 0) return -EEXIST;
    j->used = j->written = 0; j->eof = 0; j->file_bytes = 0; f->phase = 1;
    return 0;
}

/* Copying beside the source picks "Copy of NAME", as Explorer does. */
static int copy_name(struct fm_job *j, const char *source, char *target)
{
    const char *base = fm_basename(source);
    for (unsigned n = 1; n < 1000; n++) {
        char name[FM_NAME]; struct fm_meta meta;
        if (n == 1) snprintf(name, sizeof(name), "Copy of %s", base);
        else snprintf(name, sizeof(name), "Copy (%u) of %s", n, base);
        int r = fm_join(target, j->destination, name);
        if (r) return r;
        r = j->fs.stat_path(j->fs.ctx, target, &meta);
        if (r == -ENOENT) return 0;
        if (r) return r;
    }
    return -EEXIST;
}

/* fx-19: a multi-item move to the Trash picks a free name per item, as
 * fm_unique does for the single-item desktop Delete ("name (2)"). */
static int trash_target(struct fm_job *j, const char *source, char *target)
{
    const char *base = fm_basename(source);
    for (unsigned n = 1; n < 1000; n++) {
        char name[FM_NAME]; struct fm_meta meta;
        if (n == 1) snprintf(name, sizeof(name), "%s", base);
        else snprintf(name, sizeof(name), "%s (%u)", base, n);
        int r = fm_join(target, j->destination, name);
        if (r) return r;
        r = j->fs.stat_path(j->fs.ctx, target, &meta);
        if (r == -ENOENT) return 0;
        if (r) return r;
    }
    return -EEXIST;
}

/* Count using directory metadata, never opening/sniffing file contents.
 * The same bounded stack is reused by the mutation pass. */
static int count_step(struct fm_job *j)
{
    if (!j->depth) {
        if (j->next == j->count) {
            j->next = 0;
            if (j->bin) { int r = rb_job_begin(j); if (r) return fail(j, r); }   /* G2: sizes known, reserve names */
            j->counting = 0; return 1;
        }
        int r = push(j, j->sources[j->next++], NULL);
        if (r) return fail(j, r);
    }
    struct fm_frame *f = &j->frames[j->depth-1];
    struct fm_totals *t = &j->totals[j->next-1];
    if (!f->phase) {
        if (j->depth == 1) {
            int r = j->fs.stat_path(j->fs.ctx, f->source, &f->meta);
            if (r) return fail(j, r);
        }
        if (f->meta.type == FM_OTHER) return fail(j, -ENOTSUP);
        j->total_items++; t->items++;
        if (f->meta.type == FM_FILE) {
            j->total_bytes += f->meta.size; t->bytes += f->meta.size;
            free_frame(&j->frames[--j->depth]); return 1;
        }
        struct listing l = {0};
        int r = j->fs.list(j->fs.ctx, f->source, FM_LIST_METADATA, collect, &l);
        if (r) { free(l.entries); return fail(j, r); }
        f->entries = l.entries; f->count = l.count; f->phase = 2;
        return 1;
    }
    if (f->next < f->count) {
        struct fm_entry *e = &f->entries[f->next++]; char source[FM_PATH];
        int r = fm_join(source, f->source, e->name);
        if (!r) r = push(j, source, NULL);
        if (r) return fail(j, r);
        j->frames[j->depth-1].meta = e->meta;
    } else free_frame(&j->frames[--j->depth]);
    return 1;
}

static int interrupted(void)
{
    struct timespec delay = {.tv_sec=0, .tv_nsec=1000000};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
    return 1;
}

static int job_step(struct fm_job *j);
int fm_job_step(struct fm_job *j)
{
    int r = job_step(j);
    /* G2: the Recycle Bin's records follow the items that really moved. */
    if (j->bin && j->state != FM_RUNNING && j->state != FM_REPLACE && j->state != FM_IDLE) rb_job_finish(j);
    return r;
}
static int job_step(struct fm_job *j)
{
    if (j->state != FM_RUNNING) return 0;
    if (j->cancel) {
        int r = cleanup(j);
        j->cleanup_error = -r; j->state = r ? FM_FAILED : FM_CANCELLED;
        if (r) j->error = -r;
        return r;
    }
    if (j->counting) return count_step(j);
    if (!j->depth) {
        if (j->next == j->count) { j->state = FM_DONE; return 0; }
        const char *source = j->sources[j->next++];
        char target[FM_PATH] = ""; int r = 0;
        if (j->bin) {
            j->bin->start_skips = j->skipped;
            if (j->bin->refused && j->bin->refused[j->next - 1]) {   /* not for the bin, not deleted either */
                j->skipped++; j->completed++; j->bytes += j->totals[j->next - 1].bytes; j->items_done += j->totals[j->next - 1].items;
                return 1;
            }
        }
        if ((j->kind == FM_COPY || j->kind == FM_MOVE) && !j->destination_known) {
            struct fm_meta where;
            j->destination_known = j->fs.stat_path(j->fs.ctx, j->destination, &where) ? -1 : 1;
            if (j->destination_known > 0) j->destination_dev = where.dev;
        }
        if (j->kind == FM_COPY || j->kind == FM_MOVE) {
            if (j->bin && j->bin->target && j->bin->target[j->next - 1]) { snprintf(target, sizeof(target), "%s", j->bin->target[j->next - 1]); r = 0; }
            else r = j->trash == 2 ? trash_target(j, source, target) : fm_join(target, j->destination, j->trash ? j->trash_name : fm_basename(source));
            if (!r && fm_same_path(target, source)) {
                if (j->kind == FM_MOVE) { j->completed++; j->bytes += j->totals[j->next-1].bytes; j->items_done += j->totals[j->next-1].items; return 1; }
                r = copy_name(j, source, target);
            }
        }
        if (!r) r = push(j, source, target);
        if (r) return fail(j, r);
    }
    struct fm_frame *f = &j->frames[j->depth - 1];
    int r;
    if (!f->phase) {
        r = j->fs.stat_path(j->fs.ctx, f->source, &f->meta);
        if (r) return fail(j, r);
        if (f->meta.type == FM_OTHER) return fail(j, -ENOTSUP);
        if (j->kind == FM_DELETE && j->bin && j->depth == 1 && j->bin->target && j->bin->target[j->next - 1]) {
            /* G2: into the Recycle Bin: a rename inside the volume, never a copy. */
            r = j->fs.rename(j->fs.ctx, f->source, j->bin->target[j->next - 1]);
            if (r) return fail(j, r);
            if (f->meta.type == FM_FILE) { j->files++; j->bytes += f->meta.size; }
            else { j->folders++; j->bytes += j->totals[j->next - 1].bytes; j->items_done += j->totals[j->next - 1].items - 1; }
            j->bin->recycled++;
            pop(j); return 1;
        }
        if (j->kind == FM_PROPERTIES) {
            if (f->meta.type == FM_FILE) {
                j->allocated += f->meta.allocated;
                if (!f->meta.allocated_known) j->allocated_unknown = 1;
            }
            if (j->depth == 1 && j->next == 1) j->property_meta = f->meta;
            if (f->meta.type == FM_FILE) { j->files++; j->bytes += f->meta.size; pop(j); return 1; }
            if (j->depth > 1) j->folders++;
        } else if (j->kind == FM_DELETE && f->meta.type == FM_FILE) {
            r = j->fs.unlink(j->fs.ctx, f->source);
            if (r) return fail(j, r);
            j->files++; pop(j); return 1;
        } else if (j->kind == FM_COPY || j->kind == FM_MOVE) {
            r = j->fs.stat_path(j->fs.ctx, f->target, &f->existing);
            f->exists = !r;
            if (r && r != -ENOENT) return fail(j, r);
            if (f->exists && f->existing.dev == f->meta.dev && f->existing.ino == f->meta.ino) return fail(j, -EINVAL);
            if (f->exists && f->existing.type != f->meta.type) return fail(j, f->meta.type == FM_DIR ? -ENOTDIR : -EISDIR);
            if (f->exists && !f->approved && !j->yes_all) { j->prompt_dir = f->meta.type == FM_DIR; j->state = FM_REPLACE; return 0; }
            if (j->kind == FM_MOVE && (f->meta.type == FM_FILE || (j->depth == 1 && !f->exists))) {
                r = j->fs.rename(j->fs.ctx, f->source, f->target);
                if (!r) {
                    if (f->meta.type == FM_FILE) { j->files++; j->bytes += f->meta.size; }
                    else { j->folders++; j->bytes += j->totals[j->next-1].bytes; j->items_done += j->totals[j->next-1].items-1; }
                    pop(j); return 1;
                }
                /* A read-only source volume may refuse the rename before
                 * it compares volumes; another volume still copies first. */
                int other = j->destination_known > 0 && f->meta.dev != j->destination_dev;
                if (r != -EXDEV && !(other && (r == -EROFS || r == -EACCES || r == -EPERM))) return fail(j, r);
            }
            if (f->meta.type == FM_FILE) { r = begin_file(j, f); if (r) return fail(j, r); return 1; }
            if (!f->exists) { r = j->fs.mkdir(j->fs.ctx, f->target); if (r) return fail(j, r); }
            j->folders++;
        }
        struct listing l;
        r = listing(&j->fs, f->source, &l);
        if (r) return fail(j, r);
        f->entries = l.entries; f->count = l.count; f->phase = 2;
        return 1;
    }
    if (f->phase == 1) {
        unsigned capacity = j->batch && j->batch <= FM_WRITE_BATCH ? j->batch : FM_WRITE_BATCH;
        if (j->written < j->used && (j->eof || j->used >= capacity || j->written)) {
            r = j->fs.write(j->fs.ctx, j->output, j->buffer + j->written, j->used - j->written);
            if (r == -EINTR) return interrupted();
            if (r <= 0) return fail(j, r ? r : -EIO);
            if ((unsigned)r > j->used - j->written) return fail(j, -EIO);
            j->written += (unsigned)r; j->bytes += (unsigned)r; j->file_bytes += (unsigned)r;
            if (j->written == j->used) j->used = j->written = 0;
            return 1;
        }
        if (!j->eof) {
            unsigned chunk = j->chunk && j->chunk <= FM_CHUNK ? j->chunk : FM_CHUNK;
            if (chunk > capacity - j->used) chunk = capacity - j->used;
            r = j->fs.read(j->fs.ctx, j->input, j->buffer + j->used, chunk);
            if (r == -EINTR) return interrupted();
            if (r < 0) return fail(j, r);
            if ((unsigned)r > chunk) return fail(j, -EIO);
            if (r) { j->used += (unsigned)r; return 1; }
            j->eof = 1;
            if (j->used) return 1;
        }
        r = j->fs.sync(j->fs.ctx, j->output);
        if (r) return fail(j, r);
        r = j->fs.close(j->fs.ctx, j->output); j->output = -1;
        if (r) return fail(j, r);
        r = j->fs.close(j->fs.ctx, j->input); j->input = -1;
        if (r) return fail(j, r);
        struct fm_meta current;
        r = j->fs.stat_path(j->fs.ctx, f->target, &current);
        if (!f->exists && !r) { f->exists = 1; f->existing = current; j->prompt_dir = 0; j->state = FM_REPLACE; f->phase = 3; return 0; }
        if (r && r != -ENOENT) return fail(j, r);
        f->phase = 3;
    }
    if (f->phase == 3) {
        /* POSIX rename publishes the complete copy, replacing atomically. */
        r = j->fs.rename(j->fs.ctx, j->temporary, f->target);
        if (r) return fail(j, r);
        j->temporary[0] = 0;
        metadata(j, f);
        if (j->kind == FM_MOVE) { r = j->fs.unlink(j->fs.ctx, f->source); if (r) keep_source(j, f->source, r, 0); }
        j->files++; pop(j);
        return 1;
    }
    if (f->next < f->count) {
        struct fm_entry *e = &f->entries[f->next++];
        char source[FM_PATH], target[FM_PATH] = "";
        r = fm_join(source, f->source, e->name);
        if (!r && (j->kind == FM_COPY || j->kind == FM_MOVE)) r = fm_join(target, f->target, e->name);
        if (!r) r = push(j, source, target);
        if (r) return fail(j, r);
        return 1;
    }
    if (j->kind == FM_DELETE || (j->kind == FM_MOVE && !j->skip_seen)) {
        r = j->fs.rmdir(j->fs.ctx, f->source);
        /* A folder whose files were kept, or whose medium refuses removal,
         * stays beside its complete copy. */
        if (r && j->kind == FM_MOVE && (j->kept || j->kept_folders || r == -EROFS || r == -EACCES || r == -EPERM || r == -EBUSY))
            keep_source(j, f->source, r, 1);
        else if (r) return fail(j, r);
        if (j->kind == FM_DELETE) j->folders++;
    }
    if (j->kind == FM_COPY || j->kind == FM_MOVE) metadata(j, f);
    pop(j);
    return 1;
}

void fm_job_answer(struct fm_job *j, enum fm_replace answer)
{
    if (j->state != FM_REPLACE) return;
    j->state = FM_RUNNING;
    if (answer == FM_CANCEL) { j->cancel = 1; return; }
    if (answer == FM_NO) {
        int r = cleanup(j);
        if (r) { fail(j, r); return; }
        j->skip_seen = 1; j->skipped++;
        j->frames[j->depth - 1].target[0] = 0;
        pop(j);
        return;
    }
    j->frames[j->depth - 1].approved = 1;
    if (answer == FM_YES_ALL) j->yes_all = 1;
}

void fm_job_cancel(struct fm_job *j) { j->cancel = 1; if (j->state == FM_REPLACE) j->state = FM_RUNNING; }

void fm_job_destroy(struct fm_job *j)
{
    if (j->fs.close) cleanup(j);
    for (unsigned i = 0; i < j->depth; i++) free_frame(&j->frames[i]);
    for (unsigned i = 0; i < j->count; i++) free(j->sources ? j->sources[i] : NULL);
    free(j->sources);
    free(j->totals);
    free(j->buffer);
    rb_job_free(j);
    memset(j, 0, sizeof(*j)); j->input = j->output = -1;
}

#include "recycle.inc"
#include "cleanup.inc"
