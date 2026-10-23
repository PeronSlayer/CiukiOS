/*
 * Copyright 1993 by Jon Block <block@frc.com>
 * Modified by Mike Hollick <hollick@graphics.cis.upenn.edu>
 * Modified 1994 by Régis Cridlig <cridlig@dmi.ens.fr>
 *
 * Major Contributors to XFree86 3.2
 *   Modified 1995/6 by Nozomi Ytow
 *   Modified 1996 by Egbert Eich <eich@xfree86.org>
 *   Modified 1996 by David Bateman <dbateman@club-internet.fr>
 *   Modified 1996 by Xavier Ducoin <xavier@rd.lectra.fr>
 *
 * Contributors to XFree86 3.2
 *   Modified 1995/6 by Ken Raeburn <raeburn@raeburn.org>
 *   Modified 1996 by Shigehiro Nomura <nomura@sm.sony.co.jp>
 *   Modified 1996 by Marc de Courville <marc@courville.org>
 *   Modified 1996 by Adam Sulmicki <adam@cfar.umd.edu>
 *   Modified 1996 by Jens Maurer <jmaurer@cck.uni-kl.de>
 *
 * Large parts rewritten for XFree86 4.0
 *   Modified 1998 by David Bateman <dbateman@club-internet.fr>
 *   Modified 1998 by Egbert Eich <eich@xfree86.org>
 *   Modified 1998 by Nozomi Ytow
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the authors not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  The authors makes no representations
 * about the suitability of this software for any purpose.  It is provided
 * "as is" without express or implied warranty.
 *
 * THE AUTHORS DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
/* Origin: X.Org xf86-video-chips src/ct_driver.c (register offsets),
 * src/ct_BltHiQV.h (constants; no original licence header),
 * commit 9187d76bd4f8d500fc3403b6d4a7bef55508bf2b.
 * Private CiukiOS glue: SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_HIQV_PRIVATE_H
#define CIUKI_HIQV_PRIVATE_H
#include <ciuki/kernel.h>
#include <ciuki/display_driver.h>

#define HIQV_MMIO_OFFSET 0x400000u
#define HIQV_SCRATCH_BYTES (2u * PAGE_SIZE)
#define HIQV_PATTERN_OFFSET PAGE_SIZE
#define HIQV_UPLOAD_OFFSET (PAGE_SIZE + 8u)
#define HIQV_WAIT_US 20000u
#define HIQV_WAIT_POLLS 4096u

enum hiqv_br {
    HIQV_PITCH = 0x00, HIQV_BG = 0x04, HIQV_FG = 0x08,
    HIQV_MONO = 0x0c, HIQV_ROP = 0x10, HIQV_PATTERN = 0x14,
    HIQV_SRC = 0x18, HIQV_DST = 0x1c, HIQV_GO = 0x20
};
struct hiqv_io {
    void *ctx;
    uint8_t (*read_xr)(void *, uint8_t);
    void (*write_xr)(void *, uint8_t, uint8_t);
    void (*write_br)(void *, uint32_t, uint32_t);
    void (*palette)(void *, const uint8_t[6]);
    uint64_t (*now_us)(void *);
    int (*delay_us)(void *, uint32_t);
    void (*drain)(void *);
    bool (*valid)(void *);
};
struct hiqv_engine {
    struct hiqv_io io;
    struct accel_ops ops;
    struct fb_device fb;
    uint8_t *scratch;
    uint32_t scratch_offset;
    uint8_t saved20;
    bool active, quarantined, cpu_safe, cursor_hidden, cursor_loaded;
    bool configured, recovery_tried, stopped;
    const char *reason;
    uint64_t operations, waits, timeouts;
    int (*pause)(void *);
    void *pause_arg;
};
bool hiqv_mode_supported(const struct fb_device *);
int hiqv_engine_open(struct hiqv_engine *, const struct hiqv_io *,
                     const struct fb_device *, uint8_t *, uint32_t);
int hiqv_engine_activate(struct hiqv_engine *);
int hiqv_engine_check(struct hiqv_engine *);
void hiqv_engine_quarantine(struct hiqv_engine *, const char *);
int hiqv_engine_recover(struct hiqv_engine *);
int hiqv_cursor_check(const struct accel_ops *, uint64_t, bool (*)(void *), void *);
extern const struct display_driver chipshiqv_driver;
#endif
