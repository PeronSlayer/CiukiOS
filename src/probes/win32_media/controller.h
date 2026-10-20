/* Authored w32-media CD fixture, observed through the child's address space.
 * SPDX-License-Identifier: MIT */
#ifndef W32_MEDIA_CONTROLLER_H
#define W32_MEDIA_CONTROLLER_H
#include <stdint.h>
#define W32_MEDIA_RESULT_VA 0x6ffe0000u
#define W32_MEDIA_MAGIC 0x41444357u
#define W32_MEDIA_PATH "/System/Win32/Fixtures/media-cdaudio.exe"
struct w32_media_result {
    uint32_t magic, version, done, release;
    uint32_t errors, first_error, cdaudio, present, tracks, data_only, range_refused, tray;
    uint32_t reserved[24];
};
_Static_assert(sizeof(struct w32_media_result)==144,"w32-media result");
static inline int w32_media_valid(const struct w32_media_result *r)
{ return r->magic==W32_MEDIA_MAGIC && r->version==1 && r->done==1; }
static inline void w32_media_publish(const struct w32_media_result *r,uint32_t exit_code,
    void (*emit)(const char *,const char *,const char *,...))
{
    emit("w32-media","DATA","case=cdaudio errors=%u installed=%u present=%u tracks=%u data_only=%u range_refused=%u tray=%u first_error=%u exit_code=%u",
        r->errors,r->cdaudio,r->present,r->tracks,r->data_only,r->range_refused,r->tray,r->first_error,exit_code);
}
#endif
