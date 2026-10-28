/* SPDX-License-Identifier: MIT
 * fx-exeicons: the icon a file shows by itself, as Windows 98's shell picks
 * it (docs/design/file-icons.md), for every libdeskui client and the desktop.
 *
 * Resolution (du_file_icon_resolve) maps a path to an icon location: a
 * PE/NE program or library, .ico/.cur/.ani shows its own first icon;
 * extensionless files in /Programs use the shared program:<name> Tango map; a
 * shortcut (.lnk: CiukiOS CIUKLNK3 or MS-SHLLINK) its IconLocation or else
 * its target's icon; a .pif the icon of its "WINDOWS VMM 4.0" section, else
 * the MS-DOS program icon; an Internet shortcut its IconFile; a read-only or
 * system folder its desktop.ini [.ShellClassInfo] icon; a drive root its
 * AUTORUN.INF [autorun] icon=. Windows paths (C:\..., D:\... = /mnt/d) and
 * paths relative to the naming file are mapped to POSIX paths; a missing
 * component is found case-insensitively, as on FAT and ISO 9660.
 *
 * Extraction uses the bounded iconx core (iconx.h). The cache keeps the
 * pixels per location and size together with the subject's modification
 * time; a lookup with a different time extracts again. With a worker
 * (du_file_icons_start) a miss is queued and NULL is returned until the
 * worker publishes the pixels, bumps du_file_icons_generation() and calls
 * the wake function: the caller draws the type's default meanwhile, and a
 * slow medium (a CD spinning up) never stalls the user interface. Without
 * a worker a lookup extracts at once (tools and host harnesses).
 *
 * Pixel buffers are freed only when an entry unused for FI_GRACE_NS is
 * evicted, so a pointer a caller obtained in its current paint stays valid. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "deskui.h"
#include "iconx.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __linux__ /* The native host's unistd enums follow different _SC values. */
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#endif
#include <unistd.h>
#if defined(__ciuki__) || defined(__i386__)
#include <ciuki/fileinfo.h>
#define FI_ATTRIBUTES 1
#endif

#define FI_LINK_BYTES (64u * 1024u)     /* a .lnk, .pif, .url, desktop.ini or AUTORUN.INF read whole */
#define FI_LINK_DEPTH 4u
#define FI_CAP 512u                     /* cached locations, bounded as Explorer bounds its icon cache */
#define FI_BYTES (4u << 20)             /* pixel memory of the cache */
#define FI_GRACE_NS 5000000000ull
#define FI_CI_ENTRIES 1024u             /* directory entries scanned for a case-insensitive match */

static char root[DU_FILE_ICON_PATH];   /* host harnesses: every POSIX path lives under this prefix */

int du_file_icons_root(const char *prefix)
{
    if (prefix && strlen(prefix) >= sizeof(root)) return -1;
    snprintf(root, sizeof(root), "%s", prefix ? prefix : "");
    size_t n = strlen(root);
    while (n && root[n - 1] == '/') root[--n] = 0;
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ---- paths ------------------------------------------------------------------ */
/* Normalize an absolute POSIX path: no "." or ".." components, no repeated
 * or trailing slash. 0, or -1 when it would leave the root or not fit. */
static int normalize(const char *in, char *out, size_t cap)
{
    size_t used = 0;
    if (in[0] != '/' || cap < 2) return -1;
    out[used++] = '/';
    for (const char *p = in; *p;) {
        while (*p == '/') p++;
        const char *end = p;
        while (*end && *end != '/') end++;
        size_t len = (size_t)(end - p);
        if (!len) break;
        if (len == 1 && p[0] == '.') { p = end; continue; }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (used == 1) return -1;
            while (used > 1 && out[used - 1] != '/') used--;
            if (used > 1) used--;
            p = end; continue;
        }
        if (used > 1) { if (used + 1 >= cap) return -1; out[used++] = '/'; }
        if (used + len >= cap) return -1;
        memcpy(out + used, p, len); used += len;
        p = end;
    }
    out[used] = 0;
    return 0;
}

static void parent_of(const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash && slash != out) *slash = 0; else snprintf(out, cap, "/");
}

/* A Windows or POSIX path named by a link, PIF, INI or icon location ->
 * POSIX. base: the folder relative names start from. */
