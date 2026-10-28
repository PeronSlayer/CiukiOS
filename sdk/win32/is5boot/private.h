/* Internal helper interface; not part of the Win32 SDK or syscall ABI.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef IS5_PRIVATE_H
#define IS5_PRIVATE_H
#include <stdint.h>
#define IS5_MEMBERS 16
#define IS5_NAME 64
#define IS5_PATH 778
struct is5_member {
    uint32_t offset, packed, expanded;
    char original[IS5_NAME], runtime[IS5_NAME];
};
struct is5_archive { uint32_t count; struct is5_member member[IS5_MEMBERS]; };
int is5_directory(const uint8_t *, uint32_t, struct is5_archive *);
int is5_dcl(const uint8_t *, uint32_t, uint8_t *, uint32_t);
int is5_arguments(char *, unsigned, const char *, const char *, const char *, const char *);
int is5_environment(char *, unsigned, const char *, unsigned);
int is5_cleanup_text(char *, unsigned, const char *, const char *, const char *);
int is5_join(char *, const char *, const char *);
int is5_ne(const uint8_t *, uint32_t);
int is5_string(const uint8_t *, uint32_t, unsigned, char *, unsigned);
const char *is5_tail(const char *);
#endif
