/* Private REGISTRY service schema; not a public SDK ABI. SPDX-License-Identifier: MIT */
#ifndef CIUKI_PRIVATE_REGISTRY_PROTOCOL_H
#define CIUKI_PRIVATE_REGISTRY_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#define RB_BODY_MAX 8192u
#define RB_PAGE 8128u
#define RB_ROOT_MAX (256u*1024u)
#define RB_LINK_MAX 8u
enum { RB_NATIVE_READ=1, RB_NATIVE_PAGE, RB_NATIVE_BEGIN, RB_NATIVE_PART,
       RB_NATIVE_COMMIT, RB_NATIVE_ABORT, RB_MACHINE_ID, RB_WIN32=16, RB_WIN32_BEGIN,
       RB_WIN32_PART, RB_WIN32_COMMIT, RB_WIN32_READ };
enum { RB_SYSTEM=1, RB_PROGRAMS, RB_USERS };
/* sec-sysvc2: Registry Checker in the SYSTEM broker (apps/registry/checker.c,
 * apps/sysrecovery/regcheck.c). Requests: 32-byte header (u32 version=1, the
 * rest zero) and a tail: BACKUP char comment[40], RESTORE char name[16].
 * Replies: 32-byte header (u32 version=1, u32 word) and a tail. LIST (word:
 * count, then count 80-byte records), BACKUP (word 1, record), RESTORE (the
 * next start restores it), FIX (word: roots restored, then from[16]): the
 * administrators. DAILY (word: 1 made, then the record): every session, a
 * SYSTEM job. STATUS (word: damaged roots, +8 restored at this start, then
 * from[16], applied[16], pending[16]): every session. */
enum { RB_CHECKER_LIST=32, RB_CHECKER_BACKUP, RB_CHECKER_RESTORE, RB_CHECKER_FIX,
       RB_CHECKER_DAILY, RB_CHECKER_STATUS };
/* Native bodies: 32-byte LE header (version=1, root, offset, length,
 * expected generation u64, reserved u64=0), then <=8128 bytes. Replies
 * have the same header, generation/total length and optional page bytes.
 * Offsets are contiguous on upload; pages are generation checked on read.
 * One staged root per connection, dropped on disconnect/revocation. */
uint32_t rb_get32(const void *);
uint64_t rb_get64(const void *);
void rb_put32(void *,uint32_t);
void rb_put64(void *,uint64_t);
#endif
