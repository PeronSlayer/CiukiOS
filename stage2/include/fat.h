#ifndef STAGE2_FAT_H
#define STAGE2_FAT_H

#include "types.h"

#define FAT_ATTR_READ_ONLY   0x01
#define FAT_ATTR_HIDDEN      0x02
#define FAT_ATTR_SYSTEM      0x04
#define FAT_ATTR_VOLUME_ID   0x08
#define FAT_ATTR_DIRECTORY   0x10
#define FAT_ATTR_ARCHIVE     0x20
#define FAT_ATTR_LONG_NAME   0x0F

typedef struct fat_dir_entry {
    char name[13]; /* 8.3 uppercase */
    u8 attr;
    u32 first_cluster;
    u32 size;
} fat_dir_entry_t;

typedef struct fat_mount_info {
    u32 fat_type;
    u32 bytes_per_sector;
    u32 sectors_per_cluster;
    u32 total_clusters;
    u32 root_cluster;
    u32 fsinfo_sector;
    u32 fsinfo_valid;
    u32 next_free_hint;
    u32 free_count_known;
    u32 free_cluster_count;
} fat_mount_info_t;

typedef int (*fat_dir_enum_cb_t)(const fat_dir_entry_t *entry, void *ctx);

int fat_init(void);
int fat_ready(void);
int fat_get_mount_info(fat_mount_info_t *out);
int fat_list_dir(const char *path, fat_dir_enum_cb_t cb, void *ctx);
int fat_find_file(const char *path, fat_dir_entry_t *out);
int fat_read_file(const char *path, void *out, u32 out_capacity, u32 *out_size);
int fat_write_file(const char *path, const void *data, u32 size);
int fat_set_attr(const char *path, u8 attr);
int fat_rename_entry(const char *old_path, const char *new_name);
int fat_create_dir(const char *path);
int fat_remove_dir(const char *path);
int fat_delete_file(const char *path);

#endif
