/* SPDX-License-Identifier: MIT
 * In-memory FAT-like tree for Files host tests, with injected failures. */
#ifndef CIUKI_TEST_FAKE_VFS_H
#define CIUKI_TEST_FAKE_VFS_H
#include "../../../apps/files/model.h"
#define FAKE_NODES 160
#define FAKE_DATA 40000
struct fake_node { char path[FM_PATH]; struct fm_meta meta; unsigned char data[FAKE_DATA]; int live; };
extern struct fake_node fake_nodes[FAKE_NODES];
extern int fake_countdown, fake_operations, fake_metadata_count, fake_short_io;
extern struct fm_vfs fake_vfs;
void fake_reset(void);
int fake_lookup(const char *);
int fake_create(const char *, enum fm_type);
int fake_file(const char *, unsigned size, unsigned char seed);
void fake_assert_clean(void);
void fake_image_tree(void);   /* mirrors the canonical image's / , /Programs and /system */
#endif
