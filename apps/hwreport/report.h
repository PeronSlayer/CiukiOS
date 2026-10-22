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
#endif
