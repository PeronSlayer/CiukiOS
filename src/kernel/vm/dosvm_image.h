/* fx-4b background verification of C:\SYSTEM\DOS\UNIFIED.IMG and its
 * persistent verified stamp (dosvm_image.c). SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_IMAGE_H
#define CIUKI_DOSVM_IMAGE_H
#include <ciuki/storage.h>
#define DOSVM_IMAGE_STAMP "C:\\SYSTEM\\DOS\\UNIFIED.VFY"
/* Session digest cache (dosvm_disk.c), shared with the verifier. */
bool dosvm_game_digest_find(const struct vfs_file_version *, char out[65]);
void dosvm_game_digest_publish(struct vfs_table *, int handle, const struct vfs_file_version *, const char sha256[65]);
void dosvm_image_desktop_presented(void);
void dosvm_image_hold(void);
void dosvm_image_release(void);
int dosvm_image_lookup(struct vfs_table *, const struct vfs_file_version *, char sha[65],
                       const char **source, bool (*cancelled)(void *), void *arg);
void dosvm_image_publish(struct vfs_table *, const struct vfs_file_version *, const char sha[65]);
void dosvm_image_query(uint32_t *state, uint32_t *progress, uint32_t *error);
/* Host harnesses: no task; the test drives the steps. */
bool dosvm_image_step(void);
void dosvm_image_test_mode(void);
#endif
