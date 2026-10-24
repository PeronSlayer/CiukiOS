/* SPDX-License-Identifier: MIT
 * CiukNote document logic: file boundary, line endings, .LOG, time stamps,
 * and paths. No UI and no direct system calls, so that
 * host tests run the production code against a fake file system. */
#ifndef CIUKI_CIUKNOTE_DOC_H
#define CIUKI_CIUKNOTE_DOC_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define CN_LIMIT 61440u     /* text limit in bytes (the 0.8 CiukNote limit); line breaks count as one byte */
#define CN_LIMIT_TEXT "61,440"
#define CN_PATH 1040        /* PATH_MAX of the POSIX subset */

struct cn_stat { int dir; uint64_t size; int64_t mtime; };
typedef int (*cn_emit)(const char *name, const struct cn_stat *, void *);
/* Every function returns 0 or -errno. */
struct cn_fs {
    void *ctx;
    int (*stat)(void *, const char *path, struct cn_stat *);
    int (*list)(void *, const char *dir, cn_emit, void *); /* "." and ".." are not listed */
    int (*load)(void *, const char *path, char *buf, size_t cap, size_t *len); /* -EFBIG when larger than cap */
    int (*save)(void *, const char *path, const char *data, size_t len);       /* replaces the file as a whole */
};

/* The POSIX implementation (vfs.c): open/read/write on the process's own namespace. */
const struct cn_fs *cn_native_fs(void);

/* Text file <-> buffer. A file's CRLF pairs become "\n"; crlf records the style of its
 * first line break (left unchanged when there is none). -EFBIG: more than cap bytes. */
int cn_decode(const char *raw, size_t n, char *out, size_t cap, size_t *len, int *crlf);
size_t cn_encoded_size(const char *text, size_t n, int crlf);
size_t cn_encode(const char *text, size_t n, int crlf, char *out); /* out holds cn_encoded_size() bytes */

int cn_is_log(const char *text, size_t n); /* the first line is exactly ".LOG" */
void cn_timestamp(char *out, size_t cap, const struct tm *tm); /* "9:41 AM 10/11/2026" */

const char *cn_basename(const char *path);
int cn_has_extension(const char *name);                /* a final ".ext" that is not the whole name */
int cn_match(const char *pattern, const char *name);   /* case-insensitive * and ? glob */
int cn_has_wildcard(const char *name);
int cn_is_text_name(const char *name);                 /* *.txt, ASCII case-insensitive */
int cn_resolve(char *out, const char *dir, const char *input); /* absolute, normalised; input may be relative */
int cn_join(char *out, const char *dir, const char *name);
#endif
