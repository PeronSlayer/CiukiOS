/* Storage maintenance is outside the native files/time observation backend.
 * The production implementations are exercised by the filesystem harnesses.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/storage.h>

int storage_defrag(struct storage *s, struct ciuki_media_defrag *r, bool step)
{ (void)s; (void)r; (void)step; return -FS_EOPNOTSUPP; }

int storage_convert(struct storage *s, struct ciuki_media_convert *r, bool execute)
{ (void)s; (void)r; (void)execute; return -FS_EOPNOTSUPP; }

int storage_format_removable(struct storage *s, struct ciuki_media_format *r, bool execute)
{ (void)s; (void)r; (void)execute; return -FS_EOPNOTSUPP; }

int storage_floppy_copy(struct storage *s, struct ciuki_floppy_image *r,
                       storage_floppy_copy_fn copy, void *arg, bool write)
{ (void)s; (void)r; (void)copy; (void)arg; (void)write; return -FS_EOPNOTSUPP; }