int du_file_icon_native(const char *in, const char *base, char *out, size_t cap)
{
    static const struct { const char *name, *value; } vars[] = {
        {"SystemRoot", "C:\\WINDOWS"}, {"windir", "C:\\WINDOWS"}, {"SystemDrive", "C:"},
        {"ProgramFiles", "C:\\Program Files"}, {"CommonProgramFiles", "C:\\Program Files\\Common Files"}};
    char text[DU_FILE_ICON_PATH * 2], joined[DU_FILE_ICON_PATH * 2];
    size_t used = 0;
    if (!in || !out || !cap) return -1;
    while (*in == ' ' || *in == '\t' || *in == '"') in++;
    for (const char *p = in; *p; p++) {
        if (*p == '%') {
            const char *end = strchr(p + 1, '%');
            size_t k = 0, n = end ? (size_t)(end - p - 1) : 0;
            for (; end && k < sizeof(vars) / sizeof(*vars); k++)
                if (strlen(vars[k].name) == n && !strncasecmp(p + 1, vars[k].name, n)) break;
            if (!end || k == sizeof(vars) / sizeof(*vars)) return -1;   /* an unknown variable */
            for (const char *v = vars[k].value; *v; v++) {
                if (used + 1 >= sizeof(text)) return -1;
                text[used++] = *v == '\\' ? '/' : *v;
            }
            p = end; continue;
        }
        if (used + 1 >= sizeof(text)) return -1;
        text[used++] = *p == '\\' ? '/' : *p;
    }
    while (used && (text[used - 1] == ' ' || text[used - 1] == '\t' || text[used - 1] == '"')) used--;
    text[used] = 0;
    if (!used) return -1;
    const char *drive_root = "";
    if (((text[0] | 32) >= 'a' && (text[0] | 32) <= 'z') && text[1] == ':') {
        char letter = (char)(text[0] | 32);
        if (text[2] && text[2] != '/') return -1;   /* drive-relative "D:name" */
        int n = letter == 'c' ? snprintf(joined, sizeof(joined), "/%s", text + 2)
                              : snprintf(joined, sizeof(joined), "/mnt/%c/%s", letter, text + 2);
        if (n < 0 || (size_t)n >= sizeof(joined)) return -1;
    } else if (text[0] == '/' && text[1] == '/') return -1;          /* UNC: no network names */
    else if (text[0] == '/') {
        /* A POSIX path from a CiukiOS link, or a Windows "\path" on the
         * base's drive: both start at the root here, except on /mnt/x. */
        if (base && !strncmp(base, "/mnt/", 5) && base[5] && (!base[6] || base[6] == '/') && strncmp(text, "/mnt/", 5)) {
            char drive[8] = "/mnt/x"; drive[5] = base[5]; drive_root = drive;
            int n = snprintf(joined, sizeof(joined), "%s%s", drive_root, text);
            if (n < 0 || (size_t)n >= sizeof(joined)) return -1;
        } else snprintf(joined, sizeof(joined), "%s", text);
    } else {
        if (!base || base[0] != '/') return -1;
        int n = snprintf(joined, sizeof(joined), "%s/%s", base, text);
        if (n < 0 || (size_t)n >= sizeof(joined)) return -1;
    }
    return normalize(joined, out, cap);
}

/* The host path of a POSIX path (the harness root prefix), with a
 * case-insensitive walk of components that do not exist as spelled. */
static int host_path(const char *path, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s%s", root, path);
    if (n < 0 || (size_t)n >= cap) return -1;
    struct stat st;
    if (!stat(out, &st)) return 0;
    if (errno != ENOENT && errno != ENOTDIR) return -1;
    char built[DU_FILE_ICON_PATH * 2];
    size_t used = (size_t)snprintf(built, sizeof(built), "%s", root);
    for (const char *p = path; *p;) {
        while (*p == '/') p++;
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (!len) break;
        if (used + 1 + len >= sizeof(built)) return -1;
        built[used] = '/'; memcpy(built + used + 1, p, len); built[used + 1 + len] = 0;
        if (stat(built, &st)) {
            built[used] = 0;
            DIR *dir = opendir(used ? built : "/");
            if (!dir) return -1;
            struct dirent *e; unsigned scanned = 0; int found = 0;
            while (!found && scanned++ < FI_CI_ENTRIES && (e = readdir(dir)))
                if (strlen(e->d_name) == len && !strncasecmp(e->d_name, p, len)) {
                    built[used] = '/'; memcpy(built + used + 1, e->d_name, len); built[used + 1 + len] = 0; found = 1;
                }
            closedir(dir);
            if (!found) return -1;
        }
        used += 1 + len;
        p += len;
    }
    if (used >= cap) return -1;
    memcpy(out, built, used + 1);
    return 0;
}

