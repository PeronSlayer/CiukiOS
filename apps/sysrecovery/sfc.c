/* SPDX-License-Identifier: MIT
 * G14 System File Checker (docs/design/startup-recovery.md 2.6; Windows 98
 * Resource Kit "Using System File Checker", Tables 27.3-27.5): the image
 * builder's SFC.LST baseline (sha256 size path, the build-manifest.json
 * records) with SFC.DAT overrides; changed and deleted files; update the
 * verification information; restore an original after backing the current
 * file up to Helpdesk\SFC; a log. */
#define _POSIX_C_SOURCE 200809L
/* System headers first: abi.h then leaves the C library's macros alone. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "recovery.h"
#include <ciuki/sha256.h>

#define LINE 256u
struct entry { char path[160], sha[65]; uint64_t size; };
struct table { struct entry *e; unsigned n, cap; };

static int full(char *out, size_t cap, const char *root, const char *path)
{
    int n = snprintf(out, cap, "%s%s", root ? root : "", path);
    return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}
static int find(const struct table *t, const char *path)
{
    for (unsigned i = 0; i < t->n; i++) if (!strcasecmp(t->e[i].path, path)) return (int)i;
    return -1;
}
/* "sha256 size path" lines; ';' comments; a later line replaces an earlier one. */
static int load(const char *root, const char *file, struct table *t, bool required)
{
    char p[512], line[LINE];
    int e = full(p, sizeof(p), root, file); if (e) return e;
    FILE *f = fopen(p, "r");
    if (!f) return required || errno != ENOENT ? -errno : 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == ';' || line[0] == '\n') continue;
        struct entry en; unsigned long long size; memset(&en, 0, sizeof(en));
        if (sscanf(line, "%64s %llu %159[^\n]", en.sha, &size, en.path) != 3 || strlen(en.sha) != 64 || en.path[0] != '/') continue;
        en.size = size;
        int at = find(t, en.path);
        if (at >= 0) { t->e[at] = en; continue; }
        if (t->n == t->cap) {
            unsigned cap = t->cap ? t->cap * 2 : 256;
            struct entry *grown = realloc(t->e, cap * sizeof(*grown));
            if (!grown) { fclose(f); return -ENOMEM; }
            t->e = grown; t->cap = cap;
        }
        t->e[t->n++] = en;
    }
    e = ferror(f) ? -EIO : 0;
    fclose(f);
    return e;
}
static int hash_file(const char *path, char hex[65], uint64_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -errno;
    struct sha256_ctx c; sha256_init(&c);
    uint8_t *buf = malloc(32768), digest[32];
    if (!buf) { fclose(f); return -ENOMEM; }
    size_t n; *size = 0;
    while ((n = fread(buf, 1, 32768, f)) > 0) { sha256_update(&c, buf, n); *size += n; }
    int e = ferror(f) ? -EIO : 0;
    fclose(f); free(buf);
    if (e) return e;
    sha256_final(&c, digest); sha256_hex(digest, hex);
    return 0;
}

int sfc_scan(const char *root, struct sfc_result *out)
{
    memset(out, 0, sizeof(*out));
    struct table t = { 0 };
    int e = load(root, SFC_LIST, &t, true);
    if (!e) e = load(root, SFC_OVERRIDES, &t, false);
    for (unsigned i = 0; !e && i < t.n; i++) {
        char p[512], hex[65]; uint64_t size = 0;
        if ((e = full(p, sizeof(p), root, t.e[i].path))) break;
        out->checked++;
        int h = hash_file(p, hex, &size);
        enum sfc_state state = h == -ENOENT ? SFC_DELETED : h ? SFC_CHANGED :
            size != t.e[i].size || strcasecmp(hex, t.e[i].sha) ? SFC_CHANGED : SFC_OK;
        if (state == SFC_OK) continue;
        if (!(out->count % 64)) {
            struct sfc_problem *grown = realloc(out->problems, (out->count + 64) * sizeof(*grown));
            if (!grown) { e = -ENOMEM; break; }
            out->problems = grown;
        }
        struct sfc_problem *pr = &out->problems[out->count++];
        memset(pr, 0, sizeof(*pr));
        snprintf(pr->path, sizeof(pr->path), "%s", t.e[i].path);
        pr->state = state; pr->expected_size = t.e[i].size; pr->size = size;
        memcpy(pr->expected, t.e[i].sha, 65);
        if (state == SFC_DELETED) out->deleted++; else out->changed++;
    }
    free(t.e);
    if (e) sfc_free(out);
    return e;
}
void sfc_free(struct sfc_result *r) { free(r->problems); r->problems = NULL; r->count = 0; }

