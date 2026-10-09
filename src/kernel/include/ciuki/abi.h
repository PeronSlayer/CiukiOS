/* CiukiOS native ABI v1; authority: docs/design/execution-abi.md (F2),
 * docs/design/posix-subset.md. Wire addresses are uint32_t, never C pointers.
 * Research (2026-10-10):
 * https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/asm-generic/errno-base.h
 * https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/asm-generic/errno.h
 * https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/kbd.c
 * https://gcc.gnu.org/onlinedocs/gcc/Structure-Layout-Pragmas.html
 * Decision: scoped pack(4) preserves the contracted i386 alignment even in
 * host checks; no packed(1), host POSIX types or host pointer widths.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef CIUKI_ABI_H
#define CIUKI_ABI_H

#include <stdint.h>

#define CIUKI_ABI_VERSION 1
#define __CIUKI_ABI_VERSION__ CIUKI_ABI_VERSION
#define __CIUKIOS__ 1

/* Public scalar types; the SDK maps POSIX names onto these. */
typedef uint32_t ciuki_size_t;
typedef uint32_t ciuki_uintptr_t;
typedef int32_t ciuki_ssize_t;
typedef int32_t ciuki_ptrdiff_t;
typedef int32_t ciuki_pid_t;
typedef int64_t ciuki_off_t;
typedef int64_t ciuki_time_t;
typedef int64_t ciuki_clock_t;
typedef uint64_t ciuki_ino_t;
typedef uint32_t ciuki_mode_t;
typedef uint32_t ciuki_nlink_t;
typedef uint32_t ciuki_uid_t;
typedef uint32_t ciuki_gid_t;
typedef uint32_t ciuki_dev_t;
typedef int32_t ciuki_blksize_t;
typedef int64_t ciuki_blkcnt_t;
typedef uint32_t ciuki_pthread_t;
typedef uint64_t ciuki_sigset_t;

