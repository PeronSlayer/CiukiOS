/* sec-user-06: /Programs/sectest, the runtime security probe of the
 * acceptance (tests/suites/acc-t23.json) and security (sec-users.json)
 * suites. It runs as whoever starts it (from Run, in that user's session),
 * attempts each operation through the production entry point (kernel VFS,
 * SYSTEM registry broker, call-99 SYSTEM services, raw disk node) and writes
 * one record per check on its console, which the kernel copies to the serial
 * port as "[console] sectest ..." (proc/devnodes.c device_write). It never
 * elevates, never writes outside what the check itself creates and removes,
 * and prints no secret. The model is host-tested
 * (tests/host/security/test_runner_contract.py); sectest_main.c binds it to
 * the real calls. Expectations: docs/design/user-security-contract.md,
 * section 10.1 (SU-04/05/06) and the lead decisions M-Q7, K-Q1, K-Q4.
 * SPDX-License-Identifier: MIT */
#ifndef SECTEST_MODEL_H
#define SECTEST_MODEL_H
#include <stddef.h>
#include <stdint.h>

enum st_role { ST_SYSTEM, ST_ADMIN, ST_STANDARD, ST_GUEST, ST_UNKNOWN, ST_ROLES };
enum st_check {
    ST_ENVIRONMENT, ST_OWN_PROFILE, ST_OTHER_PROFILE, ST_PROGRAMS, ST_ROOT_FOLDER, ST_FOREIGN_FOLDER,
    ST_INSTALLER_STORE, ST_INSTALLER_SHORTCUTS, ST_MACHINE_REGISTRY, ST_MACHINE_SETTING, ST_RAW_DISK,
    ST_ACCOUNT_STORE, ST_BACKUPS, ST_CHECKS
};
/* Expectation of one check for one role. */
enum st_expect { ST_DENY = 0, ST_ALLOW = 1, ST_NOT_ATTEMPTED = 2 };

#define ST_GROUPS_MAX 8u
#define ST_LINE_MAX 200u
#define ST_FOLDER_MAX 31u

struct st_identity {
    uint32_t uid, gid, session, count;
    uint32_t groups[ST_GROUPS_MAX];
};
/* Every operation returns 0 when it was carried out (and undone, where the
 * check created something) or a negative errno. A check that cannot be tried
 * on this machine returns ST_ABSENT (-ENOENT/-ENODEV/-ENXIO are reported as
 * the reason, never as a refusal). */
struct st_ops {
    void *ctx;
    int (*identity)(void *, struct st_identity *);
    /* The session's committed profile root (USERPROFILE) and its leaf
     * (u%08x) in leaf; 0 when TMPDIR and HOME lie inside it, -EINVAL when
     * one does not, -ENOENT without a profile (sec-user-05 A1-10). */
    int (*environment)(void *, char *leaf, size_t);
    int (*own_profile)(void *);
    /* Open another account's profile root for listing; -ENOENT: there is none. */
    int (*other_profile)(void *);
    int (*programs)(void *);
    /* mkdir /<folder>, a file in it, then remove both unless keep. */
    int (*root_folder)(void *, const char *folder, int keep);
    /* A file in an existing root folder another account created (M-Q7:
     * the creator and administrators write, other users read). */
    int (*foreign_folder)(void *, const char *folder);
    /* A DLL beside the protected installer and a file in its presentation
     * folder (A1-5: /Programs/Installers, /Documents/Desktop/...). */
    int (*installer_store)(void *);
    int (*installer_shortcuts)(void *);
    int (*machine_registry)(void *);
    int (*machine_setting)(void *);
    int (*raw_disk)(void *);
    int (*account_store)(void *);
    int (*backups)(void *);
    /* SC_SCREENSAVE through the screen policy (G10), as any program of the
     * session may ask (Windows WM_SYSCOMMAND SC_SCREENSAVE). */
    int (*saver)(void *);
    /* One whole record, without its newline. */
    void (*line)(void *, const char *, size_t);
};
struct st_options {
    int expect;            /* enum st_role, or -1: report the role only */
    const char *folder;    /* root folder name; NULL: GAME (SECTEST for Guest) */
    const char *foreign;   /* another account's root folder: foreign-folder runs */
    int keep;              /* keep the root folder (a later account checks it) */
    unsigned mask;         /* checks to run (1u << enum st_check); 0: all */
};
struct st_summary { unsigned checks, passed, failed, skipped; int role, system; };

const char *st_role_name(int role);
int st_role_parse(const char *name);              /* -1 unknown */
int st_role_of(const struct st_identity *);
const char *st_check_name(enum st_check);
int st_check_parse(const char *name);             /* -1 unknown */
enum st_expect st_expect(int role, enum st_check);
/* Refusals the policy produces: EACCES, EPERM and EROFS. */
int st_denied(int result);
/* Parses argv (after the program name); 0, or -1 with a usage record. */
int st_options_parse(int argc, char **argv, struct st_options *, int *saver);
/* The whole run: begin record, one record per check, end record. Returns
 * 0 when every attempted check met its expectation. */
int st_run(const struct st_ops *, const struct st_options *, struct st_summary *);
int st_saver(const struct st_ops *);
#endif
