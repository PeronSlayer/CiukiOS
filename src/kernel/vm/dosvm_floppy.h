/* Trusted kernel assignment API; guest code cannot acquire host media.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_FLOPPY_H
#define CIUKI_DOSVM_FLOPPY_H
#include "floppy_model.h"
#include <ciuki/dosvm.h>
struct dosvm_floppy_binding {
    struct fdc_lease physical;
    int image;
    uint32_t media_generation;
    bool assigned, file, writable;
};
struct dosvm_floppy {
    struct vfdc model;
    struct dosvm_floppy_binding binding[2];
    uint8_t status;
};
int dosvm_floppy_init(struct dosvm *);
int dosvm_floppy_close(struct dosvm *);
int dosvm_floppy_assign(struct dosvm_key, unsigned guest, unsigned physical);
int dosvm_floppy_image(struct dosvm_key, unsigned guest, const char *path, bool writable);
int dosvm_floppy_bios(struct dosvm *, struct v86_frame *);
uint16_t dosvm_floppy_equipment(const struct dosvm *);
#endif