/* Contract constants use their published names. No host headers required. */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define ETXTBSY 26
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define EDEADLK 35
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define EOVERFLOW 75
#define EILSEQ 84
#define EMSGSIZE 90
#define EOPNOTSUPP 95
#define ETIMEDOUT 110
#define ECANCELED 125
#define EWOULDBLOCK EAGAIN
#define ENOTSUP EOPNOTSUPP
#define CIUKI_EPERM EPERM
#define CIUKI_ENOENT ENOENT
#define CIUKI_ESRCH ESRCH
#define CIUKI_EINTR EINTR
#define CIUKI_EIO EIO
#define CIUKI_ENXIO ENXIO
#define CIUKI_E2BIG E2BIG
#define CIUKI_ENOEXEC ENOEXEC
#define CIUKI_EBADF EBADF
#define CIUKI_ECHILD ECHILD
#define CIUKI_EAGAIN EAGAIN
#define CIUKI_ENOMEM ENOMEM
#define CIUKI_EACCES EACCES
#define CIUKI_EFAULT EFAULT
#define CIUKI_EBUSY EBUSY
#define CIUKI_EEXIST EEXIST
#define CIUKI_EXDEV EXDEV
#define CIUKI_ENODEV ENODEV
#define CIUKI_ENOTDIR ENOTDIR
#define CIUKI_EISDIR EISDIR
#define CIUKI_EINVAL EINVAL
#define CIUKI_ENFILE ENFILE
#define CIUKI_EMFILE EMFILE
#define CIUKI_ENOTTY ENOTTY
#define CIUKI_ETXTBSY ETXTBSY
#define CIUKI_EFBIG EFBIG
#define CIUKI_ENOSPC ENOSPC
#define CIUKI_ESPIPE ESPIPE
#define CIUKI_EROFS EROFS
#define CIUKI_EPIPE EPIPE
#define CIUKI_EDOM EDOM
#define CIUKI_ERANGE ERANGE
#define CIUKI_EDEADLK EDEADLK
#define CIUKI_ENAMETOOLONG ENAMETOOLONG
#define CIUKI_ENOSYS ENOSYS
#define CIUKI_ENOTEMPTY ENOTEMPTY
#define CIUKI_EOVERFLOW EOVERFLOW
#define CIUKI_EILSEQ EILSEQ
#define CIUKI_EMSGSIZE EMSGSIZE
#define CIUKI_EOPNOTSUPP EOPNOTSUPP
#define CIUKI_ETIMEDOUT ETIMEDOUT
#define CIUKI_ECANCELED ECANCELED
#define CIUKI_EWOULDBLOCK EWOULDBLOCK
#define CIUKI_ENOTSUP ENOTSUP
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 3
#define O_CREAT 0x40
#define O_EXCL 0x80
#define O_TRUNC 0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC 0x80000
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED 0xffffffff
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define WNOHANG 1
#define CIUKI_SPAWN_NEW_GROUP 1
#define CIUKI_THREAD_DETACHED 1
#define _SC_PAGESIZE 1
#define _SC_OPEN_MAX 2
#define _SC_ARG_MAX 3
#define _SC_THREAD_KEYS_MAX 4
#define _SC_THREAD_STACK_MIN 5
#define CLOCKS_PER_SEC 1000000
#define CIUKI_SC_PAGESIZE_VALUE 4096
#define CIUKI_SC_OPEN_MAX_VALUE 128
#define CIUKI_SC_ARG_MAX_VALUE 65536
#define CIUKI_SC_THREAD_KEYS_MAX_VALUE 64
#define CIUKI_SC_THREAD_STACK_MIN_VALUE 65536
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define SIG_DFL 0
#define SIG_IGN 1
#define SA_SIGINFO 4
#define SIGINT 2
#define SIGILL 4
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 17
#define DT_DIR 4
#define DT_REG 8
#define DT_CHR 2
#define DT_UNKNOWN 0
#define AT_PAGESZ 6
#define AT_ENTRY 9
#define AT_CIUKI_TLS 0x60000001
#define AT_CIUKI_TLS_SIZE 0x60000002
#define AT_CIUKI_ABI 0x60000003
#define AT_NULL 0
#define CIUKI_PATH_MAX 1040
#define CIUKI_NAME_MAX 765
#define CIUKI_ARG_MAX 65536
#define CIUKI_OPEN_MAX 128
#define CIUKI_PROCESS_MAX 64
#define CIUKI_THREAD_MAX 256
#define CIUKI_PROCESS_THREAD_MAX 16
#define CIUKI_OPEN_DESCRIPTION_MAX 1024
#define CIUKI_MAPPING_MAX 256
#define CIUKI_IO_MAX 1048576
#define CIUKI_SURFACE_MAX 16777216
#define CIUKI_MESSAGE_PAYLOAD_MAX 256
#define CIUKI_MESSAGE_FD_MAX 4
#define CIUKI_CHANNEL_QUEUE_MAX 64
#define CIUKI_INPUT_QUEUE_MAX 256
#define CIUKI_INPUT_READ_MAX 64
#define CIUKI_ARGV_MAX 256
#define CIUKI_ENVP_MAX 256
#define CIUKI_GETDENTS_MIN 792
#define CIUKI_GETDENTS_MAX 65536
#define CIUKI_PAGE_SIZE 4096
#define CIUKI_TLS_SIZE 4096
#define CIUKI_TLS_SELECTOR 0x3b
#define CIUKI_TLS_GDT_INDEX 7
#define CIUKI_THREAD_STACK_MIN 65536
#define CIUKI_THREAD_STACK_DEFAULT 1048576
#define CIUKI_THREAD_STACK_MAX 4194304
#define CIUKI_THREAD_KEYS_MAX 64
#define CIUKI_THREAD_DESTRUCTOR_ITERATIONS 4
#define CIUKI_ELF_PHDR_MAX 16
#define CIUKI_ELF_BYTES_MAX 33554432
#define CIUKI_IMAGE_BASE 0x00400000
#define CIUKI_IMAGE_LIMIT 0x10000000
#define CIUKI_HEAP_LIMIT 0x20000000
#define CIUKI_MMAP_BASE 0x20000000
#define CIUKI_MMAP_LIMIT 0xbfc00000
#define CIUKI_MAIN_STACK_BASE 0xbff00000
#define CIUKI_MAIN_STACK_LIMIT 0xc0000000
#define CIUKI_MAIN_STACK_RESERVATION_BASE 0xbfc00000
#define CIUKI_MAIN_STACK_GUARD_BASE 0xbfeff000
#define CIUKI_INITIAL_EFLAGS 0x202
#define CIUKI_SYSCALL_ERROR_MAX 4095
#define CIUKI_ID_MAX 0x7fffffff
#define CIUKI_DEBUG_WRITE_MAX 240
#define CIUKI_PROBE_REPORT_MAX 240
#define CIUKI_SLEEP_MS_MAX 60000
#define CIUKI_PROBE_QUERY_MAX 4096
#define CIUKI_SURFACE_XRGB8888 1
#define CIUKI_SURFACE_DIMENSION_MAX 2048
#define CIUKI_SURFACE_PIXEL_BYTES 4
#define DONTWAIT 1
#define CIUKI_INPUT_KEY 1
#define CIUKI_INPUT_TEXT 2
#define CIUKI_INPUT_MOTION 3
#define CIUKI_INPUT_BUTTON 4
#define CIUKI_INPUT_RESYNC 5
#define CIUKI_KEY_VALUE_UP 0
#define CIUKI_KEY_VALUE_DOWN 1
#define CIUKI_KEY_VALUE_REPEAT 2
#define CIUKI_BUTTON_LEFT 1
#define CIUKI_BUTTON_RIGHT 2
#define CIUKI_BUTTON_MIDDLE 3
#define CIUKI_FP_FNSAVE 1
#define SEGV_MAPERR 1
#define ILL_ILLOPC 1
#define BUS_ADRALN 1
#define FPE_INTDIV 1
#define SEGV_ACCERR 2
#define CIUKI_SI_X87 7
#define CIUKI_PTHREAD_MUTEX_NORMAL 0
#define CIUKI_PTHREAD_MUTEX_RECURSIVE 1
#define CIUKI_KEY_ESCAPE 0x1
#define CIUKI_KEY_1 0x2
#define CIUKI_KEY_2 0x3
#define CIUKI_KEY_3 0x4
#define CIUKI_KEY_4 0x5
#define CIUKI_KEY_5 0x6
#define CIUKI_KEY_6 0x7
#define CIUKI_KEY_7 0x8
#define CIUKI_KEY_8 0x9
#define CIUKI_KEY_9 0xa
#define CIUKI_KEY_0 0xb
#define CIUKI_KEY_MINUS 0xc
#define CIUKI_KEY_EQUAL 0xd
#define CIUKI_KEY_BACKSPACE 0xe
#define CIUKI_KEY_TAB 0xf
#define CIUKI_KEY_Q 0x10
#define CIUKI_KEY_W 0x11
#define CIUKI_KEY_E 0x12
#define CIUKI_KEY_R 0x13
#define CIUKI_KEY_T 0x14
#define CIUKI_KEY_Y 0x15
#define CIUKI_KEY_U 0x16
#define CIUKI_KEY_I 0x17
#define CIUKI_KEY_O 0x18
#define CIUKI_KEY_P 0x19
#define CIUKI_KEY_LEFTBRACKET 0x1a
#define CIUKI_KEY_RIGHTBRACKET 0x1b
#define CIUKI_KEY_ENTER 0x1c
#define CIUKI_KEY_LEFTCTRL 0x1d
#define CIUKI_KEY_A 0x1e
#define CIUKI_KEY_S 0x1f
#define CIUKI_KEY_D 0x20
#define CIUKI_KEY_F 0x21
#define CIUKI_KEY_G 0x22
#define CIUKI_KEY_H 0x23
#define CIUKI_KEY_J 0x24
#define CIUKI_KEY_K 0x25
#define CIUKI_KEY_L 0x26
#define CIUKI_KEY_SEMICOLON 0x27
#define CIUKI_KEY_APOSTROPHE 0x28
#define CIUKI_KEY_GRAVE 0x29
#define CIUKI_KEY_LEFTSHIFT 0x2a
#define CIUKI_KEY_BACKSLASH 0x2b
#define CIUKI_KEY_Z 0x2c
#define CIUKI_KEY_X 0x2d
#define CIUKI_KEY_C 0x2e
#define CIUKI_KEY_V 0x2f
#define CIUKI_KEY_B 0x30
#define CIUKI_KEY_N 0x31
#define CIUKI_KEY_M 0x32
#define CIUKI_KEY_COMMA 0x33
#define CIUKI_KEY_PERIOD 0x34
#define CIUKI_KEY_SLASH 0x35
#define CIUKI_KEY_RIGHTSHIFT 0x36
#define CIUKI_KEY_KP_MULTIPLY 0x37
#define CIUKI_KEY_LEFTALT 0x38
#define CIUKI_KEY_SPACE 0x39
#define CIUKI_KEY_CAPSLOCK 0x3a
#define CIUKI_KEY_F1 0x3b
#define CIUKI_KEY_F2 0x3c
#define CIUKI_KEY_F3 0x3d
#define CIUKI_KEY_F4 0x3e
#define CIUKI_KEY_F5 0x3f
#define CIUKI_KEY_F6 0x40
#define CIUKI_KEY_F7 0x41
#define CIUKI_KEY_F8 0x42
#define CIUKI_KEY_F9 0x43
#define CIUKI_KEY_F10 0x44
#define CIUKI_KEY_NUMLOCK 0x45
#define CIUKI_KEY_SCROLLLOCK 0x46
#define CIUKI_KEY_KP_7 0x47
#define CIUKI_KEY_KP_8 0x48
#define CIUKI_KEY_KP_9 0x49
#define CIUKI_KEY_KP_MINUS 0x4a
#define CIUKI_KEY_KP_4 0x4b
#define CIUKI_KEY_KP_5 0x4c
#define CIUKI_KEY_KP_6 0x4d
#define CIUKI_KEY_KP_PLUS 0x4e
#define CIUKI_KEY_KP_1 0x4f
#define CIUKI_KEY_KP_2 0x50
#define CIUKI_KEY_KP_3 0x51
#define CIUKI_KEY_KP_0 0x52
#define CIUKI_KEY_KP_PERIOD 0x53
#define CIUKI_KEY_102ND 0x56
#define CIUKI_KEY_F11 0x57
#define CIUKI_KEY_F12 0x58
#define CIUKI_KEY_KP_ENTER 0x11c
#define CIUKI_KEY_RIGHTCTRL 0x11d
#define CIUKI_KEY_KP_DIVIDE 0x135
#define CIUKI_KEY_PRINTSCREEN 0x137
#define CIUKI_KEY_RIGHTALT 0x138
#define CIUKI_KEY_HOME 0x147
#define CIUKI_KEY_UP 0x148
#define CIUKI_KEY_PAGEUP 0x149
#define CIUKI_KEY_LEFT 0x14b
#define CIUKI_KEY_RIGHT 0x14d
#define CIUKI_KEY_END 0x14f
#define CIUKI_KEY_DOWN 0x150
#define CIUKI_KEY_PAGEDOWN 0x151
#define CIUKI_KEY_INSERT 0x152
#define CIUKI_KEY_DELETE 0x153
#define CIUKI_KEY_LEFTMETA 0x15b
#define CIUKI_KEY_RIGHTMETA 0x15c
#define CIUKI_KEY_MENU 0x15d
#define CIUKI_KEY_PAUSE 0x200
#define CIUKI_KEY_E0 0x100
#define CIUKI_REG_GS 0
#define CIUKI_REG_FS 1
#define CIUKI_REG_ES 2
#define CIUKI_REG_DS 3
#define CIUKI_REG_EDI 4
#define CIUKI_REG_ESI 5
#define CIUKI_REG_EBP 6
#define CIUKI_REG_ESP 7
#define CIUKI_REG_EBX 8
#define CIUKI_REG_EDX 9
#define CIUKI_REG_ECX 10
#define CIUKI_REG_EAX 11
#define CIUKI_REG_VECTOR 12
#define CIUKI_REG_ERROR 13
#define CIUKI_REG_EIP 14
#define CIUKI_REG_CS 15
#define CIUKI_REG_EFLAGS 16
#define CIUKI_REG_USER_ESP 17
#define CIUKI_REG_SS 18

