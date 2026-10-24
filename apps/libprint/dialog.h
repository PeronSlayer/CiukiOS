/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_PRINT_DIALOG_H
#define CIUKI_PRINT_DIALOG_H
#include "print.h"
#include "../libdeskui/job.h"
/* G28b: builds the formatted document for the printer and page the user chose; runs on the UI thread when OK is pressed.
 * driver is the printer's cp_driver. 0 or -errno; the arrays are owned by the controller afterwards (cp_doc_free). */
typedef int (*cp_prepare_fn)(void *ctx, const struct cp_page *page, unsigned driver, struct cp_doc *out);
/* One controller per client. The worker owns only the immutable snapshot. */
struct cp_ui {
    cp_prepare_fn prepare; void *prepare_ctx; struct cp_doc doc; int document, preset;
    struct cp_page page;
    struct cp_config *config;
    struct du_job job;
    const char *root;
    void *snapshot;
    size_t bytes;
    unsigned width, height, printer;
    int initialized, image, result, error, active;
    char title[CP_TITLE], target[CP_PATH];
    struct du_item printer_rows[CP_PRINTERS];
    unsigned printer_ids[CP_PRINTERS];
};
void cp_ui_setup(struct cp_ui *, struct du_ui *);
void cp_ui_text(struct cp_ui *, struct du_ui *, const char *, const char *, size_t);
void cp_ui_image(struct cp_ui *, struct du_ui *, const char *, const uint32_t *, unsigned, unsigned);
/* Print dialog for a formatted document; `initial` (may be NULL) is the paper and margins the document was set up with. */
void cp_ui_doc(struct cp_ui *, struct du_ui *, const char *title, const struct cp_page *initial, cp_prepare_fn, void *ctx);
/* Return nonzero when the controller consumed a dialog event. */
int cp_ui_event(struct cp_ui *, struct du_ui *, const struct du_event *);
void cp_ui_poll(struct cp_ui *, struct du_ui *);
void cp_ui_destroy(struct cp_ui *);
#endif
