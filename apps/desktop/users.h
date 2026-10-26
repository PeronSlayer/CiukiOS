/* SPDX-License-Identifier: MIT
 * G32 internal application service, not an SDK or IPC ABI. */
#ifndef CIUKI_DESKTOP_USERS_H
#define CIUKI_DESKTOP_USERS_H
#include <stdint.h>
#include <stddef.h>
#define USERS_MAX 16u
#define USERS_NAME 32u
#define USERS_PASSWORD 64u
#define USERS_PATH 128u
#define USERS_LOGON 1u
#define USERS_PROFILES 2u
#define USERS_LOCK 4u
#define USERS_FLAGS 7u
#define USERS_ITERATIONS 10000u
enum { USERS_READ, USERS_CREATE, USERS_DELETE, USERS_CHANGE, USERS_POLICY, USERS_AUTH, USERS_DEFAULT };
struct users_request {
    unsigned operation, flags;
    char name[USERS_NAME], current[USERS_NAME];
    char old_password[USERS_PASSWORD], password[USERS_PASSWORD], confirm[USERS_PASSWORD];
};
struct users_view {
    unsigned count, flags;
    uint32_t ids[USERS_MAX];
    char names[USERS_MAX][USERS_NAME];
    uint32_t authenticated;
};
struct users_record {
    uint32_t id;
    char name[USERS_NAME];
    uint8_t salt[16], verifier[32];
};
struct users_db {
    unsigned count, flags;
    uint32_t next_id;
    struct users_record records[USERS_MAX];
};
struct user_profile {
    char name[USERS_NAME];
    char root[USERS_PATH], documents[USERS_PATH], desktop[USERS_PATH];
    char programs[USERS_PATH], settings[USERS_PATH], temporary[USERS_PATH];
};
int users_boot_identity(char out[64]);
int users_name_valid(const char *);
void users_erase(void *, size_t);
void users_derive(const char *, const uint8_t salt[16], unsigned iterations, uint8_t out[32]);
int users_apply(struct users_db *, const struct users_request *, const uint8_t salt[16], struct users_view *);
void users_snapshot(const struct users_db *, struct users_view *);
size_t users_encode(const struct users_db *, unsigned char *, size_t);
int users_decode(struct users_db *, const unsigned char *, size_t);
void users_profile(struct user_profile *, const char *name, uint32_t id, unsigned flags);
/* root is a trusted filesystem prefix (empty on CiukiOS, temporary dir in tests).
 * All disk I/O and derivation belong on a worker. Returns 0 or negative errno. */
int users_execute(const char *root, const struct users_request *, struct users_view *);
#endif