#define CIUKI_SIGBIT(signo) (UINT64_C(1) << ((signo) - 1))
#define WIFEXITED(status) (((uint32_t)(status) & 0x7fu) == 0)
#define WEXITSTATUS(status) (((uint32_t)(status) >> 8) & 0xffu)
#define WIFSIGNALED(status) (((uint32_t)(status) & 0x7fu) != 0)
#define WTERMSIG(status) ((uint32_t)(status) & 0x7fu)
#define CIUKI_PTHREAD_MUTEX_INITIALIZER {{0}}
#define CIUKI_PTHREAD_COND_INITIALIZER {{0}}
#define CIUKI_PTHREAD_ONCE_INIT 0

enum ciuki_syscall {
    CIUKI_SYS_EXIT = 0,
    CIUKI_SYS_YIELD = 1,
    CIUKI_SYS_DEBUG_WRITE = 2,
    CIUKI_SYS_PROBE_REPORT = 3,
    CIUKI_SYS_SLEEP_MS = 4,
    CIUKI_SYS_PROBE_QUERY = 5,
    CIUKI_SYS_RESERVED_6 = 6,
    CIUKI_SYS_RESERVED_7 = 7,
    CIUKI_SYS_RESERVED_8 = 8,
    CIUKI_SYS_RESERVED_9 = 9,
    CIUKI_SYS_RESERVED_10 = 10,
    CIUKI_SYS_RESERVED_11 = 11,
    CIUKI_SYS_RESERVED_12 = 12,
    CIUKI_SYS_RESERVED_13 = 13,
    CIUKI_SYS_RESERVED_14 = 14,
    CIUKI_SYS_RESERVED_15 = 15,
    CIUKI_SYS_SPAWN = 16,
    CIUKI_SYS_WAITPID = 17,
    CIUKI_SYS_GETPID = 18,
    CIUKI_SYS_GETPPID = 19,
    CIUKI_SYS_THREAD_CREATE = 20,
    CIUKI_SYS_THREAD_EXIT = 21,
    CIUKI_SYS_TLS_SET = 22,
    CIUKI_SYS_WAIT_WORD = 23,
    CIUKI_SYS_WAKE_WORD = 24,
    CIUKI_SYS_MMAP = 25,
    CIUKI_SYS_MUNMAP = 26,
    CIUKI_SYS_MPROTECT = 27,
    CIUKI_SYS_BRK = 28,
    CIUKI_SYS_OPEN = 29,
    CIUKI_SYS_READ = 30,
    CIUKI_SYS_WRITE = 31,
    CIUKI_SYS_PREAD = 32,
    CIUKI_SYS_PWRITE = 33,
    CIUKI_SYS_LSEEK64 = 34,
    CIUKI_SYS_CLOSE = 35,
    CIUKI_SYS_FSTAT = 36,
    CIUKI_SYS_STAT = 37,
    CIUKI_SYS_GETDENTS = 38,
    CIUKI_SYS_MKDIR = 39,
    CIUKI_SYS_RMDIR = 40,
    CIUKI_SYS_RENAME = 41,
    CIUKI_SYS_UNLINK = 42,
    CIUKI_SYS_DUP = 43,
    CIUKI_SYS_DUP2 = 44,
    CIUKI_SYS_FCNTL = 45,
    CIUKI_SYS_FSYNC = 46,
    CIUKI_SYS_FTRUNCATE = 47,
    CIUKI_SYS_GETCWD = 48,
    CIUKI_SYS_CHDIR = 49,
    CIUKI_SYS_CLOCK_GETTIME = 50,
    CIUKI_SYS_NANOSLEEP = 51,
    CIUKI_SYS_SIGACTION = 52,
    CIUKI_SYS_SIGPROCMASK = 53,
    CIUKI_SYS_KILL = 54,
    CIUKI_SYS_SIGRETURN = 55,
    CIUKI_SYS_UNAME = 56,
    CIUKI_SYS_SURFACE_CREATE = 57,
    CIUKI_SYS_SURFACE_MAP = 58,
    CIUKI_SYS_SURFACE_INFO = 59,
    CIUKI_SYS_PRESENT = 60,
    CIUKI_SYS_INPUT_READ = 61,
    CIUKI_SYS_CHANNEL_PAIR = 62,
    CIUKI_SYS_CHANNEL_SEND = 63,
    CIUKI_SYS_CHANNEL_RECV = 64,
    CIUKI_SYS_THREAD_JOIN = 65,
    CIUKI_SYS_THREAD_DETACH = 66,
    CIUKI_SYS_DISPLAY_INFO = 67,
    CIUKI_SYS_THREAD_KILL = 68,
};

/* pack(4) is scoped and restores the includer's previous setting. */
#pragma pack(push, 4)

struct ciuki_timespec {
    int64_t tv_sec; /* 0 */
    int32_t tv_nsec; /* 8 */
    uint32_t reserved; /* 12 */
};

struct ciuki_timeval {
    int64_t tv_sec; /* 0 */
    int32_t tv_usec; /* 8 */
    uint32_t reserved; /* 12 */
};

