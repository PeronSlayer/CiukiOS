/* SPDX-License-Identifier: MIT
 * fx-exeicons host driver: the iconx core and the libdeskui file icon
 * service on synthetic fixtures (tests/host/desktop/test_file_icons.py).
 * Every command prints one line of key=value results. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "deskui.h"
#include "iconx.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __linux__ /* the SDK abi.h defines _SC_* values the host's unistd enumerates */
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#endif
#include <unistd.h>

static uint8_t *slurp(const char *path, uint32_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(size > 0 ? (size_t)size : 1);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) { perror(path); exit(2); }
    fclose(f);
    *n = (uint32_t)size;
    return data;
}
static uint32_t fnv(const uint32_t *px, unsigned count)
{
    uint32_t h = 0x811c9dc5u;
    for (unsigned i = 0; i < count; i++) for (int k = 0; k < 4; k++) h = (h ^ (uint8_t)(px[i] >> (8 * k))) * 0x01000193u;
    return h;
}
static void dump(const uint32_t *px, unsigned size)
{
    printf(" fnv=%08x", fnv(px, size * size));
    printf(" pixels=");
    for (unsigned i = 0; i < size * size; i++) printf("%s%08x", i ? "," : "", px[i]);
}

static int extract_cmd(const char *file, int index, unsigned size, int full)
{
    uint32_t n;
    uint8_t *data = slurp(file, &n);
    struct iconx_memory m = {data, n};
    struct iconx *x = malloc(sizeof(*x));
    uint32_t *px = calloc((size_t)size * size, 4);
    iconx_init(x, iconx_memory_read, &m, n);
    int kind = iconx_kind(x), count = iconx_count(x);
    int e = iconx_extract(x, index, size, px);
    printf("status=%d kind=%d count=%d group=%u width=%u height=%u depth=%u entry=%u id=%u loaded=%u calls=%u",
           e, kind, count, x->group_id, x->chosen.width, x->chosen.height, x->chosen.depth, x->chosen.index, x->chosen.id,
           x->loaded, x->calls);
    if (!e) { if (full) dump(px, size); else printf(" fnv=%08x", fnv(px, size * size)); }
    printf("\n");
    free(px); free(x); free(data);
    return 0;
}

/* Deterministic mutations of a valid file: none may crash, hang or read
 * past the budget; each prints nothing unless something is wrong. */
static int fuzz_cmd(const char *file, unsigned rounds)
{
    uint32_t n;
    uint8_t *data = slurp(file, &n), *copy = malloc(n);
    struct iconx *x = malloc(sizeof(*x));
    uint32_t *px = malloc(48 * 48 * 4), seed = 0x12345678u, ok = 0, failed = 0, max_loaded = 0;
    for (unsigned r = 0; r < rounds; r++) {
        memcpy(copy, data, n);
        unsigned flips = 1 + r % 8;
        for (unsigned k = 0; k < flips; k++) {
            seed = seed * 1664525u + 1013904223u;
            uint32_t at = (seed >> 8) % n;
            seed = seed * 1664525u + 1013904223u;
            unsigned mode = seed >> 29;
            copy[at] = mode == 0 ? 0xff : mode == 1 ? 0 : mode == 2 ? (uint8_t)(copy[at] ^ 0x80) : (uint8_t)(seed >> 16);
        }
        uint32_t len = r % 5 == 4 ? (seed >> 4) % n : n;   /* truncations too */
        struct iconx_memory m = {copy, len};
        iconx_init(x, iconx_memory_read, &m, len);
        static const unsigned sizes[] = {16, 32, 48};
        for (unsigned s = 0; s < 3; s++) {
            int e = iconx_extract(x, (int)(r % 3) - 1, sizes[s], px);
            if (e) failed++; else ok++;
        }
        (void)iconx_count(x);
        if (x->loaded > max_loaded) max_loaded = x->loaded;
        if (x->loaded > ICONX_READ_BUDGET) { printf("budget exceeded round=%u loaded=%u\n", r, x->loaded); return 1; }
    }
    printf("fuzz rounds=%u ok=%u failed=%u max_loaded=%u\n", rounds, ok, failed, max_loaded);
    free(px); free(x); free(copy); free(data);
    return 0;
}

