/* SPDX-License-Identifier: MIT
 * Headless transfer of the hardware report text (see textframe.h). */
#include "report.h"
#include "textframe.h"
#include <errno.h>
#include <string.h>

int hr_link_send(const struct hr_link_io *io, const char *text, size_t bytes, int status)
{
    if (!io || !io->send || (bytes && !text) || bytes > HR_LINK_MAX) return -EINVAL;
    uint8_t frame[HR_FRAME_MAX];
    int e = io->send(io->ctx, frame, hr_frame_begin(frame, (uint32_t)bytes));
    for (size_t at = 0; !e && at < bytes;) {
        size_t n = bytes - at > HR_FRAME_CHUNK ? HR_FRAME_CHUNK : bytes - at;
        e = io->send(io->ctx, frame, hr_frame_data(frame, (uint32_t)at, text + at, n));
        at += n;
    }
    if (!e) e = io->send(io->ctx, frame, hr_frame_end(frame, (int32_t)status, (uint32_t)bytes, hr_crc32(0, (const uint8_t *)text, bytes)));
    return e;
}

int hr_link_report(const struct hr_source *s, char *buf, size_t cap, const struct hr_link_io *io, int *status)
{
    size_t n = 0;
    int e = hr_format(s, buf, cap, &n);
    if (e) { n = 0; *status = e; }
    else if (n > HR_LINK_MAX) {
        n = HR_LINK_MAX;
        while (n && buf[n - 1] != '\n') n--;
        *status = -EFBIG;
    } else *status = 0;
    return hr_link_send(io, buf, n, *status);
}
