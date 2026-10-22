/* f9-01 Setup placeholder: \SYSTEM\RELEASE.JSON fields and the
 * "[setup] media ok" record (cd-setup-plan.md 3.3; directive f9-01 item 7).
 * Pure code: host-tested by scripts/test/cd_media_tests.py.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SETUP_INFO_H
#define CIUKI_SETUP_INFO_H
#include <stddef.h>

struct setup_release {
    char content_digest[65];     /* SHA-256 of the CD content listing (RELEASE.JSON excluded) */
    char disk_image_sha256[65];  /* the sibling canonical disk image */
    char version[48];
    unsigned long payload_files; /* files in the boot image's FAT volume */
};
/* 0, or -1 when a field is missing or malformed. */
int setup_release_parse(const char *json, size_t length, struct setup_release *out);
/* "[setup] media ok iso=<digest> image=<sha> payload=<n> mode=placeholder"
 * when counted == payload_files, else "[setup] media bad ...". Returns the
 * line length (newline included) or -1 when it does not fit. */
int setup_media_record(char *out, size_t size, const struct setup_release *, unsigned long counted);
#endif