int sfc_update(const char *root, const char *path)
{
    char p[512], hex[65], line[LINE];
    uint64_t size;
    int e = full(p, sizeof(p), root, path); if (e) return e;
    if ((e = hash_file(p, hex, &size))) return e;
    if ((e = full(p, sizeof(p), root, SFC_OVERRIDES))) return e;
    FILE *f = fopen(p, "a");
    if (!f) return -errno;
    snprintf(line, sizeof(line), "%s %llu %s\n", hex, (unsigned long long)size, path);
    e = fputs(line, f) < 0 ? -EIO : 0;
    if (fflush(f) && !e) e = -errno;
    if (!e && fsync(fileno(f))) e = -errno;
    if (fclose(f) && !e) e = -errno;
    return e;
}
static int copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (!in) return -errno;
    int fd = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { int e = -errno; fclose(in); return e; }
    uint8_t *buf = malloc(32768);
    int e = buf ? 0 : -ENOMEM;
    size_t n;
    while (!e && (n = fread(buf, 1, 32768, in)) > 0)
        for (size_t done = 0; done < n && !e;) {
            ssize_t w = write(fd, buf + done, n - done);
            if (w <= 0) e = w < 0 ? -errno : -EIO; else done += (size_t)w;
        }
    if (!e && ferror(in)) e = -EIO;
    if (!e && fsync(fd)) e = -errno;
    if (close(fd) && !e) e = -errno;
    fclose(in); free(buf);
    return e;
}
static int mkdirs(const char *root, const char *path)
{
    char p[512];
    int e = full(p, sizeof(p), root, path); if (e) return e;
    for (size_t i = strlen(root ? root : "") + 1; ; i++)
        if (p[i] == '/' || !p[i]) {
            char c = p[i]; p[i] = 0;
            if (mkdir(p, 0755) && errno != EEXIST) return -errno;
            p[i] = c;
            if (!c) return 0;
        }
}
int sfc_restore(const char *root, const char *path, const char *source_root)
{
#if defined(__CIUKIOS__) && !defined(__linux__)
    /* sec-sysvc: the baseline and boot files are sealed; the SYSTEM service
     * verifies the original against SFC.LST and replaces the file. */
    if (!root || !*root) {
        if (!ciuki_sfc_restore(path, source_root && *source_root ? source_root : "/")) return 0;
        if (errno != ENOSYS && errno != ENODEV) return -errno;
    }
#endif
    struct table t = { 0 };
    int e = load(root, SFC_LIST, &t, true);
    int at = e ? -1 : find(&t, path);
    if (!e && at < 0) e = -ENOENT;
    char source[512], target[512], hex[65];
    uint64_t size;
    if (!e) e = full(source, sizeof(source), source_root, path);
    if (!e) e = full(target, sizeof(target), root, path);
    /* Only the original: the source must match the build baseline. */
    if (!e) e = hash_file(source, hex, &size);
    if (!e && (size != t.e[at].size || strcasecmp(hex, t.e[at].sha))) e = -EILSEQ;
    struct stat st;
    if (!e && !stat(target, &st)) {
        const char *base = strrchr(path, '/') + 1;
        char backup[512], name[200];
        snprintf(name, sizeof(name), SFC_HELPDESK "/%s", base);
        if (!(e = mkdirs(root, SFC_HELPDESK)) && !(e = full(backup, sizeof(backup), root, name))) {
            unlink(backup);
            e = copy_file(target, backup);
        }
    }
    if (!e) {
        char temp[530], old[530];
        snprintf(temp, sizeof(temp), "%s.sfn", target); snprintf(old, sizeof(old), "%s.sfo", target);
        e = copy_file(source, temp);
        unlink(old);
        if (!e && rename(target, old) && errno != ENOENT) e = -errno;
        if (!e && rename(temp, target)) { e = -errno; rename(old, target); }
        if (!e) unlink(old); else unlink(temp);
    }
    free(t.e);
    return e;
}
int sfc_log(const char *root, const struct sfc_result *r, int64_t now)
{
    time_t t = (time_t)now; struct tm tm; char when[32];
    if (localtime_r(&t, &tm)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm); else snprintf(when, sizeof(when), "?");
    size_t cap = 128 + (size_t)r->count * (sizeof(r->problems[0].path) + 16), used = 0;
    char *text = malloc(cap);
    if (!text) return -ENOMEM;
    int n = snprintf(text, cap, "%s System File Checker: %u checked, %u changed, %u deleted\n", when, r->checked, r->changed, r->deleted);
    if (n > 0) used = (size_t)n < cap ? (size_t)n : cap - 1;
    for (unsigned i = 0; i < r->count && used < cap - 1; i++) {
        n = snprintf(text + used, cap - used, "  %s %s\n", r->problems[i].state == SFC_DELETED ? "deleted" : "changed", r->problems[i].path);
        if (n > 0) used += (size_t)n < cap - used ? (size_t)n : cap - used - 1;
    }
    /* sec-sysvc: a system log, appended through the SYSTEM log service. */
    int e = sr_log_append(root, "SFC.LOG", text, used);
    free(text);
    return e;
}
