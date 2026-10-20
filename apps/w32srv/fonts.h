/* f5-10C2 native side of the PE-to-native font bridge.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32SRV_FONTS_H
#define W32SRV_FONTS_H
#include <w32/server.h>
#include <stddef.h>
#include <stdint.h>
/* Shared budget for the accounted open faces in the service: the 2 MiB
 * "fonts and rasteriser, shared" target of the Win32 contract minus 512 KiB
 * for what the accounting does not see: the linked libfont/FreeType text
 * (184,188 bytes of w32srv text, f5-10C2) and FreeType's transient
 * interpreter/snapshot peak above the accounted face (F2-63: 731,509-byte
 * peak against at most 541,784 accounted bytes for Liberation Sans). */
#define WF_BUDGET (2u * 1024u * 1024u - 512u * 1024u)
#define WF_FACES 32u
#define WF_NAME 128u /* longest face file name */
#define WF_DIR 256u  /* longest store directory */
struct wf_config {
    const char *system_dir, *install_dir; /* native directories, no trailing '/' */
    uint32_t budget;                      /* 0 selects WF_BUDGET */
};
struct wf_service;
struct wf_service *wf_create(const struct wf_config *);
void wf_destroy(struct wf_service *);
/* One validated request (wr_dispatch has checked the envelope). Writes the
 * reply data after the reply header and returns the Win32 error. */
uint32_t wf_call(void *, uint32_t pid, const struct w32r_request *, const char *path,
                 const void *data, struct w32r_reply *, uint8_t *out, size_t cap);
void wf_statistics(struct wf_service *, struct w32r_font_stats *);
#endif
