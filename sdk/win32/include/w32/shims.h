/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32_SHIMS_H
#define W32_SHIMS_H
#include <w32/ldr.h>
#include <ciuki/w32_diagnostics.h>
#define W32_SHIM_TEXT_MAX 65536u
#define W32_SHIM_LINE_MAX 1024u
struct w32_shim_identity {
    const char *exe;
    uint32_t timestamp, image_size, file_size;
    char sha256[65];
};
struct w32_shim_match {
    struct ciuki_w32_compat settings;
    uint32_t rank, conflicts;
};
extern const char *const w32_shim_names[12];
extern const uint32_t w32_shim_versions[12];
extern struct ciuki_w32_compat w32_shims;
const struct ciuki_w32_compat *w32_shim_current(void);
int w32_shim_list(const char *, struct ciuki_w32_compat *);
int w32_shim_parse(const void *, uint32_t, const struct w32_shim_identity *, struct w32_shim_match *);
int w32_shim_format(const struct ciuki_w32_compat *, char *, uint32_t);
int w32_shim_native(const char *);
int w32_shim_w_stub(const char *);
int w32_shim_start(struct w32_loader *, const char *);
void w32_shim_version(struct ciuki_w32_identity *);
uint32_t w32_shim_stub(volatile uint32_t *, const char *, const char *, uint32_t);
void w32_report_exception(struct w32_exception *, struct w32_context *);
void w32_report_refusal(uint32_t, const char *, uint32_t);
#endif
