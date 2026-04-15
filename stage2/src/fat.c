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

typedef struct fat_dir_slot {
    u64 lba;
    u32 offset;
} fat_dir_slot_t;

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

static void wr16(u8 *p, u16 v) {
    p[0] = (u8)(v & 0x00FFU);
    p[1] = (u8)((v >> 8) & 0x00FFU);
}

static void wr32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0x000000FFU);
    p[1] = (u8)((v >> 8) & 0x000000FFU);
    p[2] = (u8)((v >> 16) & 0x000000FFU);
    p[3] = (u8)((v >> 24) & 0x000000FFU);
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

static u8 *disk_byte_ptr_rw(u64 absolute_byte_offset) {
    u64 lba;
    u32 off;
    u8 *sector;

    if (!g_fs.mounted) {
        return (u8 *)0;
    }

    lba = absolute_byte_offset / (u64)g_fs.bytes_per_sector;
    off = (u32)(absolute_byte_offset % (u64)g_fs.bytes_per_sector);
    sector = stage2_disk_lba_ptr_rw(lba);
    if (!sector) {
        return (u8 *)0;
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

static int disk_write_bytes(u64 absolute_byte_offset, const u8 *src, u32 len) {
    for (u32 i = 0; i < len; i++) {
        u8 *p = disk_byte_ptr_rw(absolute_byte_offset + i);
        if (!p) {
            return 0;
        }
        *p = src[i];
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

static int fat_set_cluster_value_one(u32 fat_index, u32 cluster, u32 value) {
    u64 fat_base =
        (u64)(g_fs.first_fat_sector + (fat_index * g_fs.fat_size_sectors)) *
        (u64)g_fs.bytes_per_sector;
    u8 b[4];

    if (g_fs.fat_type == FAT_TYPE_12) {
        u64 offset = (u64)cluster + ((u64)cluster / 2ULL);
        if (!disk_read_bytes(fat_base + offset, b, 2)) {
            return 0;
        }
        {
            u16 v = rd16(b);
            u16 nv;
            if (cluster & 1U) {
                nv = (u16)((v & 0x000FU) | ((u16)(value & 0x0FFFU) << 4));
            } else {
                nv = (u16)((v & 0xF000U) | ((u16)(value & 0x0FFFU)));
            }
            wr16(b, nv);
        }
        return disk_write_bytes(fat_base + offset, b, 2);
    }

    if (g_fs.fat_type == FAT_TYPE_16) {
        u64 offset = (u64)cluster * 2ULL;
        wr16(b, (u16)(value & 0xFFFFU));
        return disk_write_bytes(fat_base + offset, b, 2);
    }

    {
        u64 offset = (u64)cluster * 4ULL;
        u32 cur;
        if (!disk_read_bytes(fat_base + offset, b, 4)) {
            return 0;
        }
        cur = rd32(b);
        cur = (cur & 0xF0000000U) | (value & 0x0FFFFFFFU);
        wr32(b, cur);
        return disk_write_bytes(fat_base + offset, b, 4);
    }
}

static int fat_set_cluster_value(u32 cluster, u32 value) {
    for (u32 fat_i = 0; fat_i < g_fs.num_fats; fat_i++) {
        if (!fat_set_cluster_value_one(fat_i, cluster, value)) {
            return 0;
        }
    }
    return 1;
}

static int fat_free_chain(u32 start_cluster) {
    u32 cluster = start_cluster;
    u32 guard = g_fs.total_clusters + 4U;

    if (cluster < 2U) {
        return 1;
    }

    while (cluster >= 2U && guard-- > 0U) {
        u32 next = fat_next_cluster(cluster);
        if (!fat_set_cluster_value(cluster, 0U)) {
            return 0;
        }

        if (next == 0U || next == 1U || cluster_is_eoc(next)) {
            break;
        }
        cluster = next;
    }

    return 1;
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

static int fat_find_in_dir_with_slot(
    const fat_dir_ref_t *dir,
    const char *name,
    fat_dir_entry_t *out,
    fat_dir_slot_t *slot
) {
    u32 entries_per_sector;

    if (!dir || !name) {
        return 0;
    }

    entries_per_sector = g_fs.bytes_per_sector / 32U;
    if (entries_per_sector == 0U) {
        return 0;
    }

    if (dir->fixed_root) {
        for (u32 s = 0; s < dir->sector_count; s++) {
            u64 lba = (u64)dir->start_sector + s;
            const u8 *sector = stage2_disk_lba_ptr(lba);
            if (!sector) {
                return 0;
            }
            for (u32 i = 0; i < entries_per_sector; i++) {
                const u8 *entry = sector + (i * 32U);
                char short_name[13];
                if (entry[0] == 0x00U) {
                    return 0;
                }
                if (entry[0] == 0xE5U || entry[11] == FAT_ATTR_LONG_NAME) {
                    continue;
                }
                if ((entry[11] & FAT_ATTR_VOLUME_ID) != 0U) {
                    continue;
                }
                short_name_from_entry(entry, short_name);
                if (!str_eq_casefold(short_name, name)) {
                    continue;
                }
                if (out) {
                    fat_public_from_raw(entry, out);
                }
                if (slot) {
                    slot->lba = lba;
                    slot->offset = i * 32U;
                }
                return 1;
            }
        }
        return 0;
    }

    {
        u32 cluster = dir->start_cluster;
        u32 guard = g_fs.total_clusters + 4U;

        while (cluster >= 2U && guard-- > 0U) {
            u32 first_sector = cluster_first_sector(cluster);
            for (u32 s = 0; s < g_fs.sectors_per_cluster; s++) {
                u64 lba = (u64)first_sector + s;
                const u8 *sector = stage2_disk_lba_ptr(lba);
                if (!sector) {
                    return 0;
                }
                for (u32 i = 0; i < entries_per_sector; i++) {
                    const u8 *entry = sector + (i * 32U);
                    char short_name[13];
                    if (entry[0] == 0x00U) {
                        return 0;
                    }
                    if (entry[0] == 0xE5U || entry[11] == FAT_ATTR_LONG_NAME) {
                        continue;
                    }
                    if ((entry[11] & FAT_ATTR_VOLUME_ID) != 0U) {
                        continue;
                    }
                    short_name_from_entry(entry, short_name);
                    if (!str_eq_casefold(short_name, name)) {
                        continue;
                    }
                    if (out) {
                        fat_public_from_raw(entry, out);
                    }
                    if (slot) {
                        slot->lba = lba;
                        slot->offset = i * 32U;
                    }
                    return 1;
                }
            }

            if (cluster_is_eoc(cluster)) {
                break;
            }
            cluster = fat_next_cluster(cluster);
            if (cluster == 0U || cluster == 1U || cluster_is_eoc(cluster)) {
                break;
            }
        }
    }

    return 0;
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

static int fat_locate_path_entry(
    const char *path,
    fat_dir_entry_t *entry_out,
    fat_dir_slot_t *slot_out
) {
    char tokens[FAT_PATH_MAX_TOKENS][FAT_TOKEN_MAX];
    u32 token_count = 0;
    fat_dir_ref_t dir;

    if (!path || !entry_out || !slot_out) {
        return 0;
    }
    if (!parse_path_tokens(path, tokens, &token_count)) {
        return 0;
    }
    if (token_count == 0U) {
        return 0;
    }

    if (token_count == 1U) {
        fat_root_dir(&dir);
    } else {
        if (!fat_open_dir_path(tokens, token_count - 1U, &dir)) {
            return 0;
        }
    }

    if (!fat_find_in_dir_with_slot(&dir, tokens[token_count - 1U], entry_out, slot_out)) {
        return 0;
    }

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
    /* Skip '.' and '..' entries — not shown in directory listings */
    if (entry[0] == '.' && (entry[1] == ' ' || entry[1] == '.')) {
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

/* -----------------------------------------------------------------------
 * Write-path helpers
 * --------------------------------------------------------------------- */

/*
 * Validate a single character for use in a DOS 8.3 filename.
 * Accepts A-Z, 0-9 and a subset of printable specials. Spaces and
 * path-separator characters are rejected.
 */
static int is_valid_83_char(u8 ch) {
    if (ch < 0x20U || ch == 0x7FU) {
        return 0;
    }
    /* Chars explicitly forbidden in FAT 8.3 names */
    static const u8 forbidden[] = {
        ' ', '"', '*', '+', ',', '/', ':', ';',
        '<', '=', '>', '?', '[', '\\', ']', '|', 0
    };
    for (u32 i = 0; forbidden[i] != 0; i++) {
        if (ch == forbidden[i]) {
            return 0;
        }
    }
    return 1;
}

/*
 * Convert a display-form "NAME.EXT" (already upper-cased) into the
 * on-disk 11-byte space-padded 8.3 representation.
 * Returns 1 on success, 0 if the name is invalid or too long.
 */
static int fat_normalize_83_name(const char *name, u8 out11[11]) {
    u32 i;
    const char *dot = (const char *)0;
    const char *p;

    if (!name || !out11) {
        return 0;
    }

    /* Find last dot */
    for (p = name; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
    }

    /* Fill with spaces */
    for (i = 0; i < 11U; i++) {
        out11[i] = ' ';
    }

    /* Name part (max 8 chars before the dot, or whole string if no dot) */
    p = name;
    i = 0;
    while (*p && *p != '.' && i < 8U) {
        u8 ch = (u8)*p;
        if (!is_valid_83_char(ch)) {
            return 0;
        }
        out11[i++] = ch;
        p++;
    }
    if (i == 0U) {
        return 0; /* empty name part */
    }
    if (*p && *p != '.') {
        return 0; /* name part > 8 chars */
    }

    /* Extension part (max 3 chars after the last dot, if any) */
    if (dot && dot[1] != '\0') {
        p = dot + 1;
        i = 8U;
        while (*p && i < 11U) {
            u8 ch = (u8)*p;
            if (!is_valid_83_char(ch)) {
                return 0;
            }
            out11[i++] = ch;
            p++;
        }
        if (*p != '\0') {
            return 0; /* extension > 3 chars */
        }
    }

    return 1;
}

/*
 * Walk the FAT looking for the first free cluster (entry == 0).
 * Returns the cluster number or 0 if the volume is full.
 */
static u32 fat_find_free_cluster(void) {
    u32 limit = g_fs.total_clusters + 2U;
    for (u32 c = 2U; c < limit; c++) {
        if (fat_next_cluster(c) == 0U) {
            return c;
        }
    }
    return 0U;
}

/*
 * Allocate a chain of `count` free clusters and link them together.
 * The last cluster is marked EOC.  Sets *first_out to the head cluster.
 * Returns 1 on success, 0 on failure (e.g. disk full).
 * On failure, any partially-allocated clusters are freed.
 */
static int fat_alloc_chain(u32 count, u32 *first_out) {
    u32 eoc;
    u32 prev = 0U;
    u32 first = 0U;

    if (count == 0U || !first_out) {
        return 0;
    }

    if (g_fs.fat_type == FAT_TYPE_12) {
        eoc = 0x0FFFU;
    } else if (g_fs.fat_type == FAT_TYPE_16) {
        eoc = 0xFFFFU;
    } else {
        eoc = 0x0FFFFFFFU;
    }

    for (u32 i = 0U; i < count; i++) {
        u32 c = fat_find_free_cluster();
        if (c == 0U) {
            /* Disk full — release what we have so far */
            if (first != 0U) {
                fat_free_chain(first);
            }
            return 0;
        }

        /* Mark new cluster as EOC immediately so the next search skips it */
        if (!fat_set_cluster_value(c, eoc)) {
            if (first != 0U) {
                fat_free_chain(first);
            }
            return 0;
        }

        if (prev != 0U) {
            /* Link previous → current */
            if (!fat_set_cluster_value(prev, c)) {
                fat_free_chain(first);
                return 0;
            }
        } else {
            first = c;
        }
        prev = c;
    }

    *first_out = first;
    return 1;
}

/*
 * Write `size` bytes from `data` into the cluster chain starting at
 * `start_cluster`.  The last (partial) sector is zero-padded to the
 * sector boundary.
 * Returns 1 on success, 0 on I/O error or chain-too-short.
 */
static int fat_write_cluster_data(u32 start_cluster, const u8 *data, u32 size) {
    u32 cluster = start_cluster;
    const u8 *src = data;
    u32 remaining = size;
    u32 guard = g_fs.total_clusters + 4U;

    while (remaining > 0U && guard-- > 0U) {
        u32 first_sector = cluster_first_sector(cluster);
        for (u32 s = 0U; s < g_fs.sectors_per_cluster && remaining > 0U; s++) {
            u8 *sector = stage2_disk_lba_ptr_rw((u64)first_sector + s);
            u32 to_write;
            u32 i;

            if (!sector) {
                return 0;
            }

            to_write = remaining;
            if (to_write > g_fs.bytes_per_sector) {
                to_write = g_fs.bytes_per_sector;
            }

            for (i = 0U; i < to_write; i++) {
                sector[i] = src[i];
            }
            /* Zero-pad the remainder of the last sector */
            for (i = to_write; i < g_fs.bytes_per_sector; i++) {
                sector[i] = 0U;
            }

            src += to_write;
            remaining -= to_write;
        }

        if (remaining == 0U) {
            break;
        }

        if (cluster_is_eoc(cluster)) {
            return 0; /* chain too short */
        }
        cluster = fat_next_cluster(cluster);
        if (cluster < 2U || cluster_is_eoc(cluster)) {
            break;
        }
    }

    return remaining == 0U;
}

/*
 * Find a free (0x00 or 0xE5) directory entry slot in `dir`.
 * Returns 1 and fills *slot_out on success, 0 if directory is full.
 */
static int fat_find_free_dir_slot(const fat_dir_ref_t *dir, fat_dir_slot_t *slot_out) {
    u32 entries_per_sector;

    if (!dir || !slot_out || !g_fs.mounted) {
        return 0;
    }

    entries_per_sector = g_fs.bytes_per_sector / 32U;
    if (entries_per_sector == 0U) {
        return 0;
    }

    if (dir->fixed_root) {
        for (u32 s = 0U; s < dir->sector_count; s++) {
            u64 lba = (u64)dir->start_sector + s;
            const u8 *sector = stage2_disk_lba_ptr(lba);
            if (!sector) {
                return 0;
            }
            for (u32 i = 0U; i < entries_per_sector; i++) {
                u8 b0 = sector[i * 32U];
                if (b0 == 0x00U || b0 == 0xE5U) {
                    slot_out->lba    = lba;
                    slot_out->offset = i * 32U;
                    return 1;
                }
            }
        }
        return 0; /* root directory full */
    }

    {
        u32 cluster = dir->start_cluster;
        u32 guard   = g_fs.total_clusters + 4U;

        while (cluster >= 2U && guard-- > 0U) {
            u32 first_sector = cluster_first_sector(cluster);
            for (u32 s = 0U; s < g_fs.sectors_per_cluster; s++) {
                u64 lba = (u64)first_sector + s;
                const u8 *sector = stage2_disk_lba_ptr(lba);
                if (!sector) {
                    return 0;
                }
                for (u32 i = 0U; i < entries_per_sector; i++) {
                    u8 b0 = sector[i * 32U];
                    if (b0 == 0x00U || b0 == 0xE5U) {
                        slot_out->lba    = lba;
                        slot_out->offset = i * 32U;
                        return 1;
                    }
                }
            }
            if (cluster_is_eoc(cluster)) {
                break;
            }
            cluster = fat_next_cluster(cluster);
            if (cluster < 2U || cluster_is_eoc(cluster)) {
                break;
            }
        }
    }

    return 0;
}

/*
 * Write a new 32-byte directory entry into the slot described by *slot.
 * name83 must be the 11-byte space-padded 8.3 name (no dot, uppercase).
 */
static int fat_write_dir_entry_slot(
    const fat_dir_slot_t *slot,
    const u8 name83[11],
    u8 attr,
    u32 start_cluster,
    u32 size
) {
    u8 *sector;
    u8 *entry;
    u32 i;

    if (!slot || !name83) {
        return 0;
    }

    sector = stage2_disk_lba_ptr_rw(slot->lba);
    if (!sector) {
        return 0;
    }

    entry = sector + slot->offset;

    /* Zero the entire 32-byte entry first */
    for (i = 0U; i < 32U; i++) {
        entry[i] = 0U;
    }

    /* Name field (bytes 0-10) */
    for (i = 0U; i < 11U; i++) {
        entry[i] = name83[i];
    }

    /* Attributes (byte 11) */
    entry[11] = attr;

    /* First cluster high word (bytes 20-21, FAT32 only; 0 on FAT12/16) */
    wr16(entry + 20, (u16)((start_cluster >> 16) & 0xFFFFU));

    /* First cluster low word (bytes 26-27) */
    wr16(entry + 26, (u16)(start_cluster & 0xFFFFU));

    /* File size (bytes 28-31) */
    wr32(entry + 28, size);

    return 1;
}

int fat_write_file(const char *path, const void *data, u32 size) {
    char tokens[FAT_PATH_MAX_TOKENS][FAT_TOKEN_MAX];
    u32 token_count = 0;
    fat_dir_ref_t parent_dir;
    fat_dir_slot_t slot;
    u8 name83[11];
    u32 start_cluster = 0U;
    fat_dir_entry_t existing;

    if (!g_fs.mounted || !path || (!data && size > 0U)) {
        return 0;
    }
    if (!parse_path_tokens(path, tokens, &token_count)) {
        return 0;
    }
    if (token_count == 0U) {
        return 0;
    }

    /* Validate and produce the 11-byte 8.3 name from the last token */
    if (!fat_normalize_83_name(tokens[token_count - 1U], name83)) {
        return 0;
    }

    /* Locate parent directory */
    if (token_count == 1U) {
        fat_root_dir(&parent_dir);
    } else {
        if (!fat_open_dir_path(tokens, token_count - 1U, &parent_dir)) {
            return 0;
        }
    }

    /* Fail if an entry with this name already exists */
    if (fat_find_in_dir(&parent_dir, tokens[token_count - 1U], &existing)) {
        return 0;
    }

    /* Allocate cluster chain for the data (skip if size == 0) */
    if (size > 0U) {
        u32 cluster_bytes = g_fs.sectors_per_cluster * g_fs.bytes_per_sector;
        u32 cluster_count = (size + cluster_bytes - 1U) / cluster_bytes;

        if (!fat_alloc_chain(cluster_count, &start_cluster)) {
            return 0;
        }

        if (!fat_write_cluster_data(start_cluster, (const u8 *)data, size)) {
            fat_free_chain(start_cluster);
            return 0;
        }
    }

    /* Find a free slot in the parent directory */
    if (!fat_find_free_dir_slot(&parent_dir, &slot)) {
        if (size > 0U) {
            fat_free_chain(start_cluster);
        }
        return 0;
    }

    /* Write the directory entry */
    if (!fat_write_dir_entry_slot(&slot, name83, FAT_ATTR_ARCHIVE, start_cluster, size)) {
        if (size > 0U) {
            fat_free_chain(start_cluster);
        }
        return 0;
    }

    return 1;
}

/* -----------------------------------------------------------------------
 * Rename, mkdir, rmdir
 * --------------------------------------------------------------------- */

/*
 * Rename an existing file or directory entry within the same parent directory.
 * new_name is a display-form 8.3 name (e.g. "FILE.TXT"); path separators are
 * not allowed.  Cross-directory rename is not supported.
 * Returns 1 on success, 0 on failure.
 */
int fat_rename_entry(const char *old_path, const char *new_name) {
    char tokens[FAT_PATH_MAX_TOKENS][FAT_TOKEN_MAX];
    u32 token_count = 0;
    fat_dir_entry_t old_entry;
    fat_dir_slot_t  old_slot;
    fat_dir_ref_t   parent_dir;
    fat_dir_entry_t existing;
    char new_upper[FAT_TOKEN_MAX];
    u8   new_name83[11];
    u8  *sector;
    u32  i;

    if (!g_fs.mounted || !old_path || !new_name || new_name[0] == '\0') {
        return 0;
    }

    /* Parse old path */
    if (!parse_path_tokens(old_path, tokens, &token_count) || token_count == 0U) {
        return 0;
    }

    /* Locate old entry + its directory slot */
    if (!fat_locate_path_entry(old_path, &old_entry, &old_slot)) {
        return 0;
    }

    /* Upper-case the new name (parse_path_tokens already uppercases tokens;
     * new_name comes from the shell and may be mixed case). */
    for (i = 0U; new_name[i] && i < FAT_TOKEN_MAX - 1U; i++) {
        new_upper[i] = (char)to_upper_ascii((u8)new_name[i]);
    }
    new_upper[i] = '\0';

    /* Validate and produce the 11-byte padded 8.3 representation */
    if (!fat_normalize_83_name(new_upper, new_name83)) {
        return 0;
    }

    /* Resolve the parent directory */
    if (token_count == 1U) {
        fat_root_dir(&parent_dir);
    } else {
        if (!fat_open_dir_path(tokens, token_count - 1U, &parent_dir)) {
            return 0;
        }
    }

    /* Reject if a different entry with the new name already exists */
    if (fat_find_in_dir(&parent_dir, new_upper, &existing)) {
        return 0;
    }

    /* Overwrite the name bytes in the existing directory entry */
    sector = stage2_disk_lba_ptr_rw(old_slot.lba);
    if (!sector) {
        return 0;
    }
    for (i = 0U; i < 11U; i++) {
        sector[old_slot.offset + i] = new_name83[i];
    }

    return 1;
}

/*
 * Create a new empty directory at path.
 * The parent directory must already exist; the name must not.
 * Initializes the new directory cluster with '.' and '..' entries.
 * Returns 1 on success, 0 on failure.
 */
int fat_create_dir(const char *path) {
    char tokens[FAT_PATH_MAX_TOKENS][FAT_TOKEN_MAX];
    u32 token_count = 0;
    fat_dir_ref_t parent_dir;
    fat_dir_slot_t slot;
    fat_dir_entry_t existing;
    u8  name83[11];
    u32 new_cluster;
    u32 parent_cluster;
    u8 *sector;
    u8 *entry;
    u32 i;

    if (!g_fs.mounted || !path) {
        return 0;
    }
    if (!parse_path_tokens(path, tokens, &token_count) || token_count == 0U) {
        return 0;
    }

    /* Validate the 8.3 name for the new directory */
    if (!fat_normalize_83_name(tokens[token_count - 1U], name83)) {
        return 0;
    }

    /* Resolve parent */
    if (token_count == 1U) {
        fat_root_dir(&parent_dir);
    } else {
        if (!fat_open_dir_path(tokens, token_count - 1U, &parent_dir)) {
            return 0;
        }
    }

    /* Fail if the name already exists */
    if (fat_find_in_dir(&parent_dir, tokens[token_count - 1U], &existing)) {
        return 0;
    }

    /* Allocate one cluster for the new directory */
    if (!fat_alloc_chain(1U, &new_cluster)) {
        return 0;
    }

    /* Determine the parent cluster for the '..' entry.
     * Fixed root directories are referenced by cluster 0 in DOS convention. */
    parent_cluster = parent_dir.fixed_root ? 0U : parent_dir.start_cluster;

    /* Zero all sectors in the new cluster */
    {
        u32 first_sector = cluster_first_sector(new_cluster);
        for (u32 s = 0U; s < g_fs.sectors_per_cluster; s++) {
            sector = stage2_disk_lba_ptr_rw((u64)first_sector + s);
            if (!sector) {
                fat_free_chain(new_cluster);
                return 0;
            }
            for (i = 0U; i < g_fs.bytes_per_sector; i++) {
                sector[i] = 0U;
            }
        }

        /* Write '.' entry at offset 0 of the first sector */
        sector = stage2_disk_lba_ptr_rw((u64)first_sector);
        if (!sector) {
            fat_free_chain(new_cluster);
            return 0;
        }

        entry = sector + 0U;
        entry[0] = '.';
        for (i = 1U; i < 11U; i++) { entry[i] = ' '; }
        entry[11] = FAT_ATTR_DIRECTORY;
        wr16(entry + 20, (u16)((new_cluster >> 16) & 0xFFFFU));
        wr16(entry + 26, (u16)(new_cluster & 0xFFFFU));
        wr32(entry + 28, 0U);

        /* Write '..' entry at offset 32 */
        entry = sector + 32U;
        entry[0] = '.';
        entry[1] = '.';
        for (i = 2U; i < 11U; i++) { entry[i] = ' '; }
        entry[11] = FAT_ATTR_DIRECTORY;
        wr16(entry + 20, (u16)((parent_cluster >> 16) & 0xFFFFU));
        wr16(entry + 26, (u16)(parent_cluster & 0xFFFFU));
        wr32(entry + 28, 0U);
    }

    /* Find a free slot in the parent directory */
    if (!fat_find_free_dir_slot(&parent_dir, &slot)) {
        fat_free_chain(new_cluster);
        return 0;
    }

    /* Write the new directory's entry in the parent */
    if (!fat_write_dir_entry_slot(&slot, name83, FAT_ATTR_DIRECTORY, new_cluster, 0U)) {
        fat_free_chain(new_cluster);
        return 0;
    }

    return 1;
}

/* Callback for fat_remove_dir: sets *result=0 if any real entry is found */
static int fat_is_not_empty_cb(const u8 *entry, void *ctx_void) {
    int *result = (int *)ctx_void;
    /* Skip '.' and '..' — they do not make a directory non-empty */
    if (entry[0] == '.' && (entry[1] == ' ' || entry[1] == '.')) {
        return 1;
    }
    *result = 0; /* found a real entry */
    return 0;    /* stop iteration */
}

/*
 * Remove an empty directory at path.
 * Fails if the directory contains any entries other than '.' and '..'.
 * Returns 1 on success, 0 on failure.
 */
int fat_remove_dir(const char *path) {
    fat_dir_entry_t entry;
    fat_dir_slot_t  slot;
    fat_dir_ref_t   dir;
    int is_empty = 1;
    u8 *sector;
    u8 *raw;

    if (!g_fs.mounted || !path) {
        return 0;
    }

    if (!fat_locate_path_entry(path, &entry, &slot)) {
        return 0;
    }
    if ((entry.attr & FAT_ATTR_DIRECTORY) == 0U) {
        return 0; /* not a directory */
    }
    if (entry.first_cluster < 2U) {
        return 0; /* root or cluster-0 — cannot delete */
    }

    /* Open as a dir_ref for the emptiness check */
    dir.fixed_root   = 0;
    dir.start_sector = 0;
    dir.sector_count = 0;
    dir.start_cluster = entry.first_cluster;

    fat_iter_dir(&dir, fat_is_not_empty_cb, &is_empty);
    if (!is_empty) {
        return 0; /* directory not empty */
    }

    /* Release the cluster chain */
    if (!fat_free_chain(entry.first_cluster)) {
        return 0;
    }

    /* Mark the directory entry as deleted */
    sector = stage2_disk_lba_ptr_rw(slot.lba);
    if (!sector) {
        return 0;
    }
    raw = sector + slot.offset;
    raw[0]  = 0xE5U;
    raw[20] = 0U; raw[21] = 0U;
    raw[26] = 0U; raw[27] = 0U;
    raw[28] = 0U; raw[29] = 0U; raw[30] = 0U; raw[31] = 0U;

    return 1;
}

/*
 * Update the attribute byte of an existing file or directory entry.
 * DIRECTORY and VOLUME_ID bits in attr are silently cleared to protect
 * the directory tree structure.
 * Returns 1 on success, 0 on failure.
 */
int fat_set_attr(const char *path, u8 attr) {
    fat_dir_entry_t entry;
    fat_dir_slot_t slot;
    u8 *sector;

    if (!g_fs.mounted || !path) {
        return 0;
    }
    if (!fat_locate_path_entry(path, &entry, &slot)) {
        return 0;
    }

    /* Preserve DIRECTORY and VOLUME_ID bits from the on-disk entry */
    attr = (u8)((attr & ~(FAT_ATTR_DIRECTORY | FAT_ATTR_VOLUME_ID)) |
                (entry.attr & (FAT_ATTR_DIRECTORY | FAT_ATTR_VOLUME_ID)));

    sector = stage2_disk_lba_ptr_rw(slot.lba);
    if (!sector) {
        return 0;
    }
    sector[slot.offset + 11U] = attr;
    return 1;
}

int fat_delete_file(const char *path) {
    fat_dir_entry_t entry;
    fat_dir_slot_t slot;
    u8 *sector;
    u8 *raw;

    if (!g_fs.mounted || !path) {
        return 0;
    }

    if (!fat_locate_path_entry(path, &entry, &slot)) {
        return 0;
    }
    if ((entry.attr & FAT_ATTR_DIRECTORY) != 0U) {
        return 0;
    }
    if ((entry.attr & FAT_ATTR_READ_ONLY) != 0U) {
        return 0;
    }

    if (!fat_free_chain(entry.first_cluster)) {
        return 0;
    }

    sector = stage2_disk_lba_ptr_rw(slot.lba);
    if (!sector) {
        return 0;
    }
    raw = sector + slot.offset;
    raw[0] = 0xE5U;
    raw[20] = 0U;
    raw[21] = 0U;
    raw[26] = 0U;
    raw[27] = 0U;
    raw[28] = 0U;
    raw[29] = 0U;
    raw[30] = 0U;
    raw[31] = 0U;
    return 1;
}
