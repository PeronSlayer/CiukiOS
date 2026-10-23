/* Staged synchronous display command executor. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DISPLAY_BATCH_H
#define CIUKI_DISPLAY_BATCH_H
#include <ciuki/swdib.h>
struct display_batch {
    const uint8_t *blob;
    struct ciuki_batch_header header;
    struct swdib_target targets[CIUKI_BATCH_MAX_TARGETS];
    struct ciuki_batch_reply reply;
    bool cancel, cursor_shape;
    int interrupt_error;
    bool (*interrupted)(void *);
    void *interrupted_arg;
    int (*authority)(void *);
    void (*preflight_pause)(void);
    bool noop[CIUKI_BATCH_MAX_RECORDS];
};
struct display_batch_ops {
    void *ctx;
    int (*begin)(void *, struct display_batch *);
    int (*native)(void *, const struct swdib_command *);
    int (*sync)(void *);
    bool (*recover)(void *);
    void (*end)(void *);
    void (*tile)(void *);
    bool (*cancelled)(void *);
    void (*count)(void *, unsigned, bool);
};
/* Header/structure checks before target resolution; then complete preflight.
 * Caller owns blob and resolved targets through terminal completion. */
int display_batch_header(const void *, uint32_t, struct ciuki_batch_header *);
int display_batch_validate(struct display_batch *);
int display_batch_execute(struct display_batch *, const struct display_batch_ops *);
bool display_batch_recover(struct display_batch *, const struct display_batch_ops *);
void display_batch_command(const struct display_batch *, unsigned, struct swdib_command *);
int fbdev_batch_execute(struct display_batch *);
bool fbdev_batch_recover(struct display_batch *);
void fbdev_batch_abort(struct display_batch *);
void fbdev_batch_recovery_abandon(struct display_batch *);
int fbdev_command_info(uint32_t, struct ciuki_command_info *);
/* The owned display fd names its complete, already-verified offscreen lease.
 * Snapshot only: never probes, maps, allocates or tests pixels. */
int fbdev_batch_video_target(uint32_t, struct swdib_target *);
#endif
