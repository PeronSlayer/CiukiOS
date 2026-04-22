/*
 * OPENGEM-004 — App Catalog module.
 *
 * Two-lane discovery (FAT + handoff COM catalog) with case-insensitive
 * dedupe on the 8.3 name. Static backing storage. See
 * stage2/include/app_catalog.h for the contract.
 */

#include "app_catalog.h"
#include "fat.h"
#include "video.h"
#include "serial.h"

static app_catalog_entry_t g_entries[APP_CATALOG_MAX_ENTRIES];
static u32                 g_count;
static u8                  g_initialized;

static const char *const k_scan_roots[APP_CATALOG_MAX_ROOTS] = {
    "/",
    "/FREEDOS",
    "/FREEDOS/OPENGEM",
    "/EFI/CiukiOS",
};

static u8 ac_upper(u8 ch) {
    if (ch >= 'a' && ch <= 'z') return (u8)(ch - 'a' + 'A');
    return ch;
}

static int ac_eq_nocase(const char *a, const char *b) {
    while (*a && *b) {
        if (ac_upper((u8)*a) != ac_upper((u8)*b)) return 0;
        a++; b++;
    }
    return (*a == '\0') && (*b == '\0');
}

static u32 ac_strlen(const char *s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static void ac_copy(char *dst, const char *src, u32 cap) {
    u32 i = 0;
    if (cap == 0U) return;
    while (i < cap - 1U && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static u8 ac_kind_from_name(const char *name) {
    u32 n = ac_strlen(name);
    if (n < 5U) return APP_CATALOG_KIND_UNKNOWN;
    const char *ext = name + (n - 4U);
    if (ext[0] != '.') return APP_CATALOG_KIND_UNKNOWN;
    char e1 = (char)ac_upper((u8)ext[1]);
    char e2 = (char)ac_upper((u8)ext[2]);
    char e3 = (char)ac_upper((u8)ext[3]);
    if (e1 == 'C' && e2 == 'O' && e3 == 'M') return APP_CATALOG_KIND_COM;
    if (e1 == 'E' && e2 == 'X' && e3 == 'E') return APP_CATALOG_KIND_EXE;
    if (e1 == 'B' && e2 == 'A' && e3 == 'T') return APP_CATALOG_KIND_BAT;
    return APP_CATALOG_KIND_UNKNOWN;
}

const char *app_catalog_kind_label(u8 kind) {
    switch (kind) {
        case APP_CATALOG_KIND_COM: return "com";
        case APP_CATALOG_KIND_EXE: return "exe";
        case APP_CATALOG_KIND_BAT: return "bat";
        default:                   return "?";
    }
}

static int ac_has_entry(const char *name) {
    u32 i;
    for (i = 0; i < g_count; i++) {
        if (ac_eq_nocase(g_entries[i].name, name)) return 1;
    }
    return 0;
}

static int ac_add(const char *name, const char *path, u8 kind, u8 source) {
    if (g_count >= APP_CATALOG_MAX_ENTRIES) return 0;
    ac_copy(g_entries[g_count].name, name,
            (u32)sizeof(g_entries[g_count].name));
    ac_copy(g_entries[g_count].path, path,
            (u32)sizeof(g_entries[g_count].path));
    g_entries[g_count].kind = kind;
    g_entries[g_count].source = source;
    g_entries[g_count].reserved[0] = 0U;
    g_entries[g_count].reserved[1] = 0U;
    {
        /* [ catalog ] scan entry <name> kind=<com|exe|bat> path=<path> */
        char line[160];
        u32 li = 0;
        const char *p = "[ catalog ] scan entry ";
        while (p[li] && li < sizeof(line) - 1U) { line[li] = p[li]; li++; }
        {
            u32 j = 0;
            while (name[j] && li < sizeof(line) - 1U) line[li++] = name[j++];
        }
        {
            const char *k = " kind=";
            u32 j = 0;
            while (k[j] && li < sizeof(line) - 1U) line[li++] = k[j++];
        }
        {
            const char *kl = app_catalog_kind_label(kind);
            u32 j = 0;
            while (kl[j] && li < sizeof(line) - 1U) line[li++] = kl[j++];
        }
        {
            const char *pp = " path=";
            u32 j = 0;
            while (pp[j] && li < sizeof(line) - 1U) line[li++] = pp[j++];
        }
        {
            u32 j = 0;
            while (path[j] && li < sizeof(line) - 1U) line[li++] = path[j++];
        }
        if (li < sizeof(line) - 1U) line[li++] = '\n';
        line[li] = '\0';
        serial_write(line);
    }
    g_count++;
    return 1;
}

typedef struct ac_scan_ctx {
    const char *root;
} ac_scan_ctx_t;

static int ac_scan_cb(const fat_dir_entry_t *entry, void *ctx_v) {
    ac_scan_ctx_t *ctx = (ac_scan_ctx_t *)ctx_v;
    u8 kind;
    char path[64];
    u32 pi = 0;
    u32 rlen;
    if (entry == (const fat_dir_entry_t *)0) return 0;
    if (entry->attr & (FAT_ATTR_DIRECTORY | FAT_ATTR_VOLUME_ID)) return 0;
    kind = ac_kind_from_name(entry->name);
    if (kind == APP_CATALOG_KIND_UNKNOWN) return 0;
    if (ac_has_entry(entry->name)) return 0;
    /* Build canonical path: root + "/" + name (strip double slashes). */
    rlen = ac_strlen(ctx->root);
    {
        u32 i;
        for (i = 0; i < rlen && pi < sizeof(path) - 1U; i++)
            path[pi++] = ctx->root[i];
        if (pi > 0 && path[pi - 1U] != '/' && pi < sizeof(path) - 1U)
            path[pi++] = '/';
        for (i = 0; entry->name[i] && pi < sizeof(path) - 1U; i++)
            path[pi++] = entry->name[i];
        path[pi] = '\0';
    }
    ac_add(entry->name, path, kind, APP_CATALOG_SRC_FAT);
    return 0;
}

int app_catalog_init(handoff_v0_t *handoff) {
    u32 i;
    u32 roots_scanned = 0;
    g_count = 0;
    g_initialized = 1U;

    /* Lane 1: FAT scan. Only when FAT is ready; otherwise fall back to
     * the handoff COM catalog alone (stage2 still boots). */
    if (fat_ready()) {
        for (i = 0; i < APP_CATALOG_MAX_ROOTS; i++) {
            const char *root = k_scan_roots[i];
            ac_scan_ctx_t ctx;
            {
                /* [ catalog ] scan begin root=<path> */
                char line[96];
                u32 li = 0;
                const char *pfx = "[ catalog ] scan begin root=";
                while (pfx[li] && li < sizeof(line) - 1U) {
                    line[li] = pfx[li]; li++;
                }
                {
                    u32 j = 0;
                    while (root[j] && li < sizeof(line) - 1U)
                        line[li++] = root[j++];
                }
                if (li < sizeof(line) - 1U) line[li++] = '\n';
                line[li] = '\0';
                serial_write(line);
            }
            ctx.root = root;
            (void)fat_list_dir(root, ac_scan_cb, &ctx);
            roots_scanned++;
        }
    } else {
        serial_write("[ catalog ] fat not ready, skipping FAT scan\n");
    }

    /* Lane 2: handoff COM catalog (loader-shipped demo COMs). */
    if (handoff) {
        u64 j;
        u64 cc = handoff->com_count;
        if (cc > HANDOFF_COM_MAX) cc = HANDOFF_COM_MAX;
        for (j = 0; j < cc; j++) {
            handoff_com_entry_t *ce = &handoff->com_entries[j];
            if (ce->phys_base == 0 || ce->name[0] == '\0') continue;
            if (ac_has_entry(ce->name)) continue;
            {
                char synth_path[64];
                const char *prefix = "(handoff)/";
                u32 pi = 0;
                while (prefix[pi] && pi < sizeof(synth_path) - 1U) {
                    synth_path[pi] = prefix[pi]; pi++;
                }
                {
                    u32 k = 0;
                    while (ce->name[k] && pi < sizeof(synth_path) - 1U)
                        synth_path[pi++] = ce->name[k++];
                }
                synth_path[pi] = '\0';
                ac_add(ce->name, synth_path,
                       ac_kind_from_name(ce->name),
                       APP_CATALOG_SRC_HANDOFF);
            }
        }
    }

    {
        /* [ catalog ] scan done entries=<n> roots=<m> */
        char line[96];
        u32 li = 0;
        const char *pfx = "[ catalog ] scan done entries=";
        while (pfx[li] && li < sizeof(line) - 1U) {
            line[li] = pfx[li]; li++;
        }
        {
            u32 n = g_count;
            char buf[12];
            u32 bi = 0;
            if (n == 0U) {
                buf[bi++] = '0';
            } else {
                char tmp[12];
                u32 ti = 0;
                while (n > 0 && ti < sizeof(tmp)) {
                    tmp[ti++] = (char)('0' + (n % 10U));
                    n /= 10U;
                }
                while (ti > 0 && bi < sizeof(buf))
                    buf[bi++] = tmp[--ti];
            }
            {
                u32 k;
                for (k = 0; k < bi && li < sizeof(line) - 1U; k++)
                    line[li++] = buf[k];
            }
        }
        {
            const char *r = " roots=";
            u32 j = 0;
            while (r[j] && li < sizeof(line) - 1U) line[li++] = r[j++];
        }
        {
            u32 n = roots_scanned;
            char buf[6];
            u32 bi = 0;
            if (n == 0U) buf[bi++] = '0';
            else {
                char tmp[6];
                u32 ti = 0;
                while (n > 0 && ti < sizeof(tmp)) {
                    tmp[ti++] = (char)('0' + (n % 10U));
                    n /= 10U;
                }
                while (ti > 0 && bi < sizeof(buf))
                    buf[bi++] = tmp[--ti];
            }
            {
                u32 k;
                for (k = 0; k < bi && li < sizeof(line) - 1U; k++)
                    line[li++] = buf[k];
            }
        }
        if (li < sizeof(line) - 1U) line[li++] = '\n';
        line[li] = '\0';
        serial_write(line);
    }
    return 1;
}

u32 app_catalog_count(void) { return g_count; }

const app_catalog_entry_t *app_catalog_get(u32 index) {
    if (!g_initialized || index >= g_count) {
        return (const app_catalog_entry_t *)0;
    }
    return &g_entries[index];
}

const app_catalog_entry_t *app_catalog_find(const char *name) {
    u32 i;
    if (name == (const char *)0 || name[0] == '\0') {
        return (const app_catalog_entry_t *)0;
    }
    for (i = 0; i < g_count; i++) {
        if (ac_eq_nocase(g_entries[i].name, name)) {
            return &g_entries[i];
        }
    }
    return (const app_catalog_entry_t *)0;
}