static int link_cmd(const char *file)
{
    uint32_t n;
    uint8_t *data = slurp(file, &n);
    struct iconx_link l;
    int e = iconx_link_parse(data, n, &l);
    printf("status=%d ciuki=%d target=%s icon=%s index=%d arguments=%s\n", e, l.ciuki, l.target, l.icon, l.icon_index, l.arguments);
    free(data);
    return 0;
}
static int pif_cmd(const char *file)
{
    uint32_t n;
    uint8_t *data = slurp(file, &n);
    char icon[260], program[260]; int32_t index;
    int e = iconx_pif_parse(data, n, icon, sizeof(icon), &index, program, sizeof(program));
    printf("status=%d icon=%s index=%d program=%s\n", e, icon, index, program);
    free(data);
    return 0;
}
static int ini_cmd(const char *kind, const char *file, const char *section, const char *key)
{
    uint32_t n;
    uint8_t *data = slurp(file, &n);
    char out[260]; int32_t index = 0; int e;
    if (!strcmp(kind, "ini")) e = iconx_ini_get(data, n, section, key, out, sizeof(out));
    else if (!strcmp(kind, "desktopini")) e = iconx_desktop_ini(data, n, out, sizeof(out), &index);
    else e = iconx_autorun_inf(data, n, out, sizeof(out), &index);
    printf("status=%d value=%s index=%d\n", e, e ? out : "", index);
    free(data);
    return 0;
}
static int split_cmd(const char *text)
{
    char s[300]; int32_t index;
    snprintf(s, sizeof(s), "%s", text);
    iconx_location_split(s, &index);
    printf("file=%s index=%d\n", s, index);
    return 0;
}
static int native_cmd(const char *in, const char *base)
{
    char out[260];
    int e = du_file_icon_native(in, base[0] ? base : NULL, out, sizeof(out));
    printf("status=%d path=%s\n", e, e ? "" : out);
    return 0;
}
static int resolve_cmd(const char *path, int directory, unsigned size)
{
    struct du_icon_source s;
    int own = du_file_icon_resolve(path, directory, &s);
    printf("own=%d file=%s index=%d fallback=%s", own, own > 0 ? s.file : "", s.index, s.fallback);
    if (size) {
        uint32_t *px = calloc((size_t)size * size, 4);
        int loaded = du_file_icon_load(path, directory, size, px, &s);
        printf(" loaded=%d", loaded);
        if (loaded) printf(" fnv=%08x", fnv(px, size * size));
        free(px);
    }
    printf("\n");
    return 0;
}

