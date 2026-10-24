/* SPDX-License-Identifier: MIT. Windows 3.1/9x byte-format .CLP interchange. */
#ifndef DU_CLIPFILE_H
#define DU_CLIPFILE_H
#include <stdint.h>
int du_clip_export(void **, uint32_t *);
int du_clip_import(const void *, uint32_t);
#endif
