/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_HARDWARE_REPORT_H
#define CIUKI_HARDWARE_REPORT_H
#include <ciuki/abi.h>
#include <ciuki/firmware_abi.h>
#include <stddef.h>
#include <sys/types.h>
#define HR_TEXT_MAX (512u * 1024u)
#define HR_LOG_MAX (64u * 1024u)
#define HR_FULL_LOG_MAX (4u * 1024u * 1024u)
/* /System/BUILD.TXT is about 300 bytes (scripts/build_identity.py); more than
 * this is reported as oversized, never truncated and quoted. */
#define HR_BUILD_MAX 1024u
struct hr_source {
    void *ctx;
    const struct ciuki_devices_info *devices;
    const struct ciuki_hardware_info *hardware;
    const struct ciuki_display_info *display;
    const struct ciuki_accel_info *engine;
    const struct ciuki_net_state *network;
    int (*firmware)(void *, struct ciuki_firmware_page *);
    const char *boot_log, *latest_log;
    size_t boot_length, latest_length;
    int boot_error, latest_error;
    /* /System/BUILD.TXT bytes as read (not NUL terminated); build_error is 0,
     * or the negative errno of the read (-ENOENT missing, -EFBIG oversized). */
    const char *build_txt;
    size_t build_length;
    int build_error;
    int (*device_name)(void *, unsigned bus, unsigned vendor, unsigned device, char *, size_t);
};
extern const char hr_summary[];
extern const char hr_privacy[];
/* Exact preview and exported bytes are the same buffer. Negative errno on error. */
int hr_format(const struct hr_source *, char *, size_t, size_t *);
int hr_filter_log(const char *, size_t, char *, size_t, size_t *);
int hr_gzip(const char *, size_t, unsigned char **, size_t *);
/* Injectable actual file operations, also used by full/removed-medium tests. */
struct hr_io {
    void *ctx;
    int (*open)(void *, const char *);
    ssize_t (*write)(void *, int, const void *, size_t);
    int (*sync)(void *, int);
    int (*close)(void *, int);
    int (*rename)(void *, const char *, const char *);
    int (*remove)(void *, const char *);
    int (*present)(void *); /* validate selected drive/generation before publication */
};
int hr_save(const struct hr_io *, const char *root, const char *, size_t);
int hr_save_logs(const struct hr_io *, const char *root, const void *, size_t);
unsigned hr_default_drive(uint32_t writable, uint32_t usb, int rescue);
/* Headless text mode (--rescue --text-fd=N): the exact bytes of CIUKIREP.TXT
 * go to a sink in framed chunks (textframe.h); nothing is written to a file. */
struct hr_link_io {
    void *ctx;
    int (*send)(void *, const void *data, size_t bytes); /* one frame; 0 or negative errno */
};
/* BEGIN, DATA frames, END with the status and the checksum. */
int hr_link_send(const struct hr_link_io *, const char *text, size_t bytes, int status);
/* hr_format into buf (cap bytes) and hr_link_send. A collection error sends no
 * text and that error as the status; a report over HR_LINK_MAX is cut at a line
 * boundary and sent with status -EFBIG. *status is the status sent. Returns 0
 * once the transfer was sent, or the negative errno of the failed send. */
int hr_link_report(const struct hr_source *, char *buf, size_t cap, const struct hr_link_io *, int *status);
#endif