static int open_path(const char *path, struct stat *st)
{
    char host[DU_FILE_ICON_PATH * 2];
    if (host_path(path, host, sizeof(host))) return -1;
    int fd = open(host, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (st && fstat(fd, st)) { close(fd); return -1; }
    return fd;
}

/* A small text or link file, whole (at most FI_LINK_BYTES). */
static uint8_t *read_small(const char *path, uint32_t *n)
{
    struct stat st;
    int fd = open_path(path, &st);
    if (fd < 0) return NULL;
    uint8_t *data = NULL;
    if (S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= (off_t)FI_LINK_BYTES && (data = malloc((size_t)st.st_size))) {
        size_t got = 0;
        while (got < (size_t)st.st_size) {
            ssize_t r = read(fd, data + got, (size_t)st.st_size - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (got != (size_t)st.st_size) { free(data); data = NULL; }
        else *n = (uint32_t)got;
    }
    close(fd);
    return data;
}

static const char *base_name(const char *path)
{ const char *b = strrchr(path, '/'); return b ? b + 1 : path; }
static const char *extension(const char *path)
{
    const char *b = base_name(path), *dot = strrchr(b, '.');
    return dot && dot != b ? dot + 1 : "";
}
static int ext_in(const char *ext, const char *const *list)
{
    for (; *list; list++) if (!strcasecmp(ext, *list)) return 1;
    return 0;
}
static const char *const own_types[] = {"exe", "com", "cpl", "scr", "icl", "ico", "cur", "ani", NULL}; /* Windows 98: .dll and .ocx keep the generic library icon */;
static const char *const link_types[] = {"lnk", "ciuki", NULL};

int du_file_icon_shortcut(const char *name)
{
    const char *ext = extension(name ? name : "");
    return ext_in(ext, link_types) || !strcasecmp(ext, "pif") || !strcasecmp(ext, "url");
}
int du_file_icon_candidate(const char *name, int directory)
{
    if (directory) return 1;
    const char *ext = extension(name ? name : "");
    /* Extensionless /Programs entries are native executables. The resolver
     * validates their location and uses the same program:<name> Tango mapping
     * as the Start menu. Other extensionless files are harmless cache misses. */
    return !*ext || ext_in(ext, own_types) || ext_in(ext, link_types) || !strcasecmp(ext, "pif") || !strcasecmp(ext, "url");
}

/* /, or /mnt/<letter>: a medium's root, where AUTORUN.INF lives. */
static int drive_root(const char *path)
{ return !strcmp(path, "/") || (!strncmp(path, "/mnt/", 5) && path[5] && !path[6]); }

/* Windows 98 reads a folder's desktop.ini only when the folder is read-only
 * or system (PathMakeSystemFolder). */
static int custom_folder(const char *path)
{
#ifdef FI_ATTRIBUTES
    struct ciuki_file_info info;
    char host[DU_FILE_ICON_PATH * 2];
    if (host_path(path, host, sizeof(host)) || ciuki_file_info_get(host, &info)) return 0;
    return (info.attributes & (CIUKI_ATTR_READONLY | CIUKI_ATTR_SYSTEM)) != 0;
#else
    char host[DU_FILE_ICON_PATH * 2];
    struct stat st;
    if (host_path(path, host, sizeof(host)) || stat(host, &st)) return 0;
    return !(st.st_mode & 0200);   /* the host build's read-only mapping (apps/files/vfs.c) */
#endif
}

static void set_fallback(struct du_icon_source *s, const char *object)
{ snprintf(s->fallback, sizeof(s->fallback), "%s", object ? object : "type:unknown"); }

static int set_own(struct du_icon_source *s, const char *location, const char *base, int32_t index)
{
    if (du_file_icon_native(location, base, s->file, sizeof(s->file))) { s->file[0] = 0; return 0; }
    s->index = index; s->own = 1; s->resolved = 1;
    return 1;
}

static int resolve(const char *path, int directory, struct du_icon_source *out, unsigned depth);

/* scheme:... (apps/desktop/shell_link.h desk_target_url): a drive letter is a path. */
static int url(const char *s)
{
    if (!s || !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) return 0;
    for (unsigned i = 1; s[i] && i < 32; i++) {
        unsigned c = (unsigned char)s[i];
        if (c == ':') return i > 1 && s[i + 1];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) return 0;
    }
    return 0;
}

/* What a shortcut's target shows: its own icon or its type, by name. */
static void target_icon(const char *target, const char *arguments, const char *link_folder, struct du_icon_source *out, unsigned depth)
{
    const char *t = target;
    if (url(t)) { set_fallback(out, "program:netsurf"); return; }
    char native[DU_FILE_ICON_PATH];
    if (du_file_icon_native(t, link_folder, native, sizeof(native))) { set_fallback(out, "type:executable"); return; }
    const char *name = base_name(native), *dot = strrchr(name, '.');
    if (!strncmp(native, "/Programs/", 10) && !dot) {
        char object[48];
        if (!strcmp(name, "dosbox") && arguments && arguments[0]) snprintf(object, sizeof(object), "menu:game");
        else snprintf(object, sizeof(object), "program:%.30s", name);
        set_fallback(out, object);
        return;
    }
    if (depth + 1 < FI_LINK_DEPTH) {
        struct du_icon_source *t_src = malloc(sizeof(*t_src));
        if (t_src) {
            resolve(native, -1, t_src, depth + 1);
            *out = *t_src;
            free(t_src);
            return;
        }
    }
    set_fallback(out, dot ? du_icon_file(name, 0, ext_in(dot + 1, own_types)) : "type:folder");
}

static int resolve(const char *path, int directory, struct du_icon_source *out, unsigned depth)
{
    memset(out, 0, sizeof(*out));
    char host[DU_FILE_ICON_PATH * 2];
    struct stat st;
    if (directory < 0) directory = !host_path(path, host, sizeof(host)) && !stat(host, &st) && S_ISDIR(st.st_mode);
    const char *name = base_name(path), *ext = extension(path);
    char folder[DU_FILE_ICON_PATH];
    if (directory) {
        set_fallback(out, "type:folder");
        uint8_t *data; uint32_t n = 0; char file[DU_FILE_ICON_PATH], ini[DU_FILE_ICON_PATH + 16]; int32_t index = 0;
        if (drive_root(path)) {
            snprintf(ini, sizeof(ini), "%s%sautorun.inf", path, path[1] ? "/" : "");
            if ((data = read_small(ini, &n))) {
                int found = iconx_autorun_inf(data, n, file, sizeof(file), &index);
                free(data);
                if (found && set_own(out, file, path, index)) return 1;
            }
        }
        if (custom_folder(path)) {
            snprintf(ini, sizeof(ini), "%s%sdesktop.ini", path, path[1] ? "/" : "");
            if ((data = read_small(ini, &n))) {
                int found = iconx_desktop_ini(data, n, file, sizeof(file), &index);
                free(data);
                if (found && set_own(out, file, path, index)) return 1;
            }
        }
        return 0;
    }
    parent_of(path, folder, sizeof(folder));
    if (!*ext && !strncmp(path, "/Programs/", 10) && path[10] && !strchr(path + 10, '/')) {
        char object[48];
        snprintf(object, sizeof(object), "program:%.30s", name);
        set_fallback(out, object);
        out->resolved = !host_path(path, host, sizeof(host)) && !stat(host, &st) && S_ISREG(st.st_mode);
        return 0;
    }
    if (ext_in(ext, own_types)) {
        set_fallback(out, du_icon_file(name, 0, strcasecmp(ext, "ico") && strcasecmp(ext, "cur") && strcasecmp(ext, "ani")));
        snprintf(out->file, sizeof(out->file), "%s", path);
        out->own = 1;
        out->resolved = !host_path(path, host, sizeof(host)) && !stat(host, &st);
        return 1;
    }
    if (ext_in(ext, link_types)) {
        set_fallback(out, "type:.lnk");
        uint32_t n = 0;
        uint8_t *data = read_small(path, &n);
        struct iconx_link *link = data ? malloc(sizeof(*link)) : NULL;
        int parsed = link && !iconx_link_parse(data, n, link);
        free(data);
        if (!parsed) { free(link); return 0; }
        if (link->target[0]) target_icon(link->target, link->arguments, folder, out, depth);
        out->resolved = 1;   /* the shortcut was read: its target's type is the default */
        /* IconLocation wins; the target's icon stays the fallback. */
        if (link->icon[0]) {
            char saved[sizeof(out->fallback)];
            snprintf(saved, sizeof(saved), "%s", out->fallback);
            if (set_own(out, link->icon, folder, link->icon_index)) snprintf(out->fallback, sizeof(out->fallback), "%s", saved);
        }
        free(link);
        return out->own;
    }
    if (!strcasecmp(ext, "pif")) {
        set_fallback(out, "type:executable");          /* the MS-DOS program icon */
        uint32_t n = 0;
        uint8_t *data = read_small(path, &n);
        char icon[DU_FILE_ICON_PATH], program[DU_FILE_ICON_PATH];
        int32_t index = 0;
        int named = data ? iconx_pif_parse(data, n, icon, sizeof(icon), &index, program, sizeof(program)) : -1;
        free(data);
        out->resolved = named >= 0;
        if (named > 0 && set_own(out, icon, folder, index)) return 1;
        char native[DU_FILE_ICON_PATH];
        if (named >= 0 && program[0] && depth + 1 < FI_LINK_DEPTH && !du_file_icon_native(program, folder, native, sizeof(native))) {
            const char *pe = extension(native);
            if (!strcasecmp(pe, "exe")) {            /* a Windows program's own icon; a DOS one has none */
                snprintf(out->file, sizeof(out->file), "%s", native);
                out->own = 1; out->index = 0;
                return 1;
            }
        }
        return 0;
    }
    if (!strcasecmp(ext, "url")) {
        set_fallback(out, "program:netsurf");
        uint32_t n = 0;
        uint8_t *data = read_small(path, &n);
        char icon[DU_FILE_ICON_PATH], value[32];
        int32_t index = 0;
        out->resolved = data != NULL;
        if (data && iconx_ini_get(data, n, "InternetShortcut", "IconFile", icon, sizeof(icon)) && icon[0]) {
            if (iconx_ini_get(data, n, "InternetShortcut", "IconIndex", value, sizeof(value))) {
                char probe[40] = "x,"; snprintf(probe + 2, sizeof(probe) - 2, "%s", value);
                iconx_location_split(probe, &index);
            }
            free(data);
            return set_own(out, icon, folder, index);
        }
        free(data);
        return 0;
    }
    set_fallback(out, du_icon_file(name, 0, 0));
    return 0;
}

int du_file_icon_resolve(const char *path, int directory, struct du_icon_source *out)
{
    if (!out) return -1;
    if (!path || path[0] != '/') { memset(out, 0, sizeof(*out)); set_fallback(out, "type:unknown"); return -1; }
    return resolve(path, directory, out, 0);
}

/* ---- extraction ---------------------------------------------------------------- */
static int fd_read(void *ctx, uint32_t offset, void *buffer, uint32_t bytes)
{
    int fd = *(const int *)ctx;
    uint8_t *p = buffer;
    while (bytes) {
        ssize_t r = pread(fd, p, bytes, (off_t)offset);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; offset += (uint32_t)r; bytes -= (uint32_t)r;
    }
    return 0;
}

int du_file_icon_extract(const char *file, int32_t index, const unsigned *sizes, uint32_t *const *argb, unsigned count)
{
    struct stat st;
    if (!file || !sizes || !argb || !count) return -1;
    int fd = open_path(file, &st);
    if (fd < 0) return -1;
    struct iconx *x = S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 0x7fffffff ? malloc(sizeof(*x)) : NULL;
    int done = 0;
    if (x) {
        iconx_init(x, fd_read, &fd, (uint32_t)st.st_size);
        for (unsigned k = 0; k < count; k++)
            if (!argb[k] || iconx_extract(x, index, sizes[k], argb[k])) break;
            else done++;
    }
    free(x);
    close(fd);
    return done == (int)count ? 0 : -1;
}

int du_file_icon_load(const char *path, int directory, unsigned size, uint32_t *argb, struct du_icon_source *source)
{
    struct du_icon_source local, *s = source ? source : &local;
    if (du_file_icon_resolve(path, directory, s) <= 0) return 0;
    uint32_t *outs[1] = {argb};
    unsigned sizes[1] = {size};
    if (du_file_icon_extract(s->file, s->index, sizes, outs, 1)) { s->own = 0; return 0; }
    return 1;
}

/* ---- the cache --------------------------------------------------------------- */
static const unsigned slot_sizes[DU_FILE_ICON_SLOTS] = {16, 22, 32, 48};
enum { FI_QUEUED, FI_WORKING, FI_DONE };
struct fi_entry {
    char *key; uint32_t hash; int64_t mtime;
    int state, own, dead, resolved;
    unsigned want, have;
    uint32_t *px[DU_FILE_ICON_SLOTS], *overlay[DU_FILE_ICON_SLOTS];
    char fallback[sizeof(((struct du_icon_source *)0)->fallback)];
    uint64_t used;
    struct fi_entry *next;              /* queue */
};
static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    int running, stopping;
    void (*wake)(void *); void *wake_ctx;
    struct fi_entry *entries[FI_CAP]; unsigned count;
    struct fi_entry *head, *tail;
    unsigned generation;
    size_t bytes;
    struct du_file_icon_stats stats;
} C = {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};

static uint32_t hash_key(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    while (*s) h = (h ^ (uint8_t)*s++) * 0x01000193u;
    return h;
}
static int slot_of(unsigned size)
{
    for (int k = 0; k < DU_FILE_ICON_SLOTS; k++) if (slot_sizes[k] == size) return k;
    return -1;
}
static void entry_free(struct fi_entry *e)
{
    for (int k = 0; k < DU_FILE_ICON_SLOTS; k++) {
        if (e->px[k]) C.bytes -= (size_t)slot_sizes[k] * slot_sizes[k] * 4;
        if (e->overlay[k]) C.bytes -= (size_t)slot_sizes[k] * slot_sizes[k] * 4;
        free(e->px[k]); free(e->overlay[k]);
    }
    free(e->key); free(e);
}
/* Drop the least recently used finished entry idle for the grace time. */
static int evict(uint64_t now)
{
    int victim = -1;
    for (unsigned k = 0; k < C.count; k++) {
        struct fi_entry *e = C.entries[k];
        if (e->state != FI_DONE || now - e->used < FI_GRACE_NS) continue;
        if (victim < 0 || e->used < C.entries[victim]->used) victim = (int)k;
    }
    if (victim < 0) return 0;
    entry_free(C.entries[victim]);
    C.entries[victim] = C.entries[--C.count];
    C.stats.evicted++;
    return 1;
}
static void enqueue(struct fi_entry *e)
{
    e->state = FI_QUEUED; e->next = NULL;
    if (C.tail) C.tail->next = e; else C.head = e;
    C.tail = e;
    C.stats.queued++;
    pthread_cond_signal(&C.changed);
}

/* Resolve and extract an entry's wanted sizes; called without the lock. */
static void work(const char *key, unsigned want, int *own, int *resolved, char *fallback, size_t cap, uint32_t **px)
{
    struct du_icon_source *s = malloc(sizeof(*s));
    *own = 0; *resolved = 0;
    snprintf(fallback, cap, "type:unknown");
    if (!s) return;
    const char *bar = strchr(key, '|');
    if (bar) {                         /* an explicit location: file|index */
        size_t n = (size_t)(bar - key);
        memset(s, 0, sizeof(*s));
        if (n < sizeof(s->file)) { memcpy(s->file, key, n); s->file[n] = 0; s->index = (int32_t)strtol(bar + 1, NULL, 10); s->own = 1; }
        snprintf(s->fallback, sizeof(s->fallback), "%s", du_icon_file(base_name(s->file), 0, 1));
        s->resolved = 0;   /* an explicit resource without icons: the caller's own default */
    } else resolve(key, -1, s, 0);
    snprintf(fallback, cap, "%s", s->fallback);
    *resolved = s->resolved;
    if (s->own) {
        unsigned sizes[DU_FILE_ICON_SLOTS]; uint32_t *outs[DU_FILE_ICON_SLOTS]; unsigned n = 0;
        for (int k = 0; k < DU_FILE_ICON_SLOTS; k++) if (want & (1u << k)) {
            sizes[n] = slot_sizes[k]; outs[n] = px[k] = malloc((size_t)slot_sizes[k] * slot_sizes[k] * 4);
            if (!px[k]) { n = 0; break; }
            n++;
        }
        *own = n && !du_file_icon_extract(s->file, s->index, sizes, outs, n);
        if (!*own) for (int k = 0; k < DU_FILE_ICON_SLOTS; k++) { free(px[k]); px[k] = NULL; }
    }
    free(s);
}

/* Publish a finished extraction into its entry; called with the lock. */
/* notify: the worker published (the generation tells the UI to repaint);
 * an extraction in line is seen by the very lookup that asked for it. */
static void publish(struct fi_entry *e, unsigned want, int own, int resolved, const char *fallback, uint32_t **px, int notify)
{
    e->resolved = e->resolved || resolved;
    for (int k = 0; k < DU_FILE_ICON_SLOTS; k++) if (px[k]) {
        if (!e->px[k]) { e->px[k] = px[k]; C.bytes += (size_t)slot_sizes[k] * slot_sizes[k] * 4; }
        else free(px[k]);
    }
    e->own = e->own || own;
    if (!e->have) snprintf(e->fallback, sizeof(e->fallback), "%s", fallback);
    e->have |= want; e->state = FI_DONE;
    if (notify) C.generation++;
    C.stats.extracted++;
    if (!own) C.stats.defaults++;
}
/* No worker (tools, host harnesses, scripted gate runs): extract at once.
 * Called with the lock, which is released around the file I/O. */
static void work_here(struct fi_entry *e, unsigned want)
{
    uint32_t *px[DU_FILE_ICON_SLOTS] = {0};
    int own = 0, resolved = 0;
    char fallback[sizeof(e->fallback)], *key = strdup(e->key);
    e->state = FI_WORKING;
    pthread_mutex_unlock(&C.lock);
    if (key) work(key, want, &own, &resolved, fallback, sizeof(fallback), px);
    else snprintf(fallback, sizeof(fallback), "type:unknown");
    free(key);
    pthread_mutex_lock(&C.lock);
    publish(e, want, own, resolved, fallback, px, 0);
}
static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&C.lock);
    for (;;) {
        while (!C.stopping && !C.head) pthread_cond_wait(&C.changed, &C.lock);
        if (C.stopping) break;
        struct fi_entry *e = C.head;
        C.head = e->next; if (!C.head) C.tail = NULL;
        e->state = FI_WORKING;
        unsigned want = e->want & ~e->have;
        char *key = strdup(e->key);
        pthread_mutex_unlock(&C.lock);
        uint32_t *px[DU_FILE_ICON_SLOTS] = {0};
        int own = 0, resolved = 0;
        char fallback[sizeof(e->fallback)];
        if (key) work(key, want, &own, &resolved, fallback, sizeof(fallback), px);
        free(key);
        pthread_mutex_lock(&C.lock);
        publish(e, want, own, resolved, fallback, px, 1);
        int idle = !C.head;
        void (*wake)(void *) = C.wake; void *ctx = C.wake_ctx;
        pthread_mutex_unlock(&C.lock);
        if (idle && wake) wake(ctx);
        pthread_mutex_lock(&C.lock);
    }
    pthread_mutex_unlock(&C.lock);
    return NULL;
}

