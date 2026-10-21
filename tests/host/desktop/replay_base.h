/* SPDX-License-Identifier: MIT
 * Header order for the app-case replay (host build of guest programs): the
 * guest ABI first, then the host headers, so that the host's definitions win
 * (MAP_FAILED, SIG_IGN, ...) while equal-valued ABI macros stay. The ABI's
 * plain macros for names the host declares as enumerators are dropped first,
 * as tests/host/desktop/desktop_gate_test.c does. */
#ifndef CIUKI_REPLAY_BASE_H
#define CIUKI_REPLAY_BASE_H
#include <ciuki/abi.h>
#undef DT_UNKNOWN
#undef DT_CHR
#undef DT_DIR
#undef DT_REG
#undef ILL_ILLOPC
#undef FPE_INTDIV
#undef SEGV_MAPERR
#undef SEGV_ACCERR
#undef BUS_ADRALN
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#undef SOCK_STREAM
#undef SOCK_DGRAM
#undef SOCK_NONBLOCK
#undef SOCK_CLOEXEC
#undef SHUT_RD
#undef SHUT_WR
#undef SHUT_RDWR
#undef MSG_PEEK
#undef MSG_DONTWAIT
#undef MSG_NOSIGNAL
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif
