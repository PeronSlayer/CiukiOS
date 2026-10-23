/* SPDX-License-Identifier: MIT
 * Private platform boundary; NOT a public SDK or kernel ABI. The versioned
 * setup_txn.h remains the sole application interface. Negative errno results.
 * Preflight is read-only and precedes the destination lease and every write.
 */
#ifndef LIBSETUP_INTERNAL_H
#define LIBSETUP_INTERNAL_H
#include "include/setup_txn.h"
#include <libdisk.h>
#include <stddef.h>
#define ST_BUFFER_BYTES 65536u
#define ST_PATH_BYTES 260u
#define ST_MANIFEST "CDMANIFE.JSN"
#define ST_RELEASE "system/RELEASE.JSON"
struct st_info { uint64_t size; uint8_t attr; char name[260], alias[40]; };
struct st_platform {
    void *ctx;
    int (*preflight)(void *, const struct setup_answers *);
    int (*open)(void *, char, const char *, bool);
    int (*read)(void *, int, void *, unsigned);
    int (*write)(void *, int, const void *, unsigned);
    int (*seek)(void *, int, uint64_t);
    int (*sync)(void *, int);
    int (*close)(void *, int);
    int (*info)(void *, char, const char *, struct st_info *);
    int (*mkdir)(void *, char, const char *);
    int (*attrib)(void *, char, const char *, uint8_t);
    /* Read-only VFS chain query. Returns the absolute LBA of a file's 4 KiB
     * cluster, or -ENOENT after the chain. Never reads file data via VFS. */
    int (*cluster)(void *, char, const char *, uint32_t, uint64_t *);
    /* Reuse Control Panel/configuration writers, including a durable RTC
     * write. No launch, network connection, or personality activation. */
    int (*configure)(void *, char, const struct setup_answers *);
    /* open_tray=false: state only. Bind to the boot CD, never another drive.
     * Re-opened paths must bind the replacement C:/D: media generation. */
    int (*media)(void *, bool open_tray, bool *removed, uint32_t *generation);
    uint64_t (*milliseconds)(void *);
    void (*sleep_ms)(void *, unsigned);
    int (*restart)(void *);
};
void st_set_platform(const struct st_platform *); /* host injection; NULL native */
const struct st_platform *st_native_platform(void);
void st_native_cleanup(void);
int setup_restart(enum setup_restart);
struct st_record {
    char path[260]; uint64_t size; uint8_t sha[32], attr;
    uint32_t component; char drive; bool directory, has_attr;
};
struct st_manifest {
    char image_sha[65], loader_sha[65], boot_image[260], boot_cfg[128];
    char boot_template[128], boot_template_sha[65];
    uint64_t boot_image_bytes;
    unsigned files, directories;
};
typedef int (*st_record_fn)(void *, const struct st_record *);
typedef int (*st_source_retry_fn)(void *, int, const char *, unsigned *);
int st_manifest_read(const struct st_platform *, struct st_manifest *, st_record_fn, void *, st_source_retry_fn);
int st_hex_decode(const char *, uint8_t[32]);
void st_hex_encode(const uint8_t[32], char[65]);
int st_path_valid(const char *);
#endif