int du_file_icons_start(void (*wake)(void *), void *ctx)
{
    pthread_mutex_lock(&C.lock);
    C.wake = wake; C.wake_ctx = ctx;
    int e = 0;
    if (!C.running) {
        C.stopping = 0;
        e = pthread_create(&C.thread, NULL, worker, NULL);
        if (!e) C.running = 1;
    }
    pthread_mutex_unlock(&C.lock);
    return -e;
}

void du_file_icons_stop(void)
{
    pthread_mutex_lock(&C.lock);
    int running = C.running;
    C.stopping = 1; C.wake = NULL;
    pthread_cond_broadcast(&C.changed);
    pthread_mutex_unlock(&C.lock);
    if (running) pthread_join(C.thread, NULL);
    pthread_mutex_lock(&C.lock);
    C.running = 0; C.stopping = 0;
    for (unsigned k = 0; k < C.count; k++) entry_free(C.entries[k]);
    C.count = 0; C.head = C.tail = NULL;
    pthread_mutex_unlock(&C.lock);
}

unsigned du_file_icons_generation(void)
{
    pthread_mutex_lock(&C.lock);
    unsigned g = C.generation;
    pthread_mutex_unlock(&C.lock);
    return g;
}

void du_file_icons_snapshot(struct du_file_icon_stats *out)
{
    pthread_mutex_lock(&C.lock);
    if (out) { *out = C.stats; out->entries = C.count; out->bytes = (uint32_t)C.bytes; }
    pthread_mutex_unlock(&C.lock);
}