/* The cache: synchronous mode, worker mode, mtime invalidation, overlay. */
static pthread_mutex_t woke_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t woke_cond = PTHREAD_COND_INITIALIZER;
static void wake(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&woke_lock); pthread_cond_broadcast(&woke_cond); pthread_mutex_unlock(&woke_lock);
}
static void wait_generation(unsigned seen)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 10;
    pthread_mutex_lock(&woke_lock);
    while (du_file_icons_generation() == seen)
        if (pthread_cond_timedwait(&woke_cond, &woke_lock, &deadline) == ETIMEDOUT) { printf("timeout\n"); exit(1); }
    pthread_mutex_unlock(&woke_lock);
}
static int cache_cmd(int argc, char **argv)
{
    /* cache MODE KEY [KEY...]: MODE sync|worker; prints each key's 16/32 fnv. */
    int worker = !strcmp(argv[0], "worker");
    if (worker && du_file_icons_start(wake, NULL)) { printf("start failed\n"); return 1; }
    for (int k = 1; k < argc; k++) {
        const char *key = argv[k];
        unsigned flags = DU_FILE_ICON_DEFAULT;
        if (!strncmp(key, "link:", 5)) { flags |= DU_FILE_ICON_LINK; key += 5; }
        const uint32_t *p16 = du_file_icon_flags(key, 0, 16, flags);
        if (worker && !p16) {
            unsigned seen = du_file_icons_generation();
            /* Published between the lookup and here: the next lookup has it. */
            if (!(p16 = du_file_icon_flags(key, 0, 16, flags))) { wait_generation(seen); p16 = du_file_icon_flags(key, 0, 16, flags); }
            char probe[64];
            /* Published without an icon (not read, nothing to show): the caller's default. */
            if (!p16 && du_file_icon_fallback(key, probe, sizeof(probe)) < 0) { printf("pending-after-publication key=%s\n", key); return 1; }
        }
        const uint32_t *p32 = du_file_icon_flags(key, 0, 32, flags);
        /* 48 px was not asked first: extracted in line, or queued for the worker. */
        const uint32_t *p48 = du_file_icon_flags(key, 0, 48, flags);
        char fallback[64] = "";
        int own = du_file_icon_fallback(key, fallback, sizeof(fallback));
        printf("own=%d fallback=%s fnv16=%08x fnv32=%08x fnv48=%08x shortcut=%d key=%s\n", own, fallback,
               p16 ? fnv(p16, 256) : 0, p32 ? fnv(p32, 1024) : 0, p48 ? fnv(p48, 2304) : 0, du_file_icon_shortcut(key), key);
    }
    struct du_file_icon_stats s;
    du_file_icons_snapshot(&s);
    printf("stats lookups=%u hits=%u queued=%u extracted=%u defaults=%u stale=%u evicted=%u refused=%u entries=%u bytes=%u\n",
           s.lookups, s.hits, s.queued, s.extracted, s.defaults, s.stale, s.evicted, s.refused, s.entries, s.bytes);
    du_file_icons_stop();
    return 0;
}
/* mtime: the same key looked up with a new modification time extracts again. */
static int mtime_cmd(const char *key, const char *replacement, const char *host)
{
    const uint32_t *a = du_file_icon_flags(key, 100, 32, 0);
    uint32_t first = a ? fnv(a, 1024) : 0;
    const uint32_t *again = du_file_icon_flags(key, 100, 32, 0);
    /* Replace the file's bytes, then look it up with a newer time. */
    uint32_t n;
    uint8_t *data = slurp(replacement, &n);
    FILE *f = fopen(host, "wb");
    if (!f || fwrite(data, 1, n, f) != n || fclose(f)) { perror(host); return 1; }
    free(data);
    const uint32_t *same_time = du_file_icon_flags(key, 100, 32, 0);
    const uint32_t *b = du_file_icon_flags(key, 200, 32, 0);
    struct du_file_icon_stats s;
    du_file_icons_snapshot(&s);
    printf("first=%08x again_same=%d stale_time=%08x second=%08x stale=%u entries=%u\n", first, again == a,
           same_time ? fnv(same_time, 1024) : 0, b ? fnv(b, 1024) : 0, s.stale, s.entries);
    du_file_icons_stop();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    const char *root = getenv("FILE_ICONS_ROOT");
    if (root) du_file_icons_root(root);
    const char *store = getenv("ICONS_STORE");
    if (store) du_icon_paths(store, getenv("ICONS_PORTRAIT"));
    const char *c = argv[1];
    if (!strcmp(c, "extract") && argc >= 5) return extract_cmd(argv[2], atoi(argv[3]), (unsigned)atoi(argv[4]), argc > 5);
    if (!strcmp(c, "fuzz") && argc >= 4) return fuzz_cmd(argv[2], (unsigned)atoi(argv[3]));
    if (!strcmp(c, "link") && argc >= 3) return link_cmd(argv[2]);
    if (!strcmp(c, "pif") && argc >= 3) return pif_cmd(argv[2]);
    if ((!strcmp(c, "ini") && argc >= 5) || ((!strcmp(c, "desktopini") || !strcmp(c, "autorun")) && argc >= 3))
        return ini_cmd(c, argv[2], argc > 3 ? argv[3] : "", argc > 4 ? argv[4] : "");
    if (!strcmp(c, "split") && argc >= 3) return split_cmd(argv[2]);
    if (!strcmp(c, "native") && argc >= 3) return native_cmd(argv[2], argc > 3 ? argv[3] : "");
    if (!strcmp(c, "resolve") && argc >= 3) return resolve_cmd(argv[2], argc > 3 ? atoi(argv[3]) : -1, argc > 4 ? (unsigned)atoi(argv[4]) : 0);
    if (!strcmp(c, "cache") && argc >= 4) return cache_cmd(argc - 2, argv + 2);
    if (!strcmp(c, "mtime") && argc >= 5) return mtime_cmd(argv[2], argv[3], argv[4]);
    return 2;
}
