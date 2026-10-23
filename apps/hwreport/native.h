/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_HWREPORT_NATIVE_H
#define CIUKI_HWREPORT_NATIVE_H
#include "report.h"
struct hr_native {
    struct hr_source source;
    struct ciuki_devices_info devices;
    struct ciuki_hardware_info hardware;
    struct ciuki_display_info display;
    struct ciuki_accel_info engine;
    struct ciuki_net_state network;
    struct ciuki_media_state media;
    char *pci_names, *usb_names;
    char boot[HR_LOG_MAX], latest[HR_LOG_MAX];
    char build[HR_BUILD_MAX + 1];
    size_t build_length;
    uint32_t writable, usb;
    unsigned selected;
    int rescue;
    char *full_logs;
    size_t full_length, archive_length;
    unsigned char *archive;
    int archive_result;
};
void hr_native_collect(struct hr_native *);
void hr_native_free(struct hr_native *);
int hr_native_save(struct hr_native *, const char *, size_t);
int hr_native_prepare_logs(struct hr_native *);
#endif