/* The Windows shortcut overlay: the link emblem in the lower-left corner,
 * half the icon's size (area-averaged from the 16 or 32 px emblem). */
static void overlay(const uint32_t *src, uint32_t *dst, unsigned size)
{
    memcpy(dst, src, (size_t)size * size * 4);
    unsigned badge = size / 2 < 8 ? 8 : size / 2, from = badge > 16 ? 32 : 16;
    const uint32_t *arrow = du_icon_argb(du_icon_name("emblem:symlink"), from);
    if (!arrow) return;
    for (unsigned y = 0; y < badge; y++) for (unsigned x = 0; x < badge; x++) {
        uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
        for (unsigned yy = y * from / badge; yy < (y + 1) * from / badge; yy++)
            for (unsigned xx = x * from / badge; xx < (x + 1) * from / badge; xx++) {
                uint32_t p = arrow[yy * from + xx], pa = p >> 24;
                a += pa; r += (p >> 16 & 255) * pa; g += (p >> 8 & 255) * pa; b += (p & 255) * pa; n++;
            }
        if (!n || !a) continue;
        uint32_t sa = (a + n / 2) / n, sr = (r + a / 2) / a, sg = (g + a / 2) / a, sb = (b + a / 2) / a;
        uint32_t *d = &dst[(size - badge + y) * size + x], da = *d >> 24;
        uint32_t oa = sa + da * (255 - sa) / 255;
        if (!oa) continue;
        uint32_t out = 0, src_c[3] = {sr, sg, sb};
        for (unsigned c = 0; c < 3; c++) {
            uint32_t t = (*d >> (16 - 8 * c)) & 255;
            out |= ((src_c[c] * sa + t * da * (255 - sa) / 255) / oa) << (16 - 8 * c);
        }
        *d = oa << 24 | out;
    }
}