struct ciuki_stat {
    uint32_t st_dev; /* 0 */
    uint32_t st_mode; /* 4 */
    uint64_t st_ino; /* 8 */
    uint32_t st_nlink; /* 16 */
    uint32_t st_uid; /* 20 */
    uint32_t st_gid; /* 24 */
    uint32_t st_rdev; /* 28 */
    int64_t st_size; /* 32 */
    int32_t st_blksize; /* 40 */
    uint32_t reserved; /* 44 */
    int64_t st_blocks; /* 48 */
    struct ciuki_timespec st_atim; /* 56 */
    struct ciuki_timespec st_mtim; /* 72 */
    struct ciuki_timespec st_ctim; /* 88 */
};

struct ciuki_dirent {
    uint64_t d_ino; /* 0 */
    int64_t d_off; /* 8 */
    uint16_t d_reclen; /* 16 */
    uint16_t d_namlen; /* 18 */
    uint8_t d_type; /* 20 */
    uint8_t reserved[3]; /* 21 */
    char d_name[768]; /* 24 */
};

struct ciuki_spawn_fd {
    int32_t source; /* 0 */
    int32_t target; /* 4 */
};

struct ciuki_spawn_args {
    uint32_t size; /* 0 */
    uint32_t path; /* 4 */
    uint32_t argv; /* 8 */
    uint32_t envp; /* 12 */
    uint32_t fd_list; /* 16 */
    uint32_t fd_count; /* 20 */
    uint32_t flags; /* 24 */
    uint32_t reserved; /* 28 */
};

struct ciuki_thread_args {
    uint32_t size; /* 0 */
    uint32_t entry; /* 4 */
    uint32_t argument; /* 8 */
    uint32_t return_trampoline; /* 12 */
    uint32_t stack_bytes; /* 16 */
    uint32_t flags; /* 20 */
    uint32_t reserved[2]; /* 24 */
};

struct ciuki_mmap_args {
    uint32_t size; /* 0 */
    uint32_t hint; /* 4 */
    uint32_t length; /* 8 */
    uint32_t prot; /* 12 */
    uint32_t flags; /* 16 */
    int32_t fd; /* 20 */
    int64_t offset; /* 24 */
};

struct ciuki_sigaction {
    uint32_t handler; /* 0 */
    uint32_t flags; /* 4 */
    uint64_t mask; /* 8 */
    uint32_t restorer; /* 16 */
    uint32_t reserved; /* 20 */
};

struct ciuki_siginfo {
    int32_t signo; /* 0 */
    int32_t code; /* 4 */
    int32_t error; /* 8 */
    int32_t sender_pid; /* 12 */
    uint32_t fault_addr; /* 16 */
    uint32_t vector; /* 20 */
    uint32_t trap_error; /* 24 */
    uint32_t reserved; /* 28 */
};

struct ciuki_utsname {
    char sysname[65]; /* 0 */
    char nodename[65]; /* 65 */
    char release[65]; /* 130 */
    char version[65]; /* 195 */
    char machine[65]; /* 260 */
    uint8_t reserved[3]; /* 325 */
    uint32_t abi_version; /* 328 */
    uint32_t realtime_source; /* 332 */
};

struct ciuki_ucontext {
    uint32_t size; /* 0 */
    uint32_t flags; /* 4 */
    uint32_t link; /* 8 */
    uint32_t stack_base; /* 12 */
    uint32_t stack_bytes; /* 16 */
    uint32_t stack_flags; /* 20 */
    uint64_t mask; /* 24 */
    uint32_t gregs[19]; /* 32 */
    uint32_t cr2; /* 108 */
    uint32_t fp_format; /* 112 */
    uint8_t fp_state[108]; /* 116 */
    uint8_t reserved[32]; /* 224 */
};

struct ciuki_signal_frame {
    uint32_t restorer; /* 0 */
    int32_t signo; /* 4 */
    uint32_t siginfo_ptr; /* 8 */
    uint32_t context_ptr; /* 12 */
    uint32_t size; /* 16 */
    uint32_t version; /* 20 */
    uint64_t token; /* 24 */
    struct ciuki_siginfo info; /* 32 */
    struct ciuki_ucontext context; /* 64 */
};

struct ciuki_tcb {
    uint32_t self; /* 0 */
    uint32_t tid; /* 4 */
    uint32_t reent; /* 8 */
    uint32_t pthread_private; /* 12 */
    uint32_t stack_base; /* 16 */
    uint32_t stack_bytes; /* 20 */
    uint32_t flags; /* 24 */
    uint32_t reserved[9]; /* 28 */
};

struct ciuki_surface_info {
    uint32_t size; /* 0 */
    uint32_t width; /* 4 */
    uint32_t height; /* 8 */
    uint32_t stride; /* 12 */
    uint32_t format; /* 16 */
    uint32_t allocation_bytes; /* 20 */
};

struct ciuki_display_info {
    uint32_t size; /* 0 */
    uint32_t width; /* 4 */
    uint32_t height; /* 8 */
    uint32_t pitch; /* 12 */
    uint32_t bpp; /* 16 */
    uint32_t red_size; /* 20 */
    uint32_t red_pos; /* 24 */
    uint32_t green_size; /* 28 */
    uint32_t green_pos; /* 32 */
    uint32_t blue_size; /* 36 */
    uint32_t blue_pos; /* 40 */
    uint32_t generation; /* 44 */
};

struct ciuki_rect {
    int32_t src_x; /* 0 */
    int32_t src_y; /* 4 */
    int32_t dst_x; /* 8 */
    int32_t dst_y; /* 12 */
    uint32_t width; /* 16 */
    uint32_t height; /* 20 */
};

struct ciuki_input_event {
    uint32_t sequence; /* 0 */
    uint32_t source; /* 4 */
    uint32_t generation; /* 8 */
    uint32_t type; /* 12 */
    uint64_t monotonic_ns; /* 16 */
    int32_t code; /* 24 */
    int32_t value; /* 28 */
    int32_t value2; /* 32 */
    uint32_t lost_count; /* 36 */
};

struct ciuki_message {
    uint32_t length; /* 0 */
    uint32_t fd_count; /* 4 */
    int32_t fds[4]; /* 8 */
    uint32_t sender_pid; /* 24 */
    uint32_t reserved; /* 28 */
    uint8_t data[256]; /* 32 */
};

struct ciuki_pthread_mutex {
    uint32_t words[8]; /* 0 */
};

struct ciuki_pthread_cond {
    uint32_t words[4]; /* 0 */
};

struct ciuki_pthread_attr {
    uint32_t stack_bytes; /* 0 */
    uint32_t detached; /* 4 */
    uint32_t reserved[2]; /* 8 */
};

struct ciuki_pthread_mutexattr {
    uint32_t type; /* 0 */
    uint32_t reserved; /* 4 */
};

struct ciuki_pthread_condattr {
    uint32_t clock_id; /* 0 */
    uint32_t reserved; /* 4 */
};

#pragma pack(pop)

typedef struct ciuki_pthread_mutex ciuki_pthread_mutex_t;
typedef struct ciuki_pthread_cond ciuki_pthread_cond_t;
typedef struct ciuki_pthread_attr ciuki_pthread_attr_t;
typedef struct ciuki_pthread_mutexattr ciuki_pthread_mutexattr_t;
typedef struct ciuki_pthread_condattr ciuki_pthread_condattr_t;
typedef uint32_t ciuki_pthread_once_t;

