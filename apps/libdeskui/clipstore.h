/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_CLIPSTORE_H
#define CIUKI_CLIPSTORE_H
#include "../desktop/protocol.h"
#include <stddef.h>
struct clip_item { uint32_t format, bytes; unsigned char *data; int delayed; };
struct clip_store {
    struct clip_item items[DESK_CLIP_FORMATS_MAX];
    unsigned count, bytes, names, dirty;
    uint32_t sequence, owner, token, opener, open_token, locale;
    char *registered[DESK_CLIP_NAMES_MAX];
};
void clip_init(struct clip_store *);
void clip_destroy(struct clip_store *);
void clip_disconnect(struct clip_store *, uint32_t client);
/* Desktop serializes these calls. GET allocates an independent copy. EAGAIN
 * on a delayed GET asks the transport to request owner rendering. */
int clip_request(struct clip_store *, uint32_t client,
                 const struct desk_clip_wire *, const void *, struct desk_clip_wire *, void **);
int clip_text_encode(const char *, size_t, void **, uint32_t *);
int clip_text_decode(const void *, uint32_t, char **);
int clip_dib_encode(const uint32_t *, unsigned stride, unsigned width, unsigned height, void **, uint32_t *);
int clip_dib_decode(const void *, uint32_t, uint32_t **, unsigned *, unsigned *);
#endif