const uint32_t *du_file_icon_flags(const char *key, int64_t mtime, unsigned size, unsigned flags)
{
    int slot = slot_of(size);
    if (!key || key[0] != '/' || slot < 0 || strlen(key) >= DU_FILE_ICON_KEY) return NULL;
    uint32_t h = hash_key(key);
    uint64_t now = now_ns();
    const uint32_t *result = NULL;
    pthread_mutex_lock(&C.lock);
    C.stats.lookups++;
    struct fi_entry *e = NULL;
    for (unsigned k = 0; k < C.count; k++) {
        struct fi_entry *c = C.entries[k];
        if (c->hash == h && !c->dead && !strcmp(c->key, key)) { e = c; break; }
    }
    if (e && mtime && e->mtime != mtime && e->state == FI_DONE) { e->dead = 1; e->used = now; e = NULL; C.stats.stale++; }
    if (!e) {
        if (C.count == FI_CAP || C.bytes > FI_BYTES) evict(now);
        if (C.count == FI_CAP || !(e = calloc(1, sizeof(*e))) || !(e->key = strdup(key))) {
            if (e) free(e);
            C.stats.refused++;
            pthread_mutex_unlock(&C.lock);
            return NULL;
        }
        e->hash = h; e->mtime = mtime; e->used = now; e->state = FI_DONE;
        e->want = 1u << slot | (1u << 0) | (1u << 2);   /* the asked size, 16 and 32 together */
        C.entries[C.count++] = e;
        if (C.running) enqueue(e);
        else work_here(e, e->want);
    }
    e->used = now;
    if (e->state == FI_DONE && e->own && !(e->have & (1u << slot))) {
        e->want |= 1u << slot;
        if (C.running) enqueue(e);
        else work_here(e, 1u << slot);
    }
    if (e->state != FI_QUEUED || e->have) {
        if (e->own && e->px[slot]) result = e->px[slot];
        else if (!e->own && e->have && e->resolved && (flags & DU_FILE_ICON_DEFAULT))
            result = du_icon_argb(du_icon_name(e->fallback), size);
        if (result && (flags & DU_FILE_ICON_LINK)) {
            if (!e->overlay[slot] && (e->overlay[slot] = malloc((size_t)size * size * 4))) {
                overlay(result, e->overlay[slot], size);
                C.bytes += (size_t)size * size * 4;
            }
            result = e->overlay[slot];
        }
    }
    if (result) C.stats.hits++;
    pthread_mutex_unlock(&C.lock);
    return result;
}

const uint32_t *du_file_icon(const char *key, int64_t mtime, unsigned size)
{ return du_file_icon_flags(key, mtime, size, DU_FILE_ICON_DEFAULT); }

int du_file_icon_fallback(const char *key, char *out, size_t cap)
{
    if (!key || !out || !cap) return -1;
    uint32_t h = hash_key(key);
    int found = -1;
    pthread_mutex_lock(&C.lock);
    for (unsigned k = 0; k < C.count; k++) {
        struct fi_entry *c = C.entries[k];
        if (c->hash == h && !c->dead && !strcmp(c->key, key) && c->have) {
            snprintf(out, cap, "%s", c->fallback); found = c->own; break;
        }
    }
    pthread_mutex_unlock(&C.lock);
    return found;
}