/* offsetof without a second standard header (clang/GCC builtin). */
_Static_assert((char)-1 < 0, "ABI plain char is signed");
/* These describe target C objects, rather than the host's wire-record view. */
#if defined(__i386__)
_Static_assert(sizeof(char) == 1, "ABI char size");
_Static_assert(sizeof(short) == 2, "ABI short size");
_Static_assert(sizeof(int) == 4, "ABI int size");
_Static_assert(sizeof(long) == 4, "ABI long size");
_Static_assert(sizeof(void *) == 4, "ABI pointer size");
_Static_assert(sizeof(long long) == 8, "ABI long long size");
_Static_assert(sizeof(float) == 4, "ABI float size");
_Static_assert(sizeof(double) == 8, "ABI double size");
_Static_assert(sizeof(long double) == 12, "ABI long double size");
_Static_assert(_Alignof(long double) == 4, "ABI long double alignment");
#endif
_Static_assert(sizeof(ciuki_size_t) == 4, "ciuki_size_t size");
_Static_assert(sizeof(ciuki_uintptr_t) == 4, "ciuki_uintptr_t size");
_Static_assert(sizeof(ciuki_ssize_t) == 4, "ciuki_ssize_t size");
_Static_assert(sizeof(ciuki_ptrdiff_t) == 4, "ciuki_ptrdiff_t size");
_Static_assert(sizeof(ciuki_pid_t) == 4, "ciuki_pid_t size");
_Static_assert(sizeof(ciuki_off_t) == 8, "ciuki_off_t size");
_Static_assert(sizeof(ciuki_time_t) == 8, "ciuki_time_t size");
_Static_assert(sizeof(ciuki_clock_t) == 8, "ciuki_clock_t size");
_Static_assert(sizeof(ciuki_ino_t) == 8, "ciuki_ino_t size");
_Static_assert(sizeof(ciuki_mode_t) == 4, "ciuki_mode_t size");
_Static_assert(sizeof(ciuki_nlink_t) == 4, "ciuki_nlink_t size");
_Static_assert(sizeof(ciuki_uid_t) == 4, "ciuki_uid_t size");
_Static_assert(sizeof(ciuki_gid_t) == 4, "ciuki_gid_t size");
_Static_assert(sizeof(ciuki_dev_t) == 4, "ciuki_dev_t size");
_Static_assert(sizeof(ciuki_blksize_t) == 4, "ciuki_blksize_t size");
_Static_assert(sizeof(ciuki_blkcnt_t) == 8, "ciuki_blkcnt_t size");
_Static_assert(sizeof(ciuki_pthread_t) == 4, "ciuki_pthread_t size");
_Static_assert(sizeof(ciuki_sigset_t) == 8, "ciuki_sigset_t size");
_Static_assert(sizeof(ciuki_pthread_mutex_t) == 32, "ciuki_pthread_mutex_t size");
_Static_assert(sizeof(ciuki_pthread_cond_t) == 16, "ciuki_pthread_cond_t size");
_Static_assert(sizeof(ciuki_pthread_attr_t) == 16, "ciuki_pthread_attr_t size");
_Static_assert(sizeof(ciuki_pthread_mutexattr_t) == 8, "ciuki_pthread_mutexattr_t size");
_Static_assert(sizeof(ciuki_pthread_condattr_t) == 8, "ciuki_pthread_condattr_t size");
_Static_assert(sizeof(ciuki_pthread_once_t) == 4, "ciuki_pthread_once_t size");
_Static_assert(sizeof(struct ciuki_timespec) == 16, "ciuki_timespec size");
_Static_assert(_Alignof(struct ciuki_timespec) == 4, "ciuki_timespec alignment");
_Static_assert(__builtin_offsetof(struct ciuki_timespec, tv_sec) == 0, "ciuki_timespec.tv_sec offset");
_Static_assert(__builtin_offsetof(struct ciuki_timespec, tv_nsec) == 8, "ciuki_timespec.tv_nsec offset");
_Static_assert(__builtin_offsetof(struct ciuki_timespec, reserved) == 12, "ciuki_timespec.reserved offset");
_Static_assert(sizeof(struct ciuki_timeval) == 16, "ciuki_timeval size");
_Static_assert(_Alignof(struct ciuki_timeval) == 4, "ciuki_timeval alignment");
_Static_assert(__builtin_offsetof(struct ciuki_timeval, tv_sec) == 0, "ciuki_timeval.tv_sec offset");
_Static_assert(__builtin_offsetof(struct ciuki_timeval, tv_usec) == 8, "ciuki_timeval.tv_usec offset");
_Static_assert(__builtin_offsetof(struct ciuki_timeval, reserved) == 12, "ciuki_timeval.reserved offset");
_Static_assert(sizeof(struct ciuki_stat) == 104, "ciuki_stat size");
_Static_assert(_Alignof(struct ciuki_stat) == 4, "ciuki_stat alignment");
_Static_assert(__builtin_offsetof(struct ciuki_stat, st_dev) == 0, "ciuki_stat.st_dev offset");
_Static_assert(__builtin_offsetof(struct ciuki_stat, st_mode) == 4, "ciuki_stat.st_mode offset");
_Static_assert(__builtin_offsetof(struct ciuki_stat, st_ino) == 8, "ciuki_stat.st_ino offset");
_Static_assert(__builtin_offsetof(struct ciuki_stat, st_nlink) == 16, "ciuki_stat.st_nlink offset");
_Static_assert(__builtin_offsetof(struct ciuki_stat, st_uid) == 20, "ciuki_stat.st_uid offset");
_Static_assert(__builtin_offsetof(struct ciuki_stat, st_gid) == 24, "ciuki_stat.st_gid offset");
_Static_assert(__builtin_offsetof(struct ciuki_stat, st_rdev) == 28, "ciuki_stat.st_rdev offset");
_Static_assert(__builtin_offsetof(struct ciuki_stat, st_size) == 32, "ciuki_stat.st_size offset");
_Static_assert(__builtin_offsetof(struct ciuki_stat, st_blksize) == 40, "ciuki_stat.st_blksize offset");
_Static_assert(__builtin_offsetof(struct ciuki_stat, reserved) == 44, "ciuki_stat.reserved offset");
_Static_assert(__builtin_offsetof(struct ciuki_stat, st_blocks) == 48, "ciuki_stat.st_blocks offset");
_Static_assert(__builtin_offsetof(struct ciuki_stat, st_atim) == 56, "ciuki_stat.st_atim offset");
_Static_assert(__builtin_offsetof(struct ciuki_stat, st_mtim) == 72, "ciuki_stat.st_mtim offset");
_Static_assert(__builtin_offsetof(struct ciuki_stat, st_ctim) == 88, "ciuki_stat.st_ctim offset");
_Static_assert(sizeof(struct ciuki_dirent) == 792, "ciuki_dirent size");
_Static_assert(_Alignof(struct ciuki_dirent) == 4, "ciuki_dirent alignment");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_ino) == 0, "ciuki_dirent.d_ino offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_off) == 8, "ciuki_dirent.d_off offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_reclen) == 16, "ciuki_dirent.d_reclen offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_namlen) == 18, "ciuki_dirent.d_namlen offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_type) == 20, "ciuki_dirent.d_type offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, reserved) == 21, "ciuki_dirent.reserved offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_name) == 24, "ciuki_dirent.d_name offset");
_Static_assert(sizeof(struct ciuki_spawn_fd) == 8, "ciuki_spawn_fd size");
_Static_assert(_Alignof(struct ciuki_spawn_fd) == 4, "ciuki_spawn_fd alignment");
_Static_assert(__builtin_offsetof(struct ciuki_spawn_fd, source) == 0, "ciuki_spawn_fd.source offset");
_Static_assert(__builtin_offsetof(struct ciuki_spawn_fd, target) == 4, "ciuki_spawn_fd.target offset");
_Static_assert(sizeof(struct ciuki_spawn_args) == 32, "ciuki_spawn_args size");
_Static_assert(_Alignof(struct ciuki_spawn_args) == 4, "ciuki_spawn_args alignment");
_Static_assert(__builtin_offsetof(struct ciuki_spawn_args, size) == 0, "ciuki_spawn_args.size offset");
_Static_assert(__builtin_offsetof(struct ciuki_spawn_args, path) == 4, "ciuki_spawn_args.path offset");
_Static_assert(__builtin_offsetof(struct ciuki_spawn_args, argv) == 8, "ciuki_spawn_args.argv offset");
_Static_assert(__builtin_offsetof(struct ciuki_spawn_args, envp) == 12, "ciuki_spawn_args.envp offset");
_Static_assert(__builtin_offsetof(struct ciuki_spawn_args, fd_list) == 16, "ciuki_spawn_args.fd_list offset");
_Static_assert(__builtin_offsetof(struct ciuki_spawn_args, fd_count) == 20, "ciuki_spawn_args.fd_count offset");
_Static_assert(__builtin_offsetof(struct ciuki_spawn_args, flags) == 24, "ciuki_spawn_args.flags offset");
_Static_assert(__builtin_offsetof(struct ciuki_spawn_args, reserved) == 28, "ciuki_spawn_args.reserved offset");
_Static_assert(sizeof(struct ciuki_thread_args) == 32, "ciuki_thread_args size");
_Static_assert(_Alignof(struct ciuki_thread_args) == 4, "ciuki_thread_args alignment");
_Static_assert(__builtin_offsetof(struct ciuki_thread_args, size) == 0, "ciuki_thread_args.size offset");
_Static_assert(__builtin_offsetof(struct ciuki_thread_args, entry) == 4, "ciuki_thread_args.entry offset");
_Static_assert(__builtin_offsetof(struct ciuki_thread_args, argument) == 8, "ciuki_thread_args.argument offset");
_Static_assert(__builtin_offsetof(struct ciuki_thread_args, return_trampoline) == 12, "ciuki_thread_args.return_trampoline offset");
_Static_assert(__builtin_offsetof(struct ciuki_thread_args, stack_bytes) == 16, "ciuki_thread_args.stack_bytes offset");
_Static_assert(__builtin_offsetof(struct ciuki_thread_args, flags) == 20, "ciuki_thread_args.flags offset");
_Static_assert(__builtin_offsetof(struct ciuki_thread_args, reserved) == 24, "ciuki_thread_args.reserved offset");
_Static_assert(sizeof(struct ciuki_mmap_args) == 32, "ciuki_mmap_args size");
_Static_assert(_Alignof(struct ciuki_mmap_args) == 4, "ciuki_mmap_args alignment");
_Static_assert(__builtin_offsetof(struct ciuki_mmap_args, size) == 0, "ciuki_mmap_args.size offset");
_Static_assert(__builtin_offsetof(struct ciuki_mmap_args, hint) == 4, "ciuki_mmap_args.hint offset");
_Static_assert(__builtin_offsetof(struct ciuki_mmap_args, length) == 8, "ciuki_mmap_args.length offset");
_Static_assert(__builtin_offsetof(struct ciuki_mmap_args, prot) == 12, "ciuki_mmap_args.prot offset");
_Static_assert(__builtin_offsetof(struct ciuki_mmap_args, flags) == 16, "ciuki_mmap_args.flags offset");
_Static_assert(__builtin_offsetof(struct ciuki_mmap_args, fd) == 20, "ciuki_mmap_args.fd offset");
_Static_assert(__builtin_offsetof(struct ciuki_mmap_args, offset) == 24, "ciuki_mmap_args.offset offset");
_Static_assert(sizeof(struct ciuki_sigaction) == 24, "ciuki_sigaction size");
_Static_assert(_Alignof(struct ciuki_sigaction) == 4, "ciuki_sigaction alignment");
_Static_assert(__builtin_offsetof(struct ciuki_sigaction, handler) == 0, "ciuki_sigaction.handler offset");
_Static_assert(__builtin_offsetof(struct ciuki_sigaction, flags) == 4, "ciuki_sigaction.flags offset");
_Static_assert(__builtin_offsetof(struct ciuki_sigaction, mask) == 8, "ciuki_sigaction.mask offset");
_Static_assert(__builtin_offsetof(struct ciuki_sigaction, restorer) == 16, "ciuki_sigaction.restorer offset");
_Static_assert(__builtin_offsetof(struct ciuki_sigaction, reserved) == 20, "ciuki_sigaction.reserved offset");
_Static_assert(sizeof(struct ciuki_siginfo) == 32, "ciuki_siginfo size");
_Static_assert(_Alignof(struct ciuki_siginfo) == 4, "ciuki_siginfo alignment");
_Static_assert(__builtin_offsetof(struct ciuki_siginfo, signo) == 0, "ciuki_siginfo.signo offset");
_Static_assert(__builtin_offsetof(struct ciuki_siginfo, code) == 4, "ciuki_siginfo.code offset");
_Static_assert(__builtin_offsetof(struct ciuki_siginfo, error) == 8, "ciuki_siginfo.error offset");
_Static_assert(__builtin_offsetof(struct ciuki_siginfo, sender_pid) == 12, "ciuki_siginfo.sender_pid offset");
_Static_assert(__builtin_offsetof(struct ciuki_siginfo, fault_addr) == 16, "ciuki_siginfo.fault_addr offset");
_Static_assert(__builtin_offsetof(struct ciuki_siginfo, vector) == 20, "ciuki_siginfo.vector offset");
_Static_assert(__builtin_offsetof(struct ciuki_siginfo, trap_error) == 24, "ciuki_siginfo.trap_error offset");
_Static_assert(__builtin_offsetof(struct ciuki_siginfo, reserved) == 28, "ciuki_siginfo.reserved offset");
_Static_assert(sizeof(struct ciuki_utsname) == 336, "ciuki_utsname size");
_Static_assert(_Alignof(struct ciuki_utsname) == 4, "ciuki_utsname alignment");
_Static_assert(__builtin_offsetof(struct ciuki_utsname, sysname) == 0, "ciuki_utsname.sysname offset");
_Static_assert(__builtin_offsetof(struct ciuki_utsname, nodename) == 65, "ciuki_utsname.nodename offset");
_Static_assert(__builtin_offsetof(struct ciuki_utsname, release) == 130, "ciuki_utsname.release offset");
_Static_assert(__builtin_offsetof(struct ciuki_utsname, version) == 195, "ciuki_utsname.version offset");
_Static_assert(__builtin_offsetof(struct ciuki_utsname, machine) == 260, "ciuki_utsname.machine offset");
_Static_assert(__builtin_offsetof(struct ciuki_utsname, reserved) == 325, "ciuki_utsname.reserved offset");
_Static_assert(__builtin_offsetof(struct ciuki_utsname, abi_version) == 328, "ciuki_utsname.abi_version offset");
_Static_assert(__builtin_offsetof(struct ciuki_utsname, realtime_source) == 332, "ciuki_utsname.realtime_source offset");
_Static_assert(sizeof(struct ciuki_ucontext) == 256, "ciuki_ucontext size");
_Static_assert(_Alignof(struct ciuki_ucontext) == 4, "ciuki_ucontext alignment");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, size) == 0, "ciuki_ucontext.size offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, flags) == 4, "ciuki_ucontext.flags offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, link) == 8, "ciuki_ucontext.link offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, stack_base) == 12, "ciuki_ucontext.stack_base offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, stack_bytes) == 16, "ciuki_ucontext.stack_bytes offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, stack_flags) == 20, "ciuki_ucontext.stack_flags offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, mask) == 24, "ciuki_ucontext.mask offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs) == 32, "ciuki_ucontext.gregs offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, cr2) == 108, "ciuki_ucontext.cr2 offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, fp_format) == 112, "ciuki_ucontext.fp_format offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, fp_state) == 116, "ciuki_ucontext.fp_state offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, reserved) == 224, "ciuki_ucontext.reserved offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[0]) == 32, "ciuki_ucontext.gs offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[1]) == 36, "ciuki_ucontext.fs offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[2]) == 40, "ciuki_ucontext.es offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[3]) == 44, "ciuki_ucontext.ds offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[4]) == 48, "ciuki_ucontext.edi offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[5]) == 52, "ciuki_ucontext.esi offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[6]) == 56, "ciuki_ucontext.ebp offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[7]) == 60, "ciuki_ucontext.esp offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[8]) == 64, "ciuki_ucontext.ebx offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[9]) == 68, "ciuki_ucontext.edx offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[10]) == 72, "ciuki_ucontext.ecx offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[11]) == 76, "ciuki_ucontext.eax offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[12]) == 80, "ciuki_ucontext.vector offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[13]) == 84, "ciuki_ucontext.error offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[14]) == 88, "ciuki_ucontext.eip offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[15]) == 92, "ciuki_ucontext.cs offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[16]) == 96, "ciuki_ucontext.eflags offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[17]) == 100, "ciuki_ucontext.user_esp offset");
_Static_assert(__builtin_offsetof(struct ciuki_ucontext, gregs[18]) == 104, "ciuki_ucontext.ss offset");
_Static_assert(sizeof(struct ciuki_signal_frame) == 320, "ciuki_signal_frame size");
_Static_assert(_Alignof(struct ciuki_signal_frame) == 4, "ciuki_signal_frame alignment");
_Static_assert(__builtin_offsetof(struct ciuki_signal_frame, restorer) == 0, "ciuki_signal_frame.restorer offset");
_Static_assert(__builtin_offsetof(struct ciuki_signal_frame, signo) == 4, "ciuki_signal_frame.signo offset");
_Static_assert(__builtin_offsetof(struct ciuki_signal_frame, siginfo_ptr) == 8, "ciuki_signal_frame.siginfo_ptr offset");
_Static_assert(__builtin_offsetof(struct ciuki_signal_frame, context_ptr) == 12, "ciuki_signal_frame.context_ptr offset");
_Static_assert(__builtin_offsetof(struct ciuki_signal_frame, size) == 16, "ciuki_signal_frame.size offset");
_Static_assert(__builtin_offsetof(struct ciuki_signal_frame, version) == 20, "ciuki_signal_frame.version offset");
_Static_assert(__builtin_offsetof(struct ciuki_signal_frame, token) == 24, "ciuki_signal_frame.token offset");
_Static_assert(__builtin_offsetof(struct ciuki_signal_frame, info) == 32, "ciuki_signal_frame.info offset");
_Static_assert(__builtin_offsetof(struct ciuki_signal_frame, context) == 64, "ciuki_signal_frame.context offset");
_Static_assert(sizeof(struct ciuki_tcb) == 64, "ciuki_tcb size");
_Static_assert(_Alignof(struct ciuki_tcb) == 4, "ciuki_tcb alignment");
_Static_assert(__builtin_offsetof(struct ciuki_tcb, self) == 0, "ciuki_tcb.self offset");
_Static_assert(__builtin_offsetof(struct ciuki_tcb, tid) == 4, "ciuki_tcb.tid offset");
_Static_assert(__builtin_offsetof(struct ciuki_tcb, reent) == 8, "ciuki_tcb.reent offset");
_Static_assert(__builtin_offsetof(struct ciuki_tcb, pthread_private) == 12, "ciuki_tcb.pthread_private offset");
_Static_assert(__builtin_offsetof(struct ciuki_tcb, stack_base) == 16, "ciuki_tcb.stack_base offset");
_Static_assert(__builtin_offsetof(struct ciuki_tcb, stack_bytes) == 20, "ciuki_tcb.stack_bytes offset");
_Static_assert(__builtin_offsetof(struct ciuki_tcb, flags) == 24, "ciuki_tcb.flags offset");
_Static_assert(__builtin_offsetof(struct ciuki_tcb, reserved) == 28, "ciuki_tcb.reserved offset");
_Static_assert(sizeof(struct ciuki_surface_info) == 24, "ciuki_surface_info size");
_Static_assert(_Alignof(struct ciuki_surface_info) == 4, "ciuki_surface_info alignment");
_Static_assert(__builtin_offsetof(struct ciuki_surface_info, size) == 0, "ciuki_surface_info.size offset");
_Static_assert(__builtin_offsetof(struct ciuki_surface_info, width) == 4, "ciuki_surface_info.width offset");
_Static_assert(__builtin_offsetof(struct ciuki_surface_info, height) == 8, "ciuki_surface_info.height offset");
_Static_assert(__builtin_offsetof(struct ciuki_surface_info, stride) == 12, "ciuki_surface_info.stride offset");
_Static_assert(__builtin_offsetof(struct ciuki_surface_info, format) == 16, "ciuki_surface_info.format offset");
_Static_assert(__builtin_offsetof(struct ciuki_surface_info, allocation_bytes) == 20, "ciuki_surface_info.allocation_bytes offset");
_Static_assert(sizeof(struct ciuki_display_info) == 48, "ciuki_display_info size");
_Static_assert(_Alignof(struct ciuki_display_info) == 4, "ciuki_display_info alignment");
_Static_assert(__builtin_offsetof(struct ciuki_display_info, size) == 0, "ciuki_display_info.size offset");
_Static_assert(__builtin_offsetof(struct ciuki_display_info, width) == 4, "ciuki_display_info.width offset");
_Static_assert(__builtin_offsetof(struct ciuki_display_info, height) == 8, "ciuki_display_info.height offset");
_Static_assert(__builtin_offsetof(struct ciuki_display_info, pitch) == 12, "ciuki_display_info.pitch offset");
_Static_assert(__builtin_offsetof(struct ciuki_display_info, bpp) == 16, "ciuki_display_info.bpp offset");
_Static_assert(__builtin_offsetof(struct ciuki_display_info, red_size) == 20, "ciuki_display_info.red_size offset");
_Static_assert(__builtin_offsetof(struct ciuki_display_info, red_pos) == 24, "ciuki_display_info.red_pos offset");
_Static_assert(__builtin_offsetof(struct ciuki_display_info, green_size) == 28, "ciuki_display_info.green_size offset");
_Static_assert(__builtin_offsetof(struct ciuki_display_info, green_pos) == 32, "ciuki_display_info.green_pos offset");
_Static_assert(__builtin_offsetof(struct ciuki_display_info, blue_size) == 36, "ciuki_display_info.blue_size offset");
_Static_assert(__builtin_offsetof(struct ciuki_display_info, blue_pos) == 40, "ciuki_display_info.blue_pos offset");
_Static_assert(__builtin_offsetof(struct ciuki_display_info, generation) == 44, "ciuki_display_info.generation offset");
_Static_assert(sizeof(struct ciuki_rect) == 24, "ciuki_rect size");
_Static_assert(_Alignof(struct ciuki_rect) == 4, "ciuki_rect alignment");
_Static_assert(__builtin_offsetof(struct ciuki_rect, src_x) == 0, "ciuki_rect.src_x offset");
_Static_assert(__builtin_offsetof(struct ciuki_rect, src_y) == 4, "ciuki_rect.src_y offset");
_Static_assert(__builtin_offsetof(struct ciuki_rect, dst_x) == 8, "ciuki_rect.dst_x offset");
_Static_assert(__builtin_offsetof(struct ciuki_rect, dst_y) == 12, "ciuki_rect.dst_y offset");
_Static_assert(__builtin_offsetof(struct ciuki_rect, width) == 16, "ciuki_rect.width offset");
_Static_assert(__builtin_offsetof(struct ciuki_rect, height) == 20, "ciuki_rect.height offset");
_Static_assert(sizeof(struct ciuki_input_event) == 40, "ciuki_input_event size");
_Static_assert(_Alignof(struct ciuki_input_event) == 4, "ciuki_input_event alignment");
_Static_assert(__builtin_offsetof(struct ciuki_input_event, sequence) == 0, "ciuki_input_event.sequence offset");
_Static_assert(__builtin_offsetof(struct ciuki_input_event, source) == 4, "ciuki_input_event.source offset");
_Static_assert(__builtin_offsetof(struct ciuki_input_event, generation) == 8, "ciuki_input_event.generation offset");
_Static_assert(__builtin_offsetof(struct ciuki_input_event, type) == 12, "ciuki_input_event.type offset");
_Static_assert(__builtin_offsetof(struct ciuki_input_event, monotonic_ns) == 16, "ciuki_input_event.monotonic_ns offset");
_Static_assert(__builtin_offsetof(struct ciuki_input_event, code) == 24, "ciuki_input_event.code offset");
_Static_assert(__builtin_offsetof(struct ciuki_input_event, value) == 28, "ciuki_input_event.value offset");
_Static_assert(__builtin_offsetof(struct ciuki_input_event, value2) == 32, "ciuki_input_event.value2 offset");
_Static_assert(__builtin_offsetof(struct ciuki_input_event, lost_count) == 36, "ciuki_input_event.lost_count offset");
_Static_assert(sizeof(struct ciuki_message) == 288, "ciuki_message size");
_Static_assert(_Alignof(struct ciuki_message) == 4, "ciuki_message alignment");
_Static_assert(__builtin_offsetof(struct ciuki_message, length) == 0, "ciuki_message.length offset");
_Static_assert(__builtin_offsetof(struct ciuki_message, fd_count) == 4, "ciuki_message.fd_count offset");
_Static_assert(__builtin_offsetof(struct ciuki_message, fds) == 8, "ciuki_message.fds offset");
_Static_assert(__builtin_offsetof(struct ciuki_message, sender_pid) == 24, "ciuki_message.sender_pid offset");
_Static_assert(__builtin_offsetof(struct ciuki_message, reserved) == 28, "ciuki_message.reserved offset");
_Static_assert(__builtin_offsetof(struct ciuki_message, data) == 32, "ciuki_message.data offset");
_Static_assert(sizeof(struct ciuki_pthread_mutex) == 32, "ciuki_pthread_mutex size");
_Static_assert(_Alignof(struct ciuki_pthread_mutex) == 4, "ciuki_pthread_mutex alignment");
_Static_assert(__builtin_offsetof(struct ciuki_pthread_mutex, words) == 0, "ciuki_pthread_mutex.words offset");
_Static_assert(sizeof(struct ciuki_pthread_cond) == 16, "ciuki_pthread_cond size");
_Static_assert(_Alignof(struct ciuki_pthread_cond) == 4, "ciuki_pthread_cond alignment");
_Static_assert(__builtin_offsetof(struct ciuki_pthread_cond, words) == 0, "ciuki_pthread_cond.words offset");
_Static_assert(sizeof(struct ciuki_pthread_attr) == 16, "ciuki_pthread_attr size");
_Static_assert(_Alignof(struct ciuki_pthread_attr) == 4, "ciuki_pthread_attr alignment");
_Static_assert(__builtin_offsetof(struct ciuki_pthread_attr, stack_bytes) == 0, "ciuki_pthread_attr.stack_bytes offset");
_Static_assert(__builtin_offsetof(struct ciuki_pthread_attr, detached) == 4, "ciuki_pthread_attr.detached offset");
_Static_assert(__builtin_offsetof(struct ciuki_pthread_attr, reserved) == 8, "ciuki_pthread_attr.reserved offset");
_Static_assert(sizeof(struct ciuki_pthread_mutexattr) == 8, "ciuki_pthread_mutexattr size");
_Static_assert(_Alignof(struct ciuki_pthread_mutexattr) == 4, "ciuki_pthread_mutexattr alignment");
_Static_assert(__builtin_offsetof(struct ciuki_pthread_mutexattr, type) == 0, "ciuki_pthread_mutexattr.type offset");
_Static_assert(__builtin_offsetof(struct ciuki_pthread_mutexattr, reserved) == 4, "ciuki_pthread_mutexattr.reserved offset");
_Static_assert(sizeof(struct ciuki_pthread_condattr) == 8, "ciuki_pthread_condattr size");
_Static_assert(_Alignof(struct ciuki_pthread_condattr) == 4, "ciuki_pthread_condattr alignment");
_Static_assert(__builtin_offsetof(struct ciuki_pthread_condattr, clock_id) == 0, "ciuki_pthread_condattr.clock_id offset");
_Static_assert(__builtin_offsetof(struct ciuki_pthread_condattr, reserved) == 4, "ciuki_pthread_condattr.reserved offset");

#endif /* CIUKI_ABI_H */
