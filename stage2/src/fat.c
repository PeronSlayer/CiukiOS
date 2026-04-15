#include "fat.h"
#include "disk.h"

#define FAT_TYPE_12 12U
#define FAT_TYPE_16 16U
#define FAT_TYPE_32 32U

#define FAT_PATH_MAX_TOKENS 16U
#define FAT_TOKEN_MAX 13U

typedef struct fat_fs {
    int mounted;
    u32 fat_type;
    u32 bytes_per_sector;
    u32 sectors_per_cluster;
    u32 reserved_sectors;
    u32 num_fats;
    u32 fat_size_sectors;
    u32 root_dir_sectors;
    u32 first_fat_sector;
    u32 first_root_sector;
    u32 first_data_sector;
    u32 root_cluster;
    u32 total_sectors;
    u32 total_clusters;
} fat_fs_t;

typedef struct fat_dir_ref {
    int fixed_root;
    u32 start_sector;
    u32 sector_count;
    u32 start_cluster;
} fat_dir_ref_t;

typedef int (*fat_raw_entry_cb_t)(const u8 *entry, void *ctx);

static fat_fs_t g_fs;

static u16 rd16(const u8 *p) {
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static u32 rd32(const u8 *p) {
    return (u32)((u32)p[0] |
                 ((u32)p[1] << 8) |
                 ((u32)p[2] << 16) |
                 ((u32)p[3] << 24));
}

static u8 to_upper_ascii(u8 ch) {
    if (ch >= 'a' && ch <= 'z') {
        return (u8)(ch - ('a' - 'A'));
    }
    return ch;
}

static int str_eq_casefold(const char *a, const char *b) {
    while (*a && *b) {
        if (to_upper_ascii((u8)*a) != to_upper_ascii((u8)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int parse_path_tokens(
    const char *path,
    char tokens[FAT_PATH_MAX_TOKENS][FAT_TOKEN_MAX],
    u32 *token_count_out
) {
    const char *p = path;
    u32 count = 0;

    if (!path || !token_count_out) {
        return 0;
    }

    while (*p) {
        u32 n = 0;

        while (*p == '/' || *p == '\\') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        if (count >= FAT_PATH_MAX_TOKENS) {
            return 0;
        }

        while (*p && *p != '/' && *p != '\\') {
            if ((n + 1) >= FAT_TOKEN_MAX) {
                return 0;
            }
            tokens[count][n++] = (char)to_upper_ascii((u8)*p);
            p++;
        }
        tokens[count][n] = '\0';
        count++;
    }

    *token_count_out = count;
    return 1;
}

static void short_name_from_entry(const u8 *entry, char out[13]) {
    u32 i = 0;
    u32 j = 0;
    u32 ext_len = 0;

    while (i < 8 && entry[i] != ' ') {
        out[j++] = (char)entry[i++];
    }

    for (i = 8; i < 11; i++) {
        if (entry[i] != ' ') {
            ext_len++;
        }
    }

    if (ext_len > 0) {
        out[j++] = '.';
        for (i = 8; i < 11; i++) {
            if (entry[i] != ' ') {
                out[j++] = (char)entry[i];
            }
        }
    }

    out[j] = '\0';
}

static u32 entry_first_cluster(const u8 *entry) {
    u32 hi = (u32)rd16(entry + 20);
    u32 lo = (u32)rd16(entry + 26);
    return (hi << 16) | lo;
}

static const u8 *disk_byte_ptr(u64 absolute_byte_offset) {
    u64 lba;
    u32 off;
    const u8 *sector;

    if (!g_fs.mounted) {
        return (const u8 *)0;
    }

    lba = absolute_byte_offset / (u64)g_fs.bytes_per_sector;
    off = (u32)(absolute_byte_offset % (u64)g_fs.bytes_per_sector);
    sector = stage2_disk_lba_ptr(lba);
    if (!sector) {
        return (const u8 *)0;
    }
    return sector + off;
}

static int disk_read_bytes(u64 absolute_byte_offset, u8 *out, u32 len) {
    for (u32 i = 0; i < len; i++) {
        const u8 *p = disk_byte_ptr(absolute_byte_offset + i);
        if (!p) {
            return 0;
        }
        out[i] = *p;
    }
    return 1;
}

static int cluster_is_eoc(u32 cluster) {
    if (g_fs.fat_type == FAT_TYPE_12) {
        return cluster >= 0x0FF8U;
    }
    if (g_fs.fat_type == FAT_TYPE_16) {
        return cluster >= 0xFFF8U;
    }
    return cluster >= 0x0FFFFFF8U;
}

static u32 fat_next_cluster(u32 cluster) {
    u64 fat_base = (u64)g_fs.first_fat_sector * (u64)g_fs.bytes_per_sector;
    u8 b[4];

    if (g_fs.fat_type == FAT_TYPE_12) {
        u64 offset = (u64)cluster + ((u64)cluster / 2ULL);
        if (!disk_read_bytes(fat_base + offset, b, 2)) {
            return 0;
        }
        {
            u16 v = (u16)((u16)b[0] | ((u16)b[1] << 8));
            if (cluster & 1U) {
                return (u32)((v >> 4) & 0x0FFFU);
            }
            return (u32)(v & 0x0FFFU);
        }
    }

    if (g_fs.fat_type == FAT_TYPE_16) {
        u64 offset = (u64)cluster * 2ULL;
        if (!disk_read_bytes(fat_base + offset, b, 2)) {
            return 0;
        }
        return (u32)rd16(b);
    }

    {
        u64 offset = (u64)cluster * 4ULL;
        if (!disk_read_bytes(fat_base + offset, b, 4)) {
            return 0;
        }
        return rd32(b) & 0x0FFFFFFFU;
    }
}

static u32 cluster_first_sector(u32 cluster) {
    return g_fs.first_data_sector + ((cluster - 2U) * g_fs.sectors_per_cluster);
}

static int fat_iter_dir(const fat_dir_ref_t *dir, fat_raw_entry_cb_t cb, void *ctx) {
    u32 entries_per_sector;

    if (!dir || !cb || !g_fs.mounted) {
        return 0;
    }

    entries_per_sector = g_fs.bytes_per_sector / 32U;
    if (entries_per_sector == 0) {
        return 0;
    }

    if (dir->fixed_root) {
        for (u32 s = 0; s < dir->sector_count; s++) {
            const u8 *sector = stage2_disk_lba_ptr((u64)dir->start_sector + s);
            if (!sector) {
                return 0;
            }
            for (u32 i = 0; i < entries_per_sector; i++) {
                const u8 *entry = sector + (i * 32U);
                if (entry[0] == 0x00U) {
                    return 1;
                }
                if (entry[0] == 0xE5U) {
                    continue;
                }
                if (entry[11] == FAT_ATTR_LONG_NAME) {
                    continue;
                }
                if (!cb(entry, ctx)) {
                    return 1;
                }
            }
        }
        return 1;
    }

    {
        u32 cluster = dir->start_cluster;
        u32 guard = g_fs.total_clusters + 4U;

        while (cluster >= 2U && guard-- > 0) {
            u32 first_sector = cluster_first_sector(cluster);
            for (u32 s = 0; s < g_fs.sectors_per_cluster; s++) {
                const u8 *sector = stage2_disk_lba_ptr((u64)first_sector + s);
                if (!sector) {
                    return 0;
                }
                for (u32 i = 0; i < entries_per_sector; i++) {
                    const u8 *entry = sector + (i * 32U);
                    if (entry[0] == 0x00U) {
                        return 1;
                    }
                    if (entry[0] == 0xE5U) {
                        continue;
                    }
                    if (entry[11] == FAT_ATTR_LONG_NAME) {
                        continue;
                    }
                    if (!cb(entry, ctx)) {
                        return 1;
                    }
                }
            }

            if (cluster_is_eoc(cluster)) {
                break;
            }
            cluster = fat_next_cluster(cluster);
            if (cluster_is_eoc(cluster)) {
                break;
            }
            if (cluster == 0U || cluster == 1U) {
                break;
            }
        }
    }

    return 1;
}

typedef struct find_ctx {
    const char *target_name;
    int found;
    fat_dir_entry_t out;
} find_ctx_t;

static void fat_public_from_raw(const u8 *entry, fat_dir_entry_t *out) {
    short_name_from_entry(entry, out->name);
    out->attr = entry[11];
    out->first_cluster = entry_first_cluster(entry);
    out->size = rd32(entry + 28);
}

static int fat_find_cb(const u8 *entry, void *ctx_void) {
    find_ctx_t *ctx = (find_ctx_t *)ctx_void;
    char name[13];

    short_name_from_entry(entry, name);
    if (!str_eq_casefold(name, ctx->target_name)) {
        return 1;
    }

    fat_public_from_raw(entry, &ctx->out);
    ctx->found = 1;
    return 0;
}

static int fat_find_in_dir(const fat_dir_ref_t *dir, const char *name, fat_dir_entry_t *out) {
    find_ctx_t ctx;
    ctx.target_name = name;
    ctx.found = 0;

    if (!fat_iter_dir(dir, fat_find_cb, &ctx)) {
        return 0;
    }
    if (!ctx.found) {
        return 0;
    }
    if (out) {
        *out = ctx.out;
    }
    return 1;
}

static void fat_root_dir(fat_dir_ref_t *out) {
    if (g_fs.fat_type == FAT_TYPE_32) {
        out->fixed_root = 0;
        out->start_sector = 0;
        out->sector_count = 0;
        out->start_cluster = g_fs.root_cluster;
        return;
    }

    out->fixed_root = 1;
    out->start_sector = g_fs.first_root_sector;
    out->sector_count = g_fs.root_dir_sectors;
    out->start_cluster = 0;
}

static int fat_open_dir_path(
    char tokens[FAT_PATH_MAX_TOKENS][FAT_TOKEN_MAX],
    u32 token_count,
    fat_dir_ref_t *out
) {
    fat_dir_ref_t dir;

    fat_root_dir(&dir);
    if (token_count == 0) {
        *out = dir;
        return 1;
    }

    for (u32 i = 0; i < token_count; i++) {
        fat_dir_entry_t e;
        if (!fat_find_in_dir(&dir, tokens[i], &e)) {
            return 0;
        }
        if ((e.attr & FAT_ATTR_DIRECTORY) == 0) {
            return 0;
        }
        dir.fixed_root = 0;
        dir.start_sector = 0;
        dir.sector_count = 0;
        dir.start_cluster = e.first_cluster;
    }

    *out = dir;
    return 1;
}

int fat_init(void) {
    const u8 *bs;
    u32 root_entries;
    u32 total_sectors;
    u32 fat_size;
    u32 data_sectors;
    u32 cluster_count;
    u32 disk_block_size;

    g_fs.mounted = 0;

    if (!stage2_disk_ready()) {
        return 0;
    }

    bs = stage2_disk_lba_ptr(0);
    if (!bs) {
        return 0;
    }

    disk_block_size = stage2_disk_block_size();
    g_fs.bytes_per_sector = (u32)rd16(bs + 11);
    g_fs.sectors_per_cluster = (u32)bs[13];
    g_fs.reserved_sectors = (u32)rd16(bs + 14);
    g_fs.num_fats = (u32)bs[16];
    root_entries = (u32)rd16(bs + 17);

    if (g_fs.bytes_per_sector == 0 ||
        g_fs.sectors_per_cluster == 0 ||
        g_fs.num_fats == 0 ||
        g_fs.reserved_sectors == 0) {
        return 0;
    }

    if (disk_block_size != g_fs.bytes_per_sector) {
        return 0;
    }

    total_sectors = (u32)rd16(bs + 19);
    if (total_sectors == 0) {
        total_sectors = rd32(bs + 32);
    }

    fat_size = (u32)rd16(bs + 22);
    if (fat_size == 0) {
        fat_size = rd32(bs + 36);
    }

    if (total_sectors == 0 || fat_size == 0) {
        return 0;
    }

    g_fs.total_sectors = total_sectors;
    g_fs.fat_size_sectors = fat_size;
    g_fs.root_dir_sectors =
        ((root_entries * 32U) + (g_fs.bytes_per_sector - 1U)) / g_fs.bytes_per_sector;
    g_fs.first_fat_sector = g_fs.reserved_sectors;
    g_fs.first_root_sector = g_fs.reserved_sectors + (g_fs.num_fats * g_fs.fat_size_sectors);
    g_fs.first_data_sector = g_fs.first_root_sector + g_fs.root_dir_sectors;

    if (total_sectors < g_fs.first_data_sector) {
        return 0;
    }

    data_sectors = total_sectors - g_fs.first_data_sector;
    cluster_count = data_sectors / g_fs.sectors_per_cluster;
    g_fs.total_clusters = cluster_count;

    if (cluster_count < 4085U) {
        g_fs.fat_type = FAT_TYPE_12;
        g_fs.root_cluster = 0;
    } else if (cluster_count < 65525U) {
        g_fs.fat_type = FAT_TYPE_16;
        g_fs.root_cluster = 0;
    } else {
        g_fs.fat_type = FAT_TYPE_32;
        g_fs.root_cluster = rd32(bs + 44);
        if (g_fs.root_cluster < 2U) {
            g_fs.root_cluster = 2U;
        }
    }

    g_fs.mounted = 1;
    return 1;
}

int fat_ready(void) {
    return g_fs.mounted;
}

typedef struct list_ctx {
    fat_dir_enum_cb_t user_cb;
    void *user_ctx;
} list_ctx_t;

static int fat_list_cb(const u8 *entry, void *ctx_void) {
    list_ctx_t *ctx = (list_ctx_t *)ctx_void;
    fat_dir_entry_t out;

    if ((entry[11] & FAT_ATTR_VOLUME_ID) != 0) {
        return 1;
    }

    fat_public_from_raw(entry, &out);
    return ctx->user_cb(&out, ctx->user_ctx);
}

int fat_list_dir(const char *path, fat_dir_enum_cb_t cb, void *ctx) {
    char tokens[FAT_PATH_MAX_TOKENS][FAT_TOKEN_MAX];
    u32 token_count = 0;
    fat_dir_ref_t dir;
    list_ctx_t lctx;

    if (!g_fs.mounted || !cb) {
        return 0;
    }
    if (!parse_path_tokens(path ? path : "/", tokens, &token_count)) {
        return 0;
    }
    if (!fat_open_dir_path(tokens, token_count, &dir)) {
        return 0;
    }

    lctx.user_cb = cb;
    lctx.user_ctx = ctx;
    return fat_iter_dir(&dir, fat_list_cb, &lctx);
}

int fat_find_file(const char *path, fat_dir_entry_t *out) {
    char tokens[FAT_PATH_MAX_TOKENS][FAT_TOKEN_MAX];
    u32 token_count = 0;
    fat_dir_ref_t dir;

    if (!g_fs.mounted || !path || !out) {
        return 0;
    }
    if (!parse_path_tokens(path, tokens, &token_count)) {
        return 0;
    }
    if (token_count == 0) {
        return 0;
    }

    if (token_count == 1) {
        fat_root_dir(&dir);
        return fat_find_in_dir(&dir, tokens[0], out);
    }

    if (!fat_open_dir_path(tokens, token_count - 1U, &dir)) {
        return 0;
    }
    return fat_find_in_dir(&dir, tokens[token_count - 1U], out);
}

int fat_read_file(const char *path, void *out, u32 out_capacity, u32 *out_size) {
    fat_dir_entry_t entry;
    u8 *dst = (u8 *)out;
    u32 remaining;
    u32 cluster;
    u32 guard;

    if (out_size) {
        *out_size = 0;
    }

    if (!g_fs.mounted || !path || !out) {
        return 0;
    }
    if (!fat_find_file(path, &entry)) {
        return 0;
    }
    if ((entry.attr & FAT_ATTR_DIRECTORY) != 0) {
        return 0;
    }
    if (entry.size > out_capacity) {
        return 0;
    }
    if (entry.size == 0U) {
        if (out_size) {
            *out_size = 0;
        }
        return 1;
    }

    cluster = entry.first_cluster;
    if (cluster < 2U) {
        return 0;
    }

    remaining = entry.size;
    guard = g_fs.total_clusters + 4U;

    while (remaining > 0U && guard-- > 0U) {
        u32 first_sector = cluster_first_sector(cluster);

        for (u32 s = 0; s < g_fs.sectors_per_cluster && remaining > 0U; s++) {
            const u8 *sector = stage2_disk_lba_ptr((u64)first_sector + s);
            u32 to_copy;

            if (!sector) {
                return 0;
            }

            to_copy = remaining;
            if (to_copy > g_fs.bytes_per_sector) {
                to_copy = g_fs.bytes_per_sector;
            }

            for (u32 i = 0; i < to_copy; i++) {
                *dst++ = sector[i];
            }
            remaining -= to_copy;
        }

        if (remaining == 0U) {
            break;
        }

        if (cluster_is_eoc(cluster)) {
            return 0;
        }

        cluster = fat_next_cluster(cluster);
        if (cluster < 2U || cluster_is_eoc(cluster)) {
            if (remaining != 0U) {
                return 0;
            }
            break;
        }

    }

    if (remaining != 0U) {
        return 0;
    }

    if (out_size) {
        *out_size = entry.size;
    }
    return 1;
}
