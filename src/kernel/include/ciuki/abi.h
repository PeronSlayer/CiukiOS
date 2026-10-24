/* CiukiOS native ABI v1; authority: docs/design/execution-abi.md (F2),
 * docs/design/posix-subset.md. Wire addresses are uint32_t, never C pointers.
 * Research (2026-10-10):
 * https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/asm-generic/errno-base.h
 * https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/asm-generic/errno.h
 * https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/kbd.c
 * https://gcc.gnu.org/onlinedocs/gcc/Structure-Layout-Pragmas.html
 * Decision: scoped pack(4) preserves the contracted i386 alignment even in
 * host checks; no packed(1), host POSIX types or host pointer widths.
 * SPDX-License-Identifier: MIT
 * This public ABI header is MIT-licensed so that programs of any licence can
 * include it and link the SDK runtime; the kernel itself stays GPL-2.0-only.
 */
#ifndef CIUKI_ABI_H
#define CIUKI_ABI_H

#include <stdint.h>
#include <ciuki/input_abi.h>

/* f1-113: common USB/analog joystick controls on the input grant. */
#define CIUKI_JOYSTICK_VERSION 1u
#define CIUKI_GAMEPORT_FIRST 257u
#define CIUKI_JOYSTICK_LAST 264u
#define INPUT_JOY_CONTROL 0x4904u
#define CIUKI_JOY_QUERY 0u
#define CIUKI_JOY_CALIBRATE 1u
#define CIUKI_JOY_ROUTE 2u
#define CIUKI_JOY_CONFIGURE 3u
#define CIUKI_JOY_GET_ROUTE 4u
#define CIUKI_JOY_NEXT 5u
#define CIUKI_JOY_SAVE_GAME 6u /* axis=catalog game; route saved by hardware identity */
#define CIUKI_JOY_GET_GAME 8u /* axis=game; attached=0 default, 1 disabled, 2 identity, 3 missing; resolved tuple output */
#define CIUKI_DOSVM_PORT_FALLBACK 2u /* CREATE.changed; missing port or printer file; safe null/none */
#define CIUKI_DOSVM_PORT_SETTINGS_INVALID 4u /* CREATE.changed; corrupt settings used previous/default record */
#define CIUKI_JOY_SAVE_STATUS 7u /* zero request; buttons=pending, minimum=negative save error */
#define CIUKI_DOSVM_JOYSTICK_FALLBACK 1u /* CREATE.changed output only; selected source absent */
#define CIUKI_JOY_FOUR_AXIS 0u
#define CIUKI_JOY_TWO_STICKS 1u
#define CIUKI_JOY_DISABLED 0xffffffffu
#pragma pack(push, 4)
struct ciuki_joystick_control {
    uint32_t version;
    uint32_t size;
    uint32_t device;
    uint32_t generation;
    uint32_t operation;
    uint32_t axis;
    int32_t minimum;
    int32_t centre;
    int32_t maximum;
    uint32_t axes;
    uint32_t buttons;
    uint32_t attached;
    uint32_t mode;
    uint32_t second_device;
    uint32_t second_generation;
    uint32_t reserved;
    int32_t position[4];
    int32_t raw[4];
    uint32_t session; /* nonzero boot discriminator for saved explicit selections */
};
#pragma pack(pop)
_Static_assert(sizeof(struct ciuki_joystick_control)==100, "joystick control ABI");
/* CREATE reserved[1..6] are joystick device/generation/second device/
 * second generation/mode/session. Explicit selections require the current
 * nonzero boot session; stale saved locators never select a replacement.
 * Zero tuple uses the desktop default; CIUKI_JOY_DISABLED explicitly disables. */

/* f1-111: independent COM v1 records, all fields fixed-width. */
#define CIUKI_COM_VERSION 1u
#define CIUKI_COM_MAX 8u
#define CIUKI_COM_QUEUE 1024u
#define COM_GET_LINE   0x434f0101u
#define COM_SET_LINE   0x434f0102u
#define COM_GET_STATUS 0x434f0103u
#define COM_SET_MODEM  0x434f0104u
#define COM_GET_OWNER  0x434f0105u
#define COM_SET_MOUSE  0x434f0106u
#define CIUKI_COM_OWNER_NONE 0u
#define CIUKI_COM_OWNER_NATIVE 1u
#define CIUKI_COM_OWNER_VM 2u
#define CIUKI_COM_OWNER_MOUSE 3u
#define CIUKI_COM_OWNER_ENUMERATOR 4u
#define CIUKI_COM_OWNER_LOGGER 5u
#define CIUKI_SERIAL_MOUSE_OFF 0u
#define CIUKI_SERIAL_MOUSE_MICROSOFT 1u
#define CIUKI_SERIAL_MOUSE_LOGITECH 2u
#define CIUKI_SERIAL_MOUSE_SYSTEMS 3u
#define CIUKI_SERIAL_MOUSE_WHEEL 4u
struct ciuki_com_owner {
    uint32_t version;
    uint32_t size;
    uint32_t kind;
    uint32_t id;
    uint32_t generation;
    uint32_t reserved;
};
struct ciuki_com_mouse {
    uint32_t version;
    uint32_t size;
    uint32_t protocol;
    uint32_t reserved;
};
enum ciuki_com_chip { CIUKI_COM_UNKNOWN, CIUKI_COM_8250, CIUKI_COM_16450,
    CIUKI_COM_16550, CIUKI_COM_16550A, CIUKI_COM_16650, CIUKI_COM_16750 };
enum ciuki_com_origin { CIUKI_COM_BDA = 1, CIUKI_COM_PNP, CIUKI_COM_PCI };
enum ciuki_com_parity { CIUKI_COM_PARITY_NONE, CIUKI_COM_PARITY_ODD,
    CIUKI_COM_PARITY_EVEN, CIUKI_COM_PARITY_MARK, CIUKI_COM_PARITY_SPACE };
#define CIUKI_COM_DTR 1u
#define CIUKI_COM_RTS 2u
#define CIUKI_COM_BREAK 4u
#define CIUKI_COM_LOGGER 1u
#define CIUKI_COM_OPEN 2u
#define CIUKI_COM_READY 4u
#define CIUKI_COM_QUARANTINED 8u
/* errors use LSR bits 1..4, modem uses MSR bits 0..7 */
struct ciuki_com_line {
    uint32_t version;
    uint32_t size;
    uint32_t baud;
    uint32_t data_bits;
    uint32_t stop_bits;
    uint32_t parity;
    uint32_t rx_trigger;
    uint32_t read_timeout_ms;
    uint32_t write_timeout_ms;
    uint32_t reserved;
};
struct ciuki_com_modem {
    uint32_t version;
    uint32_t size;
    uint32_t outputs;
    uint32_t reserved;
};
struct ciuki_com_status {
    uint32_t version;
    uint32_t size;
    uint32_t generation;
    uint32_t number;
    uint32_t base;
    uint32_t irq;
    uint32_t origin;
    uint32_t chip;
    uint32_t fifo_depth;
    uint32_t flags;
    uint32_t modem;
    uint32_t errors;
    uint32_t rx_queued;
    uint32_t tx_queued;
    uint32_t rx_capacity;
    uint32_t tx_capacity;
    uint32_t received;
    uint32_t transmitted;
    uint32_t dropped;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ciuki_com_line) == 40, "COM line v1");
_Static_assert(sizeof(struct ciuki_com_modem) == 16, "COM modem v1");
_Static_assert(sizeof(struct ciuki_com_status) == 80, "COM status v1");

/* f1-114: exclusive parallel-port ABI; peripheral-driver-coverage.md. */
#define CIUKI_LPT_VERSION 1u
#define CIUKI_LPT_MAX 8u
#define CIUKI_LPT_CHUNK 256u
#define LPT_GET_CONFIG 0x4c500101u
#define LPT_SET_CONFIG 0x4c500102u
#define LPT_GET_STATUS 0x4c500103u
#define LPT_INIT 0x4c500104u
#define CIUKI_LPT_SPP 0u
#define CIUKI_LPT_PS2 1u
#define CIUKI_LPT_EPP 2u
#define CIUKI_LPT_ECP 3u
#define CIUKI_LPT_BDA 1u
#define CIUKI_LPT_PNP 2u
#define CIUKI_LPT_PCI 3u
#define CIUKI_LPT_CAP_SPP 1u
#define CIUKI_LPT_CAP_PS2 2u
#define CIUKI_LPT_CAP_EPP 4u
#define CIUKI_LPT_CAP_ECP 8u
#define CIUKI_LPT_CAP_DMA 16u
#define CIUKI_LPT_READY 1u
#define CIUKI_LPT_OPEN 2u
#define CIUKI_LPT_VM 4u
#define CIUKI_LPT_RAW 8u
#define CIUKI_LPT_QUARANTINED 16u
#define CIUKI_LPT_NO_DMA 0xffffffffu
struct ciuki_lpt_config {
    uint32_t version;
    uint32_t size;
    uint32_t mode;
    uint32_t reverse;
    uint32_t timeout_ms;
    uint32_t dma;
    uint32_t address;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ciuki_lpt_config) == 32, "LPT v1 ciuki_lpt_config");
struct ciuki_lpt_init {
    uint32_t version;
    uint32_t size;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ciuki_lpt_init) == 12, "LPT v1 ciuki_lpt_init");
struct ciuki_lpt_status {
    uint32_t version;
    uint32_t size;
    uint32_t generation;
    uint32_t number;
    uint32_t base;
    uint32_t base_hi;
    uint32_t origin;
    uint32_t capabilities;
    uint32_t flags;
    uint32_t status;
    uint32_t transferred;
    uint32_t fifo_depth;
    uint32_t dma_channel;
    uint32_t owner_id;
    uint32_t owner_generation;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ciuki_lpt_status) == 64, "LPT v1 ciuki_lpt_status");

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
#define ENOTSOCK 88
#define CIUKI_ENOTSOCK ENOTSOCK
#define EDESTADDRREQ 89
#define CIUKI_EDESTADDRREQ EDESTADDRREQ
#define EPROTOTYPE 91
#define CIUKI_EPROTOTYPE EPROTOTYPE
#define ENOPROTOOPT 92
#define CIUKI_ENOPROTOOPT ENOPROTOOPT
#define EPROTONOSUPPORT 93
#define CIUKI_EPROTONOSUPPORT EPROTONOSUPPORT
#define ESOCKTNOSUPPORT 94
#define CIUKI_ESOCKTNOSUPPORT ESOCKTNOSUPPORT
#define EAFNOSUPPORT 97
#define CIUKI_EAFNOSUPPORT EAFNOSUPPORT
#define EADDRINUSE 98
#define CIUKI_EADDRINUSE EADDRINUSE
#define EADDRNOTAVAIL 99
#define CIUKI_EADDRNOTAVAIL EADDRNOTAVAIL
#define ENETDOWN 100
#define CIUKI_ENETDOWN ENETDOWN
#define ENETUNREACH 101
#define CIUKI_ENETUNREACH ENETUNREACH
#define ECONNABORTED 103
#define CIUKI_ECONNABORTED ECONNABORTED
#define ECONNRESET 104
#define CIUKI_ECONNRESET ECONNRESET
#define ENOBUFS 105
#define CIUKI_ENOBUFS ENOBUFS
#define EISCONN 106
#define CIUKI_EISCONN EISCONN
#define ENOTCONN 107
#define CIUKI_ENOTCONN ENOTCONN
#define ECONNREFUSED 111
#define CIUKI_ECONNREFUSED ECONNREFUSED
#define EHOSTUNREACH 113
#define CIUKI_EHOSTUNREACH EHOSTUNREACH
#define EALREADY 114
#define CIUKI_EALREADY EALREADY
#define EINPROGRESS 115
#define CIUKI_EINPROGRESS EINPROGRESS
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
/* channel_send delivery hints. Only the owner of an input grant may use
 * these. No priority elevation is exposed to ordinary clients. */
#define CIUKI_CHANNEL_FOCUS 2u
#define CIUKI_CHANNEL_UNFOCUS 4u
#define CIUKI_CHANNEL_INPUT 8u
#define CIUKI_INPUT_KEY 1
#define CIUKI_INPUT_TEXT 2
#define CIUKI_INPUT_MOTION 3
#define CIUKI_INPUT_BUTTON 4
#define CIUKI_INPUT_RESYNC 5
#define CIUKI_INPUT_WHEEL 6
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
    CIUKI_SYS_DEVCTL = 69,
    CIUKI_SYS_PROC_LIST = 70,
    CIUKI_SYS_PROC_TERMINATE = 71,
    CIUKI_SYS_DOSVM = 72,
    CIUKI_SYS_POWER = 73,
    CIUKI_SYS_DISPLAY = 74,
    CIUKI_SYS_LAUNCH_MARK = 75,
    CIUKI_SYS_DEVICES = 76,
    CIUKI_SYS_DISPLAY_ACCEL = 77,
    CIUKI_SYS_SOCKET = 78,
    CIUKI_SYS_BIND = 79,
    CIUKI_SYS_CONNECT = 80,
    CIUKI_SYS_LISTEN = 81,
    CIUKI_SYS_ACCEPT = 82,
    CIUKI_SYS_SENDTO = 83,
    CIUKI_SYS_RECVFROM = 84,
    CIUKI_SYS_SHUTDOWN = 85,
    CIUKI_SYS_SOCKNAME = 86,
    CIUKI_SYS_SOCKOPT = 87,
    CIUKI_SYS_POLL = 88,
    CIUKI_SYS_RESOLVE4 = 89,
    CIUKI_SYS_NETCTL = 90,
    CIUKI_SYS_GETENTROPY = 91,
    /* G1 TIME=97 is preserved; G6 follows the highest allocated syscall. */
    CIUKI_SYS_TIME = 97,
    CIUKI_SYS_FILE_INFO = 98,
    CIUKI_SYS_RECOVERY = 99,
    CIUKI_SYS_NET_INFO = 100,
};
/* f5-03a: additive Win32 family, outside the native numbering (78-95 stay
 * free for F6). Only Win32-personality processes may use it. */
enum ciuki_w32_syscall { CIUKI_SYS_W32 = 96 };

/* f2-39: diagnostic markers, no copied wire record. See execution-abi.md. */
#define CIUKI_LAUNCH_STATUS 0u
#define CIUKI_LAUNCH_BEGIN 1u
#define CIUKI_LAUNCH_CONFIGURE 2u
#define CIUKI_LAUNCH_DAMAGE 3u
#define CIUKI_LAUNCH_FRAME 4u

/* pack(4) is scoped and restores the includer's previous setting. */
#pragma pack(push, 4)

/* F2-41: read-only, fixed copied device inventory (device-manager.md). */
#define CIUKI_DEVICES_VERSION 2u
#define CIUKI_DEVICE_ACCELERATOR_ID 0x30010u
#define CIUKI_DEVICES_MAX 64u
#define CIUKI_DEVICE_RESOURCES 8u
#define CIUKI_DRIVERS_MAX 16u
#define CIUKI_LEASES_MAX 64u
#define CIUKI_DEVICES_TRUNCATED 1u
#define CIUKI_RESOURCE_UNKNOWN_SIZE 1u
#define CIUKI_DEVICE_SYSTEM 0u
#define CIUKI_DEVICE_DISPLAY 1u
#define CIUKI_DEVICE_KEYBOARD 2u
#define CIUKI_DEVICE_MOUSE 3u
#define CIUKI_DEVICE_DISK 4u
#define CIUKI_DEVICE_CDROM 5u
#define CIUKI_DEVICE_SOUND 6u
#define CIUKI_DEVICE_NETWORK 7u
#define CIUKI_DEVICE_PORT 8u
#define CIUKI_DEVICE_CLASSES 9u
#define CIUKI_DEVICE_READY 0u
#define CIUKI_DEVICE_ABSENT 1u
#define CIUKI_DEVICE_QUARANTINED 2u
#define CIUKI_DEVICE_FIRMWARE_FIRST 3u
#define CIUKI_BUS_LEGACY 0u
#define CIUKI_BUS_PCI 1u
#define CIUKI_BUS_LOADER 2u
#define CIUKI_BUS_USB 3u
#define CIUKI_RESOURCE_IO 0u
#define CIUKI_RESOURCE_IRQ 1u
#define CIUKI_RESOURCE_DMA 2u
#define CIUKI_RESOURCE_MEMORY 3u
struct ciuki_device_resource {
    uint32_t type;
    uint32_t flags;
    uint64_t start;
    uint64_t end;
};
struct ciuki_device_record {
    uint32_t id;
    uint32_t class_id;
    uint32_t bus_type;
    uint32_t bus;
    uint32_t slot;
    uint32_t function;
    uint32_t vendor;
    uint32_t device;
    uint32_t sub_vendor;
    uint32_t sub_device;
    uint32_t pci_class;
    uint32_t subclass;
    uint32_t prog_if;
    uint32_t revision;
    uint32_t state;
    uint32_t activation_seq;
    uint32_t generation;
    uint32_t resource_count;
    uint32_t flags;
    char name[64];
    char driver[32];
    char reason[80];
    struct ciuki_device_resource resources[8];
    char driver_display_name[48];
    uint32_t accel_ready;
    uint32_t native_used;
    uint32_t cpu_ready;
};
struct ciuki_driver_record {
    char name[32];
    char build[32];
    char policy[64];
    uint32_t bound_devices;
    uint32_t state;
};
struct ciuki_device_lease {
    char owner[32];
    uint32_t type;
    uint32_t state;
    uint32_t generation;
    uint64_t start;
    uint64_t end;
};
struct ciuki_devices_info {
    uint32_t version;
    uint32_t size;
    uint32_t count;
    uint32_t flags;
    uint32_t generation;
    uint32_t driver_count;
    uint32_t lease_count;
    uint32_t boot_flags;
    uint32_t input_policy;
    uint32_t cpu_family;
    uint32_t cpu_model;
    uint32_t cpu_stepping;
    uint64_t uptime_ms;
    char cpu_vendor[16];
    char cpu_brand[64];
    char boot_options[128];
    struct ciuki_device_record devices[64];
    struct ciuki_driver_record drivers[16];
    struct ciuki_device_lease leases[64];
};

/* f9-07: retained hardware identity, additive call 76 version 3. */
#define CIUKI_HARDWARE_VERSION 3u
struct ciuki_hardware_info {
    uint32_t version;
    uint32_t size;
    uint32_t memmap_source;
    uint32_t memmap_count;
    uint32_t cpuid_ecx;
    uint32_t cpuid_edx;
    uint32_t smbios_valid;
    uint64_t memory_bytes[6]; /* index 0 unknown, 1 usable, 2 reserved, 3 reclaim, 4 NVS, 5 bad */
    char vendor[48];
    char product[48];
    char bios_vendor[48];
    char bios_version[48];
    char bios_date[48];
    char build_id[64];
    uint32_t log_active;
    uint32_t log_position;
    uint32_t log_pending_bytes;
    uint32_t log_dropped;
    uint32_t log_ring_start;
    uint32_t log_limit;
    char log_pending[4096];
};

/* Read-only /dev/net carrier snapshot. IP configuration is reserved for lwIP.
 * Initialize version/size and zero reserved fields before NET_GET_STATE. */
#define CIUKI_NET_VERSION 1u
#define NET_GET_STATE 0x4301u
#define NET_QUERY_DRIVER_PACK 0x4302u
#define CIUKI_DRIVER_PACK_VERSION 1u
/* Read-only compiled qualification and retained-host observation. No enable
 * authority: boot admission always rechecks the exact installed manifest. */
struct ciuki_driver_pack_query {
    uint32_t version;
    uint32_t size;
    uint32_t bus;
    uint32_t slot;
    uint32_t function;
    uint32_t vendor;
    uint32_t device;
    uint32_t sub_vendor;
    uint32_t sub_device;
    uint32_t revision;
    uint8_t binary_hash[32];
    uint8_t package_seal[32];
    uint32_t qualified;
    uint32_t busy;
    uint32_t reserved[2];
};
#define CIUKI_NET_LINK_UNKNOWN 0u
#define CIUKI_NET_LINK_DOWN 1u
#define CIUKI_NET_LINK_UP 2u
#define CIUKI_NET_STOPPED 0u
#define CIUKI_NET_RUNNING 1u
#define CIUKI_NET_QUARANTINED 2u
struct ciuki_net_state {
    uint32_t version;
    uint32_t size;
    uint32_t present;
    char driver[16];
    uint8_t mac[6];
    uint8_t reserved0[2];
    uint32_t link;
    uint32_t state;
    char reason[24];
    uint32_t reserved1;
    uint64_t rx_frames;
    uint64_t tx_frames;
    uint32_t address_configured;
    uint8_t ipv4[4];
};
_Static_assert(sizeof(struct ciuki_net_state) == 96, "network state ABI");

/* Syscall 90: ebx=operation, ecx=pointer, edx=sizeof(record), others zero.
 * All operations require the supervisor-delegated network capability. IPv4
 * words are in network byte order. Speed 0 / duplex 0 select auto negotiation;
 * forced modes require speed 10 or 100 and duplex 1 (half) or 2 (full).
 * GET enumerates index [0,count); mutations require the returned generation.
 * SET persists the complete validated config.
 * DHCP renew/release are transient. No user pointers occur in these records. */
#define CIUKI_NETCTL_VERSION 1u
#define CIUKI_NETCTL_GET 0u
#define CIUKI_NETCTL_SET 1u
#define CIUKI_NETCTL_RENEW 2u
#define CIUKI_NETCTL_RELEASE 3u
#define CIUKI_NET_MEDIA_AUTO 1u
#define CIUKI_NET_MEDIA_10 2u
#define CIUKI_NET_MEDIA_100 4u
struct ciuki_net_config {
    uint32_t enabled;
    uint32_t dhcp;
    uint32_t speed;
    uint32_t duplex;
    uint32_t address;
    uint32_t mask;
    uint32_t gateway;
    uint32_t dns[3];
    char hostname[64];
    char domain[128];
};
struct ciuki_net_control {
    uint32_t version;
    uint32_t size;
    uint32_t index;
    uint32_t count;
    struct ciuki_net_config config;
    char name[8];
    char driver[16];
    uint8_t mac[6];
    uint8_t reserved0[2];
    uint32_t pci_vendor;
    uint32_t pci_device;
    uint32_t irq;
    uint32_t io_base;
    uint32_t mmio_base;
    uint32_t link;
    uint32_t state;
    uint32_t media_caps;
    uint32_t speed;
    uint32_t duplex;
    uint32_t address;
    uint32_t mask;
    uint32_t gateway;
    uint32_t dns[3];
    uint32_t lease_seconds;
    uint32_t lease_remaining;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint32_t generation;
    uint32_t reserved[3];
};
_Static_assert(sizeof(struct ciuki_net_config) == 232, "network configuration ABI");
_Static_assert(sizeof(struct ciuki_net_control) == 400, "network control ABI");

/* f9-04b: DEVCTL on a retained FAT file/directory description. Setup mode
 * and setup_capability are mandatory. INDEX_NONE queries metadata only.
 * Continuations must echo mount_generation and chain_generation. A changed
 * mount/chain returns EAGAIN; EOF is ENOENT. lba is an absolute disk sector.
 * ATTRIB preserves the directory/volume bits and never changes file data. */
#define CIUKI_SETUP_FAT_VERSION 1u
#define CIUKI_SETUP_FAT_QUERY 0x53540001u
#define CIUKI_SETUP_FAT_ATTRIB 0x53540002u
#define CIUKI_SETUP_CLOCK_SET 0x53540003u
#define CIUKI_SETUP_NET_QUERY 0x53540004u
#define CIUKI_SETUP_INDEX_NONE 0xffffffffu
struct ciuki_setup_fat {
    uint32_t version;
    uint32_t size;
    uint32_t mount_generation;
    uint32_t attributes;
    uint32_t cluster_index;
    uint32_t cluster_sectors;
    uint64_t file_size;
    uint64_t chain_generation;
    uint64_t lba;
    char name[260];
    char alias[40];
};
/* SET is authorized by the same capability; seconds is UTC, offset is east
 * of UTC. rtc_local is 0/1. The persistent convention is written by the
 * shared configuration writer before this call. */
struct ciuki_setup_clock {
    uint32_t version;
    uint32_t size;
    int64_t seconds;
    int32_t offset_minutes;
    uint32_t rtc_local;
};
/* Read-only physical identities for f2-100 NETWORK.INI, at most two NICs. */
struct ciuki_setup_net_identity {
    uint32_t vendor;
    uint32_t device;
    uint8_t mac[6];
    uint8_t reserved[2];
};
struct ciuki_setup_net {
    uint32_t version;
    uint32_t size;
    uint32_t count;
    uint32_t reserved;
    struct ciuki_setup_net_identity identity[2];
};

/* G1: system time zone and clock, syscall 97 (CIUKI_SYS_TIME): EBX=record,
 * ECX=exact size; 0 or -errno. GET is open to every process and returns the
 * live state. SET_ZONE, SET_LOCAL, SET_UTC and ACKNOWLEDGE need the time
 * authority (desktop and Control Panel, delegated like the network one):
 * EPERM otherwise. The kernel keeps UTC; the RTC holds local time when
 * RTC_LOCAL (the Windows convention) and is rewritten at every change of
 * zone, clock or daylight-saving period. offset_minutes are east of UTC.
 * rule is a POSIX TZ string (ciuki/tzrule.h); zone an IANA identifier. */
#define CIUKI_TIME_VERSION 1u
#define CIUKI_TIME_GET 0u
#define CIUKI_TIME_SET_ZONE 1u
#define CIUKI_TIME_SET_LOCAL 2u
#define CIUKI_TIME_SET_UTC 3u
#define CIUKI_TIME_ACKNOWLEDGE 4u
#define CIUKI_TIME_AUTO_DST 1u
#define CIUKI_TIME_RTC_LOCAL 2u
#define CIUKI_TIME_DAYLIGHT 4u
#define CIUKI_TIME_ADJUSTED 8u
#define CIUKI_TIME_AUTHORITY 16u
#define CIUKI_TIME_ZONE_MAX 48u
#define CIUKI_TIME_RULE_MAX 64u
#define CIUKI_TIME_NAME_MAX 16u
struct ciuki_time_zone {
    uint32_t version;
    uint32_t size;
    uint32_t operation;
    uint32_t flags;
    int64_t utc_seconds;
    int64_t local_seconds;
    uint32_t milliseconds;
    int32_t offset_minutes;
    int32_t standard_minutes;
    int32_t daylight_minutes;
    int32_t rtc_bias_minutes;
    uint32_t generation;
    int64_t next_change;
    char zone[48];
    char rule[64];
    char standard_name[16];
    char daylight_name[16];
};
_Static_assert(sizeof(struct ciuki_time_zone) == 208, "time zone ABI");


/* /dev/media: v2 adds per-drive types; v1 remains accepted at its old size.
 * removable_mask includes ejectable USB and optical media; type[] distinguishes them.
 * A removal names a generation and ejects the physical USB device or CD. */
#define CIUKI_MEDIA_VERSION 2u
#define CIUKI_MEDIA_VERSION_1 1u
#define CIUKI_MEDIA_NONE 0u
#define CIUKI_MEDIA_USB 1u
#define CIUKI_MEDIA_OPTICAL 2u
/* Retained successful USB removal: absent from removable_mask, same generation. */
#define CIUKI_MEDIA_SAFE_REMOVED 3u
#define MEDIA_GET_STATE 0x4d01u
#define MEDIA_SAFE_REMOVE 0x4d02u
#define MEDIA_GET_BLOCKERS 0x4d04u
struct ciuki_media_program {
    uint32_t pid;
    char name[32];
};
struct ciuki_media_blockers {
    uint32_t version;
    uint32_t size;
    uint32_t drive;
    uint32_t generation;
    uint32_t count;
    struct ciuki_media_program programs[64];
};
_Static_assert(sizeof(struct ciuki_media_blockers)==2324, "media blockers ABI");
struct ciuki_media_state_v1 {
    uint32_t version;
    uint32_t size;
    uint32_t removable_mask;
    uint32_t reserved;
    uint32_t generation[26];
};
/* F2-51: independent per-letter arrival metadata; safe-removal ABI is unchanged. */
#define MEDIA_GET_VOLUME 0x4d03u
#define CIUKI_MEDIA_REMOVABLE 1u /* ciuki_media_volume.flags: removable FAT volume */
struct ciuki_media_volume {
    uint32_t version;
    uint32_t size;
    uint32_t drive;
    uint32_t flags;
    uint32_t generation;
    char id[40];                 /* stable filesystem identity; empty if unavailable */
    char label[33];
    uint8_t reserved[3];
};
_Static_assert(sizeof(struct ciuki_media_volume) == 96, "media volume ABI");
/* fx-21b: usb_unrecognized (formerly reserved; zero on input) counts the USB
 * devices quarantined at enumeration since boot. The desktop shows "USB
 * Device Not Recognized" once for every increase. */
struct ciuki_media_state {
    uint32_t version;
    uint32_t size;
    uint32_t removable_mask;
    uint32_t usb_unrecognized;
    uint32_t generation[26];
    uint32_t type[26];
};
struct ciuki_media_remove {
    uint32_t version;
    uint32_t size;
    uint32_t drive;
    uint32_t generation;
};

/* fx-17b: floppy drives A:/B: report type CIUKI_MEDIA_FLOPPY whenever the
 * controller found the drive; removable_mask has the bit only while a
 * diskette is mounted. MEDIA_FLOPPY_LOAD (request: ciuki_media_remove with
 * generation 0) reads the diskette in the drive (Retry); ENXIO: no disk. */
#define CIUKI_MEDIA_FLOPPY 4u
#define MEDIA_FLOPPY_LOAD 0x4d05u
/* fx-17b: volume space for Properties. free_clusters is exact unless
 * CIUKI_MEDIA_SPACE_COUNTING: the free count is still being computed in the
 * background (FAT32 FSInfo unknown) and free_clusters is its upper bound. */
#define MEDIA_GET_SPACE 0x4d06u
#define CIUKI_MEDIA_SPACE_COUNTING 1u
struct ciuki_media_space {
    uint32_t version;
    uint32_t size;
    uint32_t drive;
    uint32_t flags;
    uint32_t cluster_bytes;
    uint32_t total_clusters;
    uint32_t free_clusters;
    uint32_t reserved;
};
/* G6: volume label of a mounted FAT drive (C: included). GET fills label
 * (no padding, "" when none) and flags; SET writes the root-directory
 * volume-ID entry and the boot sector BS_VolLab (FAT spec 1.03, pp. 9-12, 25;
 * Win32 SetVolumeLabel). Label: at most 11 bytes of printable ASCII, stored
 * upper case; * ? . , ; : / \ | + = < > [ ] " refused with EINVAL; "" removes
 * the label. EROFS on a read-only, write-protected or optical medium;
 * ENOENT without a mounted FAT volume. reserved must be zero. */
/* Appended after MEDIA_CD_POSITION=0x4d17; existing values stay fixed. */
#define MEDIA_GET_LABEL 0x4d18u
#define MEDIA_SET_LABEL 0x4d19u
#define CIUKI_MEDIA_LABEL_READ_ONLY 1u
#define CIUKI_MEDIA_LABEL_MAX 11u
struct ciuki_media_label {
    uint32_t version;
    uint32_t size;
    uint32_t drive;
    uint32_t flags;
    char label[12];
    uint32_t reserved;
};
_Static_assert(sizeof(struct ciuki_media_label) == 32, "media label ABI");
/* G29: mounted removable FAT only; generation is the physical medium's
 * MEDIA_GET_STATE generation. GET fills filesystem (12/16/32), sectors,
 * cluster_bytes, supported_filesystems and label. FORMAT accepts family,
 * flags and label; the other output fields are ignored and recomputed.
 * Full (flags=0) scans sectors, Quick recreates metadata without a scan.
 * Both destroy files. reserved must be zero; fixed/busy/stale media refuse.
 * Partitioned volumes retain their FAT family; no system files are copied. */
#define MEDIA_GET_FORMAT 0x4d1au
#define MEDIA_FORMAT 0x4d1bu
#define CIUKI_FORMAT_QUICK 1u
#define CIUKI_FORMAT_FAT12 1u
#define CIUKI_FORMAT_FAT16 2u
#define CIUKI_FORMAT_FAT32 4u
struct ciuki_media_format {
    uint32_t version;
    uint32_t size;
    uint32_t drive;
    uint32_t generation;
    uint32_t filesystem;
    uint32_t flags;
    uint32_t sectors;
    uint32_t cluster_bytes;
    uint32_t supported_filesystems;
    char label[12];
    uint32_t reserved;
};
_Static_assert(sizeof(struct ciuki_media_format) == 52, "media format ABI");
_Static_assert(sizeof(struct ciuki_media_space) == 32, "media space ABI");
_Static_assert(sizeof(struct ciuki_media_state_v1) == 120, "media state v1 ABI");
_Static_assert(sizeof(struct ciuki_media_state) == 224, "media state ABI");
_Static_assert(sizeof(struct ciuki_media_remove) == 16, "media removal ABI");

/* f5-12B2: CD audio on /dev/media, addressed by optical slot (an audio-only
 * disc has no drive letter). Every request carries version 1 and its own
 * size; a request that names a generation is refused with EAGAIN when the
 * drive's media_generation differs (re-read MEDIA_CD_STATE/TOC first).
 * Errors: ENODEV no optical drive in the slot; ENXIO no medium (sense 2/3A);
 * EINVAL range or data track (never sent to the drive); EIO one failed
 * packet (never retried by the kernel); EBUSY door open refused while the
 * disc is playing or has open mounted handles; EOPNOTSUPP missing capability. */
#define CIUKI_MEDIA_CD_VERSION 1u
#define MEDIA_CD_STATE 0x4d10u
#define MEDIA_CD_INFO 0x4d11u
#define MEDIA_CD_TOC 0x4d12u
#define MEDIA_CD_READ_AUDIO 0x4d13u
#define MEDIA_CD_TRAY 0x4d14u
#define MEDIA_CD_PLAY 0x4d15u
#define MEDIA_CD_PAUSE 0x4d16u
#define MEDIA_CD_POSITION 0x4d17u
#define CIUKI_CD_SECTOR_BYTES 2352u     /* IEC 60908 CD-DA sector: 588 stereo S16LE frames */
#define CIUKI_CD_FRAMES_PER_SECOND 75u
#define CIUKI_CD_READ_MAX_SECTORS 27u   /* 63,504 bytes, inside the 64 KiB ATAPI packet limit */
#define CIUKI_CD_TRACKS_MAX 99u
/* MEDIA_CD_STATE.state */
#define CIUKI_CD_NO_MEDIUM 0u
#define CIUKI_CD_NOT_READY 1u
#define CIUKI_CD_READY 2u
/* MEDIA_CD_INFO.capabilities (MODE SENSE(10) page 2Ah, MMC-3 6.3.11 / SFF-8020i 10.8.6.4) */
#define CIUKI_CD_CAP_AUDIO_PLAY 1u      /* byte 4 bit 0: analog Audio Play */
#define CIUKI_CD_CAP_CDDA_READ 2u       /* byte 5 bit 0: CD-DA Commands Supported (READ CD) */
#define CIUKI_CD_CAP_CDDA_ACCURATE 4u   /* byte 5 bit 1: CD-DA Stream is Accurate */
#define CIUKI_CD_CAP_EJECT 8u           /* byte 6 bit 3: eject through START STOP UNIT LoEj */
#define CIUKI_CD_CAP_SEPARATE_VOLUME 16u /* byte 7 bit 0 */
#define CIUKI_CD_CAP_REPORTED 32u       /* page 2Ah was read; without it the bits are unknown */
/* MEDIA_CD_POSITION.audio_status (READ SUB-CHANNEL header, MMC-3 Table 219) */
#define CIUKI_CD_AUDIO_INVALID 0x00u
#define CIUKI_CD_AUDIO_PLAYING 0x11u
#define CIUKI_CD_AUDIO_PAUSED 0x12u
#define CIUKI_CD_AUDIO_COMPLETED 0x13u
#define CIUKI_CD_AUDIO_ERROR 0x14u
#define CIUKI_CD_AUDIO_NONE 0x15u
/* MEDIA_CD_TRAY.action */
#define CIUKI_CD_TRAY_OPEN 1u
#define CIUKI_CD_TRAY_CLOSE 2u
/* MEDIA_CD_PAUSE.action (analog path): PAUSE/RESUME 4Bh with Resume = 0 or 1,
 * and STOP PLAY/SCAN 4Eh (MMC-3 5.10, 5.37). */
#define CIUKI_CD_ACTION_PAUSE 0u
#define CIUKI_CD_ACTION_RESUME 1u
#define CIUKI_CD_ACTION_STOP 2u
struct ciuki_media_cd_state {
    uint32_t version;
    uint32_t size;
    uint32_t slot;
    uint32_t present;        /* 1: an ATAPI drive is bound to this slot */
    uint32_t state;          /* CIUKI_CD_* readiness of the medium */
    uint32_t generation;     /* drive media_generation */
    uint32_t audio_ready;    /* 1: first track audio: ready for CD audio, never mounted */
    uint32_t mounted;        /* 1: a data session is mounted as a drive letter */
    uint32_t drive;          /* letter index when mounted, else 26 */
    uint32_t reserved[3];
};
struct ciuki_media_cd_info {
    uint32_t version;
    uint32_t size;
    uint32_t slot;
    uint32_t generation;
    uint32_t capabilities;   /* CIUKI_CD_CAP_* */
    uint32_t volume_levels;  /* page 2Ah bytes 10-11 */
    uint32_t packet_bytes;   /* 12 or 16 */
    uint32_t max_read_sectors; /* CIUKI_CD_READ_MAX_SECTORS */
    char model[41];
    uint8_t reserved[3];
};
struct ciuki_media_cd_track {
    uint8_t number;
    uint8_t control;
    uint8_t adr;
    uint8_t reserved;
    uint32_t start_lba;
};
struct ciuki_media_cd_toc {
    uint32_t version;
    uint32_t size;
    uint32_t slot;
    uint32_t generation;
    uint32_t first;
    uint32_t last;    /* track numbers, 1..99 */
    uint32_t count;          /* descriptors filled, excluding the lead-out */
    uint32_t leadout_lba;
    struct ciuki_media_cd_track tracks[CIUKI_CD_TRACKS_MAX];
};
struct ciuki_media_cd_read {
    uint32_t version;
    uint32_t size;
    uint32_t slot;
    uint32_t generation;
    uint32_t lba;
    uint32_t sectors;   /* 1..CIUKI_CD_READ_MAX_SECTORS, inside one audio track */
    uint32_t buffer;         /* user address of sectors * 2352 bytes */
    uint32_t done;           /* bytes copied out */
};
struct ciuki_media_cd_tray {
    uint32_t version;
    uint32_t size;
    uint32_t slot;
    uint32_t generation;
    uint32_t action;
};
struct ciuki_media_cd_play {
    uint32_t version;
    uint32_t size;
    uint32_t slot;
    uint32_t generation;
    uint32_t start_lba;
    uint32_t end_lba; /* analog PLAY AUDIO MSF 47h; [start, end) inside audio tracks */
};
struct ciuki_media_cd_pause {
    uint32_t version;
    uint32_t size;
    uint32_t slot;
    uint32_t generation;
    uint32_t action;         /* CIUKI_CD_ACTION_* */
    uint32_t reserved;
};
struct ciuki_media_cd_position {
    uint32_t version;
    uint32_t size;
    uint32_t slot;
    uint32_t generation;
    uint32_t audio_status;   /* CIUKI_CD_AUDIO_* */
    uint32_t track;
    uint32_t index;
    uint32_t absolute_lba;
    uint32_t relative_lba;
    uint32_t reserved[3];
};
_Static_assert(sizeof(struct ciuki_media_cd_state) == 48, "media cd state ABI");
_Static_assert(sizeof(struct ciuki_media_cd_info) == 76, "media cd info ABI");
_Static_assert(sizeof(struct ciuki_media_cd_toc) == 32 + 8 * 99, "media cd toc ABI");
_Static_assert(sizeof(struct ciuki_media_cd_read) == 32, "media cd read ABI");
_Static_assert(sizeof(struct ciuki_media_cd_tray) == 20, "media cd tray ABI");
_Static_assert(sizeof(struct ciuki_media_cd_play) == 24, "media cd play ABI");
_Static_assert(sizeof(struct ciuki_media_cd_pause) == 24, "media cd pause ABI");
_Static_assert(sizeof(struct ciuki_media_cd_position) == 48, "media cd position ABI");

/* f9-02: raw whole-disk nodes /dev/ata0../dev/ata3 (slot = channel * 2 +
 * unit) and the destination-disk lease group 0x4401..0x440F (lead reservation,
 * cd-setup-plan.md "Lead decisions"; 0x4409..0x440F stay reserved). An absent
 * or ATAPI slot fails open with ENXIO. Only pread/pwrite move data: offset
 * and length are multiples of 512, one call transfers at most 65536 bytes
 * (128 sectors) and may return short at that bound. Reads, DISK_GET_INFO and
 * DISK_LEASE_STATE are open to every process. pwrite, fsync and every other
 * DISK_* request need the Setup capability of the CALLING process (EPERM),
 * checked on each call (a descriptor inherited or duplicated into another
 * process is read-only there), and the lease held by that process (EACCES;
 * DISK_LEASE_ACQUIRE itself refuses EBUSY while the disk is in use). While a
 * volume is published, raw writes inside its partition are refused (EBUSY).
 * Each record starts with version 1 and its own size; reserved fields are 0. */
#define CIUKI_DISK_VERSION 1u
#define DISK_GET_INFO 0x4401u       /* struct ciuki_disk_info (out) */
#define DISK_LEASE_ACQUIRE 0x4402u  /* struct ciuki_disk_info (identity out) */
#define DISK_LEASE_RELEASE 0x4403u  /* struct ciuki_disk_request */
#define DISK_FLUSH 0x4404u          /* struct ciuki_disk_request; fsync is the same */
#define DISK_PUBLISH 0x4405u        /* struct ciuki_disk_lease (drive, reasons out) */
#define DISK_LEASE_STATE 0x4406u    /* struct ciuki_disk_lease (out) */
#define DISK_SET_PHASE 0x4407u      /* struct ciuki_disk_lease (phase in) */
#define DISK_RECHECK 0x4408u        /* struct ciuki_disk_recheck */
#define CIUKI_DISK_SLOTS 4u
#define CIUKI_DISK_SECTOR 512u
#define CIUKI_DISK_IO_MAX 65536u
#define CIUKI_DISK_LBA28 (1ull << 28)
/* ciuki_disk_info.flags */
#define CIUKI_DISK_PRESENT 1u
#define CIUKI_DISK_LBA48 2u
#define CIUKI_DISK_FLUSH_CACHE 4u   /* qualified FLUSH CACHE durability path */
#define CIUKI_DISK_LEASED 8u
#define CIUKI_DISK_PUBLISHED 16u
/* ciuki_disk_info.write_cache (blkdev write_cache_state) */
#define CIUKI_DISK_CACHE_UNKNOWN 0u
#define CIUKI_DISK_CACHE_DISABLED 1u
#define CIUKI_DISK_CACHE_ENABLED 2u
/* ciuki_disk_volume.fs and .flags */
#define CIUKI_DISK_FS_FAT12 12u
#define CIUKI_DISK_FS_FAT16 16u
#define CIUKI_DISK_FS_FAT32 32u
#define CIUKI_DISK_FS_NTFS 7u
#define CIUKI_DISK_VOLUME_READONLY 1u
#define CIUKI_DISK_VOLUMES_MAX 8u
/* FAT read-only reasons (fat.h enum fat_ro_reason, same values). */
#define CIUKI_FAT_RO_REQUEST 1u
#define CIUKI_FAT_RO_DURABILITY 2u
#define CIUKI_FAT_RO_DIRTY 4u
#define CIUKI_FAT_RO_IO_FLAG 8u
#define CIUKI_FAT_RO_COPIES 16u
#define CIUKI_FAT_RO_CORRUPT 32u
#define CIUKI_FAT_RO_LOST 64u
#define CIUKI_FAT_RO_WRITE_ERROR 128u
#define CIUKI_FAT_RO_ACTIVE_ONLY 256u
struct ciuki_disk_volume {
    uint32_t drive;      /* letter index, 3 = D: */
    uint32_t partition;  /* 1-based table order (primary, then logical) */
    uint32_t fs;         /* CIUKI_DISK_FS_* */
    uint32_t flags;      /* CIUKI_DISK_VOLUME_* */
    char label[12];
};
struct ciuki_disk_info {
    uint32_t version;
    uint32_t size;
    uint32_t disk;
    uint32_t flags;
    uint64_t sectors;            /* capacity in sector_size units */
    uint32_t sector_size;
    uint32_t write_cache;
    uint64_t lba_limit;          /* partitions must end at or below this LBA */
    char model[41];              /* ATA IDENTIFY words 27-46 */
    char serial[21];             /* ATA IDENTIFY words 10-19 */
    char firmware[9];            /* ATA IDENTIFY words 23-26 */
    uint8_t reserved0;
    uint32_t volume_count;
    uint32_t reserved1;
    struct ciuki_disk_volume volumes[CIUKI_DISK_VOLUMES_MAX]; /* mounted now */
};
struct ciuki_disk_request {
    uint32_t version;
    uint32_t size;
    uint32_t flags;
    uint32_t reserved;
};
/* ciuki_disk_lease.state */
#define CIUKI_DISK_LEASE_NONE 0u
#define CIUKI_DISK_LEASE_HELD 1u
#define CIUKI_DISK_LEASE_PUBLISHED 2u
#define CIUKI_DISK_LEASE_RELEASED 3u
#define CIUKI_DISK_LEASE_REVOKED 4u
struct ciuki_disk_lease {
    uint32_t version;
    uint32_t size;
    uint32_t disk;
    uint32_t state;
    uint32_t holder;
    uint32_t generation;
    uint32_t drive;              /* published letter index, 0 when none */
    uint32_t reasons;            /* CIUKI_FAT_RO_* seen by the publication mount */
    uint64_t reads;              /* raw node sectors since boot */
    uint64_t writes;             /* raw node sectors since boot */
    uint64_t flushes;            /* raw node flushes since boot */
    char phase[32];              /* free text set by DISK_SET_PHASE */
    char reason[24];             /* last refusal or release reason */
};
#define CIUKI_DISK_RECHECK_SEAL 1u  /* leave the volume clean and read-only */
struct ciuki_disk_recheck {
    uint32_t version;
    uint32_t size;
    uint32_t flags;
    uint32_t drive;              /* out */
    uint32_t reasons;
    uint32_t fat_type;
    uint32_t clusters;
    uint32_t free_clusters;
    uint32_t lost_clusters;
    uint32_t scan_passes;
    uint32_t scan_ms;
    uint32_t lfn_orphans;
    uint32_t lfn_invalid;
    uint32_t lfn_bad_checksum;
    uint32_t reserved[2];
};
_Static_assert(sizeof(struct ciuki_disk_volume) == 28, "disk volume ABI");
_Static_assert(sizeof(struct ciuki_disk_info) == 344, "disk info ABI");
_Static_assert(sizeof(struct ciuki_disk_request) == 16, "disk request ABI");
_Static_assert(sizeof(struct ciuki_disk_lease) == 112, "disk lease ABI");
_Static_assert(sizeof(struct ciuki_disk_recheck) == 64, "disk recheck ABI");

/* F4-01: fixed copied device-control extension. */
#define CIUKI_AUDIO_VERSION 1u
#define CIUKI_AUDIO_U8 1u
#define CIUKI_AUDIO_S16LE 2u
#define AUDIO_GET_CAPS 0x4101u
#define AUDIO_SET_FORMAT 0x4102u
#define AUDIO_GET_STATUS 0x4103u
#define AUDIO_SET_GAIN 0x4104u
#define AUDIO_DRAIN 0x4105u
#define AUDIO_RESET 0x4106u
#define AUDIO_REFILL_READ 0x4107u /* command: this thread's next bounded source reads */
#define AUDIO_GET_PRODUCER 0x4108u /* f1-101: the kernel's producer boost state of this stream */
#define AUDIO_CAPTURE_CAPS 0x4109u
#define AUDIO_CAPTURE_START 0x410au
#define AUDIO_CAPTURE_READ 0x410bu
#define AUDIO_CAPTURE_STOP 0x410cu
#define CIUKI_CAPTURE_MIC 0u
#define CIUKI_CAPTURE_LINE 1u
#define CIUKI_CAPTURE_FRAMES 128u
/* Open /dev/audio O_RDONLY. START is explicit and exclusive; close stops it.
 * Fixed S16LE stereo 48000 Hz, copied buffers only. Unsupported hardware
 * reports available=0, START returns EOPNOTSUPP. READ returns EAGAIN while
 * waiting for data, EIO on an overrun. Gain is software attenuation 0..100. */
struct ciuki_audio_capture {
    uint32_t version;
    uint32_t size;
    uint32_t source;
    uint32_t gain;
    uint32_t rate;
    uint32_t channels;
    uint32_t format;
    uint32_t available;
    uint32_t reserved[2];
};
struct ciuki_audio_capture_data {
    uint32_t version;
    uint32_t size;
    uint32_t frames;
    uint32_t captured;
    int16_t pcm[256]; /* CIUKI_CAPTURE_FRAMES stereo frames */
    uint32_t reserved[2];
};
_Static_assert(sizeof(struct ciuki_audio_capture)==40,"audio capture control");
_Static_assert(sizeof(struct ciuki_audio_capture_data)==536,"audio capture data");
#define MIXER_GET_MASTER 0x4201u
#define MIXER_SET_MASTER 0x4202u
#define MIXER_GET_DIAGNOSTICS 0x4203u
#define MIXER_REFRESH_MASTER 0x4204u /* f1-100: control handle; master record, one requested hardware read */
#define CIUKI_AUDIO_DIAG_READY 1u
#define CIUKI_AUDIO_DIAG_RUNNING 2u
#define CIUKI_AUDIO_DIAG_QUARANTINE 4u
#define CIUKI_AUDIO_DIAG_VM_ACTIVE 8u /* VM source busy or holding queued audio */
/* Read-only kernel observations, also available on an absent/failed backend. */
struct ciuki_audio_diagnostics {
    uint32_t version;
    uint32_t size;
    uint32_t codec_id;
    uint32_t controller;
    uint32_t flags;
    uint32_t irq_line;
    uint32_t civ;
    uint32_t picb;
    uint64_t fetched_frames;
    uint64_t irqs;
    int32_t last_error;
    uint16_t power;
    uint16_t master;
    uint16_t pcm;
    uint16_t rate;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ciuki_audio_diagnostics) == 64, "audio diagnostics ABI");
#define CIUKI_MIXER_DEFAULTS 0u
#define CIUKI_MIXER_SAVED 1u
#define CIUKI_MIXER_UNSAVED 2u
struct ciuki_audio_caps {
    uint32_t version;
    uint32_t size;
    uint32_t formats;
    uint32_t min_rate;
    uint32_t max_rate;
    uint32_t output_rate;
    uint32_t max_streams;
    uint32_t ring_frames;
    uint32_t period_frames;
    uint32_t published_periods;
    uint32_t backend;
    uint32_t generation;
    uint32_t vendor;
    uint32_t device;
    uint32_t subsystem;
    uint32_t revision;
    uint32_t codec_id;
    uint32_t drain_ms;
    uint32_t reserved[2];
};
struct ciuki_audio_format {
    uint32_t version;
    uint32_t size;
    uint32_t rate;
    uint32_t format;
    uint32_t channels;
    uint32_t output_rate;
    uint32_t reserved[2];
};
struct ciuki_audio_gain {
    uint32_t version;
    uint32_t size;
    uint32_t left;
    uint32_t right;
    uint32_t reserved[2];
};
struct ciuki_audio_command {
    uint32_t version;
    uint32_t size;
    uint32_t reserved[2];
};
/* f1-101: the mixer reports a native stream behind its target lead (more
 * than one 256-frame period below ring_frames - period_frames, in output
 * frames) and the scheduler raises the stream's writer and its refill-read
 * thread above every ordinary level until the lead is refilled, within one
 * CPU budget shared by all producers (20 % over any 100 ms). The record is a
 * read-only observation; no flag of it can be set by the application. */
#define CIUKI_AUDIO_PRODUCER_BEHIND 1u      /* lead below the producer mark now */
#define CIUKI_AUDIO_PRODUCER_BOOSTED 2u     /* behind, producers boosted now */
#define CIUKI_AUDIO_PRODUCER_OVER_BUDGET 4u /* behind, boost withheld by the budget */
struct ciuki_audio_producer {
    uint32_t version;
    uint32_t size;
    uint32_t flags;
    uint32_t boosts;      /* boost episodes since open/reset */
    uint32_t over_budget; /* episodes withheld by the budget */
    uint32_t producers;   /* registered producer threads, 0..2 */
    uint64_t boost_us;    /* producer CPU used at the boost level */
};
struct ciuki_audio_status {
    uint32_t version;
    uint32_t size;
    uint32_t queued_frames;
    uint32_t state;
    uint32_t output_rate;
    uint32_t generation;
    uint64_t accepted_frames;
    uint64_t consumed_frames;
    uint64_t output_frames;
    uint64_t fetched_frames;
    uint64_t source_starvation;
    uint64_t fifo_errors;
    uint64_t dma_halts;
    uint64_t deadline_misses;
    int32_t last_error;
    uint32_t stream_generation;
    uint32_t max_service_us;
    uint32_t max_irq_us;
    uint32_t transport_frames;
    uint32_t latency_frames; /* output frames: source queue + DMA + drain allowance */
};
struct ciuki_mixer_master {
    uint32_t version;
    uint32_t size;
    uint32_t left;
    uint32_t right;
    uint32_t mute;
    uint32_t save_state;
    int32_t save_error;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ciuki_audio_caps) == 80, "audio caps");
_Static_assert(sizeof(struct ciuki_audio_format) == 32, "audio format");
_Static_assert(sizeof(struct ciuki_audio_status) == 112, "audio status");
_Static_assert(sizeof(struct ciuki_audio_producer) == 32, "audio producer");
_Static_assert(__builtin_offsetof(struct ciuki_audio_status, accepted_frames) == 24, "audio counters");
_Static_assert(sizeof(struct ciuki_audio_gain) == 24, "audio gain");
_Static_assert(sizeof(struct ciuki_audio_command) == 16, "audio command");
_Static_assert(sizeof(struct ciuki_mixer_master) == 32, "mixer master");

/* Power control (call 73, execution-abi.md amendment 2026-10-12): B=action.
 * Only the supervisor-spawned desktop may call it. REBOOT and OFF flush
 * storage first; OFF returns ENOTSUP or EIO when the machine stays on, with
 * storage already stopped. QUERY returns capability and irreversible
 * shutdown progress bits; UNCERTAIN forbids resuming the session. */
#define CIUKI_POWER_REBOOT 1u
#define CIUKI_POWER_OFF 2u
#define CIUKI_POWER_QUERY 3u
#define CIUKI_POWER_CAN_REBOOT 1u
#define CIUKI_POWER_CAN_OFF 2u
/* QUERY progress: after either bit is set, the session cannot resume. */
#define CIUKI_POWER_STOPPED 4u
#define CIUKI_POWER_UNCERTAIN 8u
/* Call 73 B=STATUS, C=record, D=sizeof(record). Cached read-only facts of
 * the one active provider (ACPI or APM), available to native clients; never
 * evaluates AML or calls firmware in the caller. */
#define CIUKI_POWER_STATUS 4u
#define CIUKI_POWER_VERSION 1u
#define CIUKI_POWER_UNKNOWN 0xffffffffu
#define CIUKI_POWER_FIRMWARE 0u
#define CIUKI_POWER_OWNED 1u
#define CIUKI_POWER_QUARANTINED 2u
#define CIUKI_POWER_PENDING 3u
#define CIUKI_POWER_BATTERIES 4u
#define CIUKI_POWER_ZONES 8u
/* provider: the one exclusive firmware power interface behind the record.
 * NONE while firmware keeps power management (ACPI description only). */
#define CIUKI_POWER_PROVIDER_NONE 0u
#define CIUKI_POWER_PROVIDER_ACPI 1u
#define CIUKI_POWER_PROVIDER_APM 2u
/* ac_online: APM 1.2 "on backup power" (ACPI reports only 0/1). */
#define CIUKI_POWER_AC_BACKUP 2u
/* battery unit: APM 1.2 charge percentage; full is 100, remaining 0..100. */
#define CIUKI_POWER_UNIT_PERCENT 2u
struct ciuki_power_battery {
    uint32_t id;
    uint32_t present;
    uint32_t unit; /* 0: mWh/mW; 1: mAh/mA; 2: percent (APM) */
    uint32_t state; /* _BST bits (APM: derived); UNKNOWN when unavailable */
    uint32_t design;
    uint32_t full;
    uint32_t remaining;
    uint32_t rate;
    uint32_t voltage; /* mV */
    uint32_t seconds; /* firmware-reported remaining life (APM); else UNKNOWN */
};
struct ciuki_power_zone {
    uint32_t id;
    uint32_t temperature; /* absolute tenths Kelvin */
    uint32_t critical;
    uint32_t hot;
    uint32_t passive;
    uint32_t active[10];
    uint32_t cooling; /* active trip bitmask */
    int32_t passive_delta; /* thermal trend; UNKNOWN as INT32_MIN */
};
struct ciuki_power_status {
    uint32_t version;
    uint32_t size;
    uint32_t ownership;
    uint32_t error; /* ACPICA status (APM: positive errno), zero on success */
    uint32_t sequence;
    uint32_t shutdown_requests; /* monotonic/coalesced; desktop handles changes */
    uint32_t ac_online;
    uint32_t lid_open;
    uint32_t battery_count;
    uint32_t zone_count;
    uint32_t event_overflows;
    uint32_t provider; /* CIUKI_POWER_PROVIDER_* */
    uint32_t reserved;
    struct ciuki_power_battery batteries[CIUKI_POWER_BATTERIES];
    struct ciuki_power_zone zones[CIUKI_POWER_ZONES];
};
/* G11 power policy (call 73, docs/design/power-policy.md). STANDBY (desktop
 * only) enters a qualified sleep state; ENOTSUP while QUERY lacks
 * CAN_STANDBY. POLICY (desktop only): C=record, D=sizeof; queues the ATA
 * standby timer of every fixed disk for a kernel task and returns at once;
 * the out fields report the last completed application. EVENTS (any native
 * client): C=record, D=sizeof; cached boot-monotonic counters, no firmware
 * call. */
#define CIUKI_POWER_STANDBY 5u
#define CIUKI_POWER_POLICY 6u
#define CIUKI_POWER_EVENTS 7u
#define CIUKI_POWER_CANCEL_STANDBY 8u
#define CIUKI_POWER_CAN_STANDBY 16u
#define CIUKI_POWER_POLICY_VERSION 1u
#define CIUKI_POWER_EVENTS_VERSION 1u
/* standby_reason: the first unmet standby qualification. */
#define CIUKI_POWER_STANDBY_AVAILABLE 0u
#define CIUKI_POWER_STANDBY_NO_PROVIDER 1u
#define CIUKI_POWER_STANDBY_NO_FIRMWARE 2u
#define CIUKI_POWER_STANDBY_NO_RESUME 3u
struct ciuki_power_policy {
    uint32_t version;
    uint32_t size;
    uint32_t disk_seconds; /* in: idle time before the drives spin down; 0 never */
    uint32_t disks; /* out: fixed disks that accepted the timer */
    uint32_t refused; /* out: disks without the feature or that aborted */
    uint32_t applied_seconds; /* out: programmed time (rounded up) */
    int32_t error; /* out: negative errno of the first failure */
    uint32_t reserved;
};
struct ciuki_power_events {
    uint32_t version;
    uint32_t size;
    uint32_t sleep_requests; /* APM 0001h, 0002h, 0009h, 000Ah */
    uint32_t battery_low; /* APM 0005h */
    uint32_t critical; /* APM 0008h */
    uint32_t resumes; /* APM 0003h, 0004h, 000Bh */
    uint32_t status_changes; /* APM 0006h, 000Ch */
    uint32_t standby_reason; /* CIUKI_POWER_STANDBY_* */
    uint32_t disk_seconds; /* last programmed drive timer, UNKNOWN before */
    uint32_t reserved;
};

/* f2-33: process enumeration (call 70) and termination request (call 71).
 * One system record, then process records in ascending PID order. */
#define CIUKI_PROC_VERSION 1u
#define CIUKI_PROC_NAME_MAX 32u
#define CIUKI_PROC_LIVE 1u
#define CIUKI_PROC_STOPPING 2u
#define CIUKI_PROC_ZOMBIE 3u
#define CIUKI_PROC_LIST_MIN 64u
#define CIUKI_PROC_LIST_MAX 65536u
#define CIUKI_PROC_NO_AUTHORITY 0xffffffffu
struct ciuki_proc_system {
    uint32_t version;
    uint32_t size;
    uint32_t tick_hz;
    uint32_t records;
    uint64_t total_ticks;
    uint64_t idle_ticks;
    uint32_t usable_pages;
    uint32_t free_pages;
    uint32_t kheap_pages;
    uint32_t kheap_bytes;
    uint32_t kheap_high_water;
    uint32_t processes;
    uint32_t disk_read_kib;   /* G30: device bytes read, KiB, wraps at 4 TiB; 0 when not counted */
    uint32_t disk_write_kib;  /* G30: device bytes written, KiB */
};
struct ciuki_proc_record {
    uint32_t version;
    uint32_t size;
    uint32_t pid;
    uint32_t ppid;
    uint32_t pgid;
    uint32_t state;
    uint32_t threads;
    uint32_t resident_pages;
    uint64_t cpu_ticks;
    uint64_t start_tick;
    char name[32];
};
_Static_assert(sizeof(struct ciuki_proc_system) == 64, "proc system record");
_Static_assert(__builtin_offsetof(struct ciuki_proc_system, total_ticks) == 16, "proc system ticks");
_Static_assert(sizeof(struct ciuki_proc_record) == 80, "proc record");
_Static_assert(__builtin_offsetof(struct ciuki_proc_record, cpu_ticks) == 32, "proc record ticks");
_Static_assert(__builtin_offsetof(struct ciuki_proc_record, name) == 48, "proc record name");

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
    uint8_t d_attributes; /* 21: G6 CIUKI_ATTR_* of the entry, 0 when none */
    uint8_t reserved[2]; /* 22 */
    char d_name[768]; /* 24 */
    uint64_t d_size; /* 792: enumeration metadata, no allocation-chain walk */
    int64_t d_mtime; /* 800: Unix seconds, zero when unavailable */
};

/* G6: FAT attributes, times and MS-DOS name through the native file API,
 * call 98 file_info(B=path, C=record, D=sizeof(record)); 0 or -errno.
 * GET fills every field the object has and reports them in mask; SET
 * changes only the mask fields (ATTRIBUTES, CREATED, ACCESSED, MODIFIED) in
 * one directory-entry publication. Times are Unix seconds through the same
 * FAT conversion as stat; FAT keeps created to 10 ms, modified to 2 s and
 * accessed to the day (Microsoft "File Times"). attributes are the FAT
 * byte (Microsoft FAT 1.03 p. 23): SET accepts READONLY|HIDDEN|SYSTEM|
 * ARCHIVE plus the object's own DIRECTORY bit. Errors: EINVAL (version,
 * size, operation, mask, attribute bits, a time outside 1980-2107, a
 * volume root), ENOENT/ENOTDIR/ENAMETOOLONG (path), EFAULT, EROFS (CD,
 * NTFS, write-protected or read-only FAT volume), EOPNOTSUPP (device and
 * synthetic nodes), EIO (medium gone). Win32 GetFileAttributes/SetFile-
 * Attributes/GetFileTime/SetFileTime and DOS INT 21h AX=4300h/4301h and
 * AX=5700h/5701h read and write the same directory entry. */
#define CIUKI_FILE_INFO_VERSION 1u
#define CIUKI_FILE_INFO_GET 0u
#define CIUKI_FILE_INFO_SET 1u
#define CIUKI_ATTR_READONLY 0x01u
#define CIUKI_ATTR_HIDDEN 0x02u
#define CIUKI_ATTR_SYSTEM 0x04u
#define CIUKI_ATTR_DIRECTORY 0x10u
#define CIUKI_ATTR_ARCHIVE 0x20u
#define CIUKI_ATTR_SETTABLE 0x27u
#define CIUKI_FILE_INFO_ATTRIBUTES 1u
#define CIUKI_FILE_INFO_CREATED 2u
#define CIUKI_FILE_INFO_ACCESSED 4u
#define CIUKI_FILE_INFO_MODIFIED 8u
#define CIUKI_FILE_INFO_SHORT_NAME 16u
#define CIUKI_FILE_INFO_ALLOCATED 32u
#define CIUKI_FILE_INFO_SETTABLE 15u
#define CIUKI_FILE_INFO_READ_ONLY_MEDIUM 1u
#define CIUKI_FILE_INFO_VOLUME_ROOT 2u
#define CIUKI_SHORT_NAME_MAX 16u
struct ciuki_file_info {
    uint32_t version; /* 0 */
    uint32_t size; /* 4 */
    uint32_t operation; /* 8 */
    uint32_t mask; /* 12 */
    uint32_t attributes; /* 16 */
    uint32_t flags; /* 20: GET output */
    uint64_t file_size; /* 24 */
    uint64_t allocated; /* 32: bytes in whole clusters ("size on disk") */
    int64_t created; /* 40 */
    int64_t accessed; /* 48 */
    int64_t modified; /* 56 */
    uint32_t created_ms; /* 64: 0-999 past created */
    uint32_t reserved; /* 68 */
    char short_name[16]; /* 72: 8.3 alias, NUL terminated */
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

/* f1-23: syscall 74. Query=0 (ECX pointer), set=1 (ECX mode id).
 * Syscall 67 retains its original 48-byte framebuffer wire record below. */
#define CIUKI_DISPLAY_VERSION 1
#define CIUKI_DISPLAY_QUERY 0u
#define CIUKI_DISPLAY_SET 1u
/* G10: shared screen policy and serialized monitor power, syscall 74. */
#define CIUKI_DISPLAY_SCREEN 2u
#define CIUKI_DISPLAY_DPMS 3u
#define CIUKI_SCREEN_VERSION 1u
#define CIUKI_SCREEN_CONFIG 1u
#define CIUKI_SCREEN_STATUS 2u
#define CIUKI_SCREEN_PERSIST 1u
struct ciuki_screen_control {
    uint32_t version;
    uint32_t size;
    uint32_t action;
    int32_t value;
    uint32_t flags;
    uint32_t enabled;
    uint32_t timeout;
    uint32_t saver;
    uint32_t monitor_seconds;
    uint32_t running;
    uint32_t sequence;
    uint32_t command;
    int32_t argument;
    uint32_t inhibited; /* DOS or delegated game display ownership */
};
struct ciuki_display_mode {
    uint16_t id;
    uint16_t width;
    uint16_t height;
    uint16_t bpp;
    uint16_t pitch;
    uint8_t linear;
    uint8_t current;
};
struct ciuki_display_info {
    uint32_t version;
    uint32_t size;
    uint16_t width;
    uint16_t height;
    uint16_t bpp;
    uint16_t pitch;
    uint32_t vram_bytes;
    uint32_t vbe_version;
    char oem[64];
    char vendor[64];
    char product[64];
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint16_t pci_class;
    uint16_t pci_subclass;
    uint32_t mode_count;
    struct ciuki_display_mode modes[32];
};
_Static_assert(sizeof(struct ciuki_display_mode) == 12, "display mode wire size");
_Static_assert(sizeof(struct ciuki_display_info) == 612, "display info wire size");
_Static_assert(__builtin_offsetof(struct ciuki_display_info, modes) == 228, "display modes offset");

struct ciuki_framebuffer_info {
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

/* Optional display-grant service. Native rectangles have software fallback;
 * cursor returns -EOPNOTSUPP when the desktop must draw its overlay.
 * Generation prevents stale operations across a firmware mode change. */
enum { CIUKI_ACCEL_QUERY, CIUKI_ACCEL_COPY, CIUKI_ACCEL_FILL,
       CIUKI_ACCEL_PATTERN, CIUKI_ACCEL_CURSOR_SET, CIUKI_ACCEL_CURSOR_MOVE,
       CIUKI_ACCEL_CURSOR_SHOW,
       /* f2-92: host-to-screen image write through the 2D engine. rect
        * selects source (src_x, src_y) pixels of `write` and the destination
        * (dst_x, dst_y), width x height; `write.clip` bounds the written
        * destination pixels (zero width or height: no extra clip). Returns
        * 0 when the engine wrote every band, 1 when the CPU wrote visible
        * rows instead; engine-only rows (below) fail when the engine cannot
        * write them. */
       CIUKI_ACCEL_WRITE,
       /* f2-92: engine-only video memory below the visible screen. Returns
        * (end << 16) | first: rows [first, end) at the screen's width are
        * valid COPY sources/destinations and WRITE destinations; 0 when
        * there are none (no engine, no lease, quarantined). */
       CIUKI_ACCEL_OFFSCREEN,
       /* EDX is the output buffer; background is its byte capacity,
        * and colour selects the requested version.
        * Version 1 requires at least sizeof(ciuki_display_engine_info).
        * Copies exactly that size, even when the caller offers more. */
       CIUKI_ACCEL_QUERY_ENGINE };
/* Synchronous command service. DISPLAY_ACCEL keeps its legacy request;
 * operations BATCH/FENCE use EDX for the negotiated envelope, QUERY_COMMANDS
 * uses EDX for ciuki_command_info (colour=version, background=capacity).
 * See docs/design/display-command-semantics.md. Managed batch surfaces are
 * created with the BATCH_* formats; they cannot be mapped into user space. */
#define CIUKI_ACCEL_BATCH 10u
#define CIUKI_ACCEL_FENCE 11u
#define CIUKI_ACCEL_QUERY_COMMANDS 12u
#define CIUKI_ACCEL_QUERY_SURFACE 13u
#define CIUKI_BATCH_VERSION 1u
#define CIUKI_BATCH_MAGIC 826426435u
#define CIUKI_BATCH_MAX_BYTES 1048576u
#define CIUKI_BATCH_MAX_RECORDS 64u
#define CIUKI_BATCH_MAX_TARGETS 128u
#define CIUKI_BATCH_MAX_CLIPS 16u
#define CIUKI_BATCH_MAX_FENCES 64u
#define CIUKI_BATCH_MAX_DIMENSION 16384u
#define CIUKI_BATCH_MAX_COORDINATE 1048576u
#define CIUKI_BATCH_MAX_SAMPLES 4194304u
#define CIUKI_BATCH_MAX_EXTENT 67108864u
#define CIUKI_TARGET_SCANOUT 1u
#define CIUKI_TARGET_VIDEO 2u
#define CIUKI_TARGET_SYSTEM 3u
#define CIUKI_CMD_TRANSPARENT_BG 1u
#define CIUKI_CMD_SHOW 2u
#define CIUKI_FILTER_NEAREST 0u
#define CIUKI_FILTER_BILINEAR 1u
#define CIUKI_FENCE_REFUSED 0u
#define CIUKI_FENCE_RUNNING 1u
#define CIUKI_FENCE_SUCCEEDED 2u
#define CIUKI_FENCE_STOPPED 3u
#define CIUKI_FENCE_RECOVERING 4u
#define CIUKI_FENCE_PARTIAL_DIRTY 1u
#define CIUKI_FENCE_CPU_SAFE 2u
#define CIUKI_FENCE_CANCEL_REQUESTED 4u
#define CIUKI_FENCE_STATUS 1u
#define CIUKI_FENCE_WAIT 2u
#define CIUKI_FENCE_CANCEL 3u
#define CIUKI_FENCE_RELEASE 4u
#define CIUKI_CAP_BATCH 4u
#define CIUKI_CAP_MONO 8u
#define CIUKI_CAP_INVERT 16u
#define CIUKI_CAP_KEYED 32u
#define CIUKI_CAP_SCALED 64u
#define CIUKI_CAP_BILINEAR 128u
#define CIUKI_CAP_LINE 256u
#define CIUKI_CAP_CURSOR_ARGB 512u
#define CIUKI_SURFACE_BATCH_RGB565 2u
#define CIUKI_SURFACE_BATCH_BGR888 3u
#define CIUKI_SURFACE_BATCH_XRGB8888 4u
#define CIUKI_CMD_FILL 1u
#define CIUKI_CMD_COPY 2u
#define CIUKI_CMD_PATTERN 3u
#define CIUKI_CMD_WRITE 4u
#define CIUKI_CMD_MONO 5u
#define CIUKI_CMD_INVERT 6u
#define CIUKI_CMD_KEYED 7u
#define CIUKI_CMD_SCALED 8u
#define CIUKI_CMD_LINE 9u
#define CIUKI_CMD_SYNC 10u
#define CIUKI_CMD_CURSOR_MONO_SET 11u
#define CIUKI_CMD_CURSOR_ARGB_SET 12u
#define CIUKI_CMD_CURSOR_MOVE 13u
#define CIUKI_CMD_CURSOR_SHOW 14u
struct ciuki_batch_submit {
    uint32_t size; /* 0 */
    uint16_t version; /* 4 */
    uint16_t flags; /* 6 */
    uint32_t input_address; /* 8 */
    uint32_t input_bytes; /* 12 */
    uint32_t reply_address; /* 16 */
    uint32_t reply_bytes; /* 20 */
    uint32_t reserved[2]; /* 24 */
};
_Static_assert(sizeof(struct ciuki_batch_submit) == 32, "ciuki_batch_submit size");
struct ciuki_batch_header {
    uint32_t magic; /* 0 */
    uint16_t version; /* 4 */
    uint16_t header_bytes; /* 6 */
    uint32_t total_bytes; /* 8 */
    uint32_t record_count; /* 12 */
    uint32_t records_offset; /* 16 */
    uint32_t targets_offset; /* 20 */
    uint32_t target_count; /* 24 */
    uint32_t payload_offset; /* 28 */
    uint32_t payload_bytes; /* 32 */
    uint32_t flags; /* 36 */
    uint32_t generation; /* 40 */
    uint32_t reserved[5]; /* 44 */
};
_Static_assert(sizeof(struct ciuki_batch_header) == 64, "ciuki_batch_header size");
struct ciuki_batch_target {
    uint32_t kind; /* 0 */
    uint32_t handle; /* 4 */
    uint32_t generation; /* 8 */
    uint32_t flags; /* 12 */
    uint32_t reserved[4]; /* 16 */
};
_Static_assert(sizeof(struct ciuki_batch_target) == 32, "ciuki_batch_target size");
struct ciuki_batch_clip {
    int32_t x; /* 0 */
    int32_t y; /* 4 */
    uint32_t width; /* 8 */
    uint32_t height; /* 12 */
};
_Static_assert(sizeof(struct ciuki_batch_clip) == 16, "ciuki_batch_clip size");
struct ciuki_batch_record {
    uint16_t opcode; /* 0 */
    uint16_t flags; /* 2 */
    uint32_t record_bytes; /* 4 */
    uint32_t dst; /* 8 */
    uint32_t src; /* 12 */
    int32_t dx; /* 16 */
    int32_t dy; /* 20 */
    uint32_t dw; /* 24 */
    uint32_t dh; /* 28 */
    int32_t sx; /* 32 */
    int32_t sy; /* 36 */
    uint32_t sw; /* 40 */
    uint32_t sh; /* 44 */
    uint32_t foreground; /* 48 */
    uint32_t background; /* 52 */
    uint32_t key; /* 56 */
    uint32_t filter; /* 60 */
    uint32_t clip_offset; /* 64 */
    uint32_t clip_count; /* 68 */
    uint32_t data_offset; /* 72 */
    uint32_t data_bytes; /* 76 */
    uint32_t stride; /* 80 */
    int32_t origin_x; /* 84 */
    int32_t origin_y; /* 88 */
    uint32_t reserved0; /* 92 */
    uint8_t pattern[8]; /* 96 */
    uint32_t reserved[6]; /* 104 */
};
_Static_assert(sizeof(struct ciuki_batch_record) == 128, "ciuki_batch_record size");
struct ciuki_batch_reply {
    uint32_t size; /* 0 */
    uint16_t version; /* 4 */
    uint16_t state; /* 6 */
    uint64_t fence; /* 8 */
    uint32_t completed; /* 16 */
    uint32_t first_uncompleted; /* 20 */
    int32_t error; /* 24 */
    uint32_t flags; /* 28 */
    uint32_t admitted; /* 32 */
    uint32_t generation; /* 36 */
    uint32_t reserved[6]; /* 40 */
};
_Static_assert(sizeof(struct ciuki_batch_reply) == 64, "ciuki_batch_reply size");
struct ciuki_fence_control {
    uint32_t size; /* 0 */
    uint16_t version; /* 4 */
    uint16_t action; /* 6 */
    uint64_t fence; /* 8 */
    uint32_t timeout_ms; /* 16 */
    uint32_t reply_address; /* 20 */
    uint32_t reply_bytes; /* 24 */
    uint32_t reserved; /* 28 */
};
_Static_assert(sizeof(struct ciuki_fence_control) == 32, "ciuki_fence_control size");
/* DISPLAY_ACCEL/QUERY_SURFACE: colour=version, background=capacity,
 * rect.src_x=managed surface fd, EDX=output. SURFACE_INFO stays unchanged. */
struct ciuki_batch_surface_info {
    uint32_t size;
    uint32_t version;
    uint32_t generation;
    struct ciuki_surface_info surface;
};
_Static_assert(sizeof(struct ciuki_batch_surface_info) == 36, "batch surface info size");
#define CIUKI_COMMAND_OPERATIONS 14u
struct ciuki_command_info {
    uint32_t size; /* 0 */
    uint32_t version; /* 4 */
    uint32_t software_commands; /* 8 */
    uint32_t native_commands; /* 12 */
    uint32_t capabilities; /* 16 */
    uint32_t cursor_width; /* 20 */
    uint32_t cursor_height; /* 24 */
    uint32_t reserved; /* 28 */
    uint64_t native[CIUKI_COMMAND_OPERATIONS];
    uint64_t software[CIUKI_COMMAND_OPERATIONS];
};
_Static_assert(sizeof(struct ciuki_command_info) == 256, "ciuki_command_info size");

/* f2-92 WRITE source: 32-bit 0xXXRRGGBB rows in the caller's memory. The
 * address is split so the request keeps 4-byte alignment on 64-bit hosts. */
struct ciuki_accel_write {
    uint32_t pixels_lo; /* 0: user address of pixel (0,0) */
    uint32_t pixels_hi; /* 4: zero on i386 */
    uint32_t stride; /* 8: bytes per source row, multiple of 4 */
    uint32_t source_width; /* 12 */
    uint32_t source_height; /* 16 */
    int32_t clip_x; /* 20: destination clip rectangle */
    int32_t clip_y; /* 24 */
    uint32_t clip_width; /* 28 */
    uint32_t clip_height; /* 32 */
};
struct ciuki_accel_request {
    uint32_t size;
    uint32_t operation;
    uint32_t generation;
    uint32_t colour;
    uint32_t background;
    struct ciuki_rect rect;
    uint8_t pattern[8];
    union {
        struct {
            uint32_t cursor_mask[32]; /* MSB first */
            uint32_t cursor_image[32];
        };
        struct ciuki_accel_write write; /* CIUKI_ACCEL_WRITE only */
    };
};
_Static_assert(CIUKI_ACCEL_QUERY == 0 && CIUKI_ACCEL_OFFSCREEN == 8 && CIUKI_ACCEL_QUERY_ENGINE == 9, "display operations");
_Static_assert(sizeof(struct ciuki_accel_request) == 308, "ciuki_accel_request size");
_Static_assert(_Alignof(struct ciuki_accel_request) == 4, "ciuki_accel_request alignment");
_Static_assert(__builtin_offsetof(struct ciuki_accel_request, rect) == 20, "ciuki_accel_request rect");
_Static_assert(__builtin_offsetof(struct ciuki_accel_request, cursor_mask) == 52, "ciuki_accel_request cursor mask");
_Static_assert(__builtin_offsetof(struct ciuki_accel_request, cursor_image) == 180, "ciuki_accel_request cursor image");
_Static_assert(__builtin_offsetof(struct ciuki_accel_request, write) == 52, "ciuki_accel_request write");
_Static_assert(sizeof(struct ciuki_accel_write) == 36, "ciuki_accel_write size");

/* DISPLAY_ACCEL/QUERY optionally writes this snapshot to EDX (zero keeps
 * the original capability-only query). No register reads or activation. */
enum { CIUKI_ACCEL_DRIVER_NONE, CIUKI_ACCEL_DRIVER_LEGACY_NATIVE };
/* Legacy value 1 retains its binary meaning; source compatibility for the
 * frozen driver lives in its private header. */
struct ciuki_accel_info {
    uint32_t driver;
    uint32_t capabilities;
    uint32_t quarantined;
    uint32_t generation;
    uint64_t operations;
    uint64_t idle_waits;
    uint64_t fallbacks;
};
_Static_assert(sizeof(struct ciuki_accel_info) == 40, "ciuki_accel_info size");
_Static_assert(_Alignof(struct ciuki_accel_info) == 4, "ciuki_accel_info alignment");
_Static_assert(__builtin_offsetof(struct ciuki_accel_info, operations) == 16, "ciuki_accel_info operations");

#define CIUKI_DISPLAY_ENGINE_VERSION 1u
enum { CIUKI_ENGINE_NONE, CIUKI_ENGINE_SOFTWARE, CIUKI_ENGINE_NATIVE };
enum { CIUKI_ENGINE_FILL, CIUKI_ENGINE_COPY, CIUKI_ENGINE_PATTERN, CIUKI_ENGINE_WRITE, CIUKI_ENGINE_OPERATIONS };
#define CIUKI_ENGINE_SOFTWARE_OPERATIONS 15u
struct ciuki_display_engine_info {
    uint32_t version;
    uint32_t size;
    uint32_t kind;
    uint32_t capabilities;
    uint32_t software_operations;
    uint32_t quarantined;
    uint32_t generation;
    uint32_t ready;
    uint32_t native_used;
    uint32_t cpu_ready;
    uint16_t pci_vendor;
    uint16_t pci_device;
    char driver_id[16];
    char display_name[48];
    char reason[60];
    uint64_t native[CIUKI_ENGINE_OPERATIONS];
    uint64_t software[CIUKI_ENGINE_OPERATIONS];
};
_Static_assert(sizeof(struct ciuki_display_engine_info) == 232, "display engine size");
_Static_assert(_Alignof(struct ciuki_display_engine_info) == 4, "display engine alignment");
_Static_assert(__builtin_offsetof(struct ciuki_display_engine_info, native) == 168, "display engine counters");

/* G12: input-grant-only keyboard policy; no new syscall. */
#define INPUT_KEYBOARD_SET 0x4905u
#pragma pack(push, 4)
struct ciuki_keyboard_settings {
    uint32_t version;
    uint32_t size;
    uint32_t delay;
    uint32_t speed;
};
#pragma pack(pop)
_Static_assert(sizeof(struct ciuki_keyboard_settings)==16,"keyboard settings size");
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

/* F3-02: one supervisor-delegated DOS window VM per native owner.
 * dosvm(B=operation, C=record*, D=size); execution-abi.md, F3-02 amendment. */
#define CIUKI_DOSVM_VERSION 1u
#define CIUKI_DOSVM_WIDTH 640u
#define CIUKI_DOSVM_HEIGHT 480u
#define CIUKI_DOSVM_NAME_MAX 31u
#define CIUKI_DOSVM_CREATE 1u
#define CIUKI_DOSVM_PRESENT 2u
#define CIUKI_DOSVM_INPUT 3u
#define CIUKI_DOSVM_FOCUS 4u
#define CIUKI_DOSVM_CLOSE 5u
#define CIUKI_DOSVM_STATUS 6u
#define CIUKI_DOSVM_LAUNCH 7u /* arm command visibility before its final Enter */
#define CIUKI_DOSVM_PRESENT_BANDS 8u /* PRESENT plus destination 8-line damage */
#define CIUKI_DOSVM_PRESENT_SCANLINES 1u /* opt-in PRESENT_BANDS flags */
#define CIUKI_DOSVM_FULLSCREEN 9u /* focused=1 enter, 0 return to window */
#define CIUKI_DOSVM_BACKGROUND 10u /* focused=1 pause on Alt+Tab, 0 continue */
#define CIUKI_DOSVM_WAIT_PRESENT 11u /* observe frame/lifecycle events, no drawing */
#define CIUKI_DOSVM_GAME_QUERY 12u /* launch.game input; state=0 absent, 1 present, 2 pending; changed=bytes */
#define CIUKI_DOSVM_GAME_REMOVE 13u /* delegated program-management authority; no VM handle */
/* Per-title game data: launch.game input, remaining inputs zero. QUERY returns
 * changed=file bytes, reason=quota bytes. EXPORT returns files in changed and
 * destination folder in name. RESET/EXPORT require program-management authority
 * and an idle title. RESET is issued only after the UI confirms the request. */
#define CIUKI_DOSVM_SAVE_QUERY 14u
#define CIUKI_DOSVM_SAVE_RESET 15u
#define CIUKI_DOSVM_SAVE_EXPORT 16u
/* fx-4b: background verification of the DOS system image. Only version and
 * size are inputs. Returns state (below), changed = progress in per mille,
 * reason = positive errno when FAILED. No VM capability is required. */
#define CIUKI_DOSVM_IMAGE_QUERY 17u
#define CIUKI_DOSVM_TEXT 18u /* owner-only snapshot of visible BIOS text cells */
#define CIUKI_DOSVM_MARK 19u /* focused=1 pause for Mark, 0 resume */
#define CIUKI_DOSVM_CLIP_REQUEST 20u /* owner fetches transient WINOLDAP request */
#define CIUKI_DOSVM_CLIP_COMPLETE 21u /* matching serial completes it */
#define CIUKI_DOSVM_CLIP_MAX 65536u
#define CIUKI_DOSVM_CLIP_SCREEN 256u /* text-only full-screen Print Screen */
#define CIUKI_DOSVM_TEXT_MAX 16000u
/* G17 owner-bound visible OEM text and window execution policy. No pointers. */
#define CIUKI_DOSVM_TEXT_CHUNK 22u
#define CIUKI_DOSVM_POLICY 23u
#define CIUKI_DOSVM_TEXT_BYTES 48u
struct ciuki_dosvm_text_chunk {
    uint32_t row;
    uint32_t column;
    uint32_t columns;
    uint32_t rows;
    uint8_t characters[CIUKI_DOSVM_TEXT_BYTES];
};
struct ciuki_dosvm_policy {
    uint32_t marking; /* 0/1: pause at the existing guest checkpoint */
    uint32_t idle_sensitivity; /* 0..100: empty BIOS keyboard poll detection */
    uint32_t reserved[8];
};
#define CIUKI_DOSVM_IMAGE_UNCHECKED 0u /* not verified yet: CREATE verifies */
#define CIUKI_DOSVM_IMAGE_VERIFYING 1u
#define CIUKI_DOSVM_IMAGE_VERIFIED 2u
#define CIUKI_DOSVM_IMAGE_MISMATCH 3u /* UNIFIED.IMG differs from UNIFIED.SHA */
#define CIUKI_DOSVM_IMAGE_FAILED 4u
#define CIUKI_DOSVM_WAIT_MAX_MS 50u
#define CIUKI_DOSVM_EVENT_VIDEO 1u
#define CIUKI_DOSVM_EVENT_STATE 2u
#define CIUKI_DOSVM_EVENT_CONSOLE 4u
#define CIUKI_DOSVM_EVENT_TIMEOUT 8u
#define CIUKI_DOSVM_EVENT_CLIPBOARD 16u
/* CREATE launch.kernel: all three historical values map to unified (2).
 * Values 0/1 are retired compatibility inputs, never another kernel.
 * game is the catalog index+1, or zero for the interactive shell. */
#define CIUKI_DOSVM_CIUKIDOS 0u
#define CIUKI_DOSVM_FREEDOS13 1u
#define CIUKI_DOSVM_UNIFIED 2u /* sole CiukiDOS K2043 fork, DFV2 binding and native IFS */
#define CIUKI_DOSVM_GAME_COUNT 3u
/* f3-46 CREATE launch.memory: 0 demand-zero, pageable pool (default); 1 the
 * per-game compatibility mode, eager and resident (Windows 98 PIF "XMS/EMS
 * memory locked"). Any other value returns EINVAL. */
#define CIUKI_DOSVM_MEMORY_PAGED 0u
#define CIUKI_DOSVM_MEMORY_LOCKED 1u
#define CIUKI_DOSVM_VIDEO_READY 1u /* STATUS.changed launch flags */
#define CIUKI_DOSVM_LAUNCH_FAILED 2u
#define CIUKI_DOSVM_FULL_SCREEN 4u /* STATUS.changed */
#define CIUKI_DOSVM_SUSPENDED 8u
#define CIUKI_DOSVM_SHELL_READY 32u /* STATUS: shell blocked for input, guest key queues empty */
#define CIUKI_DOSVM_FULLSCREEN_REFUSED 16u /* STATUS.reason is positive errno */
#define CIUKI_DOSVM_SAVE_FULL 64u /* STATUS.changed: game data quota/disk space exhausted */
#define CIUKI_DOSVM_PROGRAM_RETURNED 128u /* armed EXEC returned to its shell */
#define CIUKI_DOSVM_BOOTING 0u
#define CIUKI_DOSVM_RUNNING 1u
#define CIUKI_DOSVM_STOPPED 2u
struct ciuki_dosvm_clip {
    uint32_t serial;
    uint32_t op; /* WINOLDAP AX low byte, or CLIP_SCREEN */
    uint32_t format;
    uint32_t bytes;
    uint32_t buffer;
    uint32_t capacity;
    int32_t result; /* 0 success, negative errno failure */
    uint32_t reserved[3];
};
struct ciuki_dosvm_text {
    uint32_t buffer;
    uint32_t capacity;
    uint32_t columns;
    uint32_t rows;
    uint32_t cell_height;
    uint32_t reserved[5];
};
struct ciuki_dosvm_command {
    uint32_t version; /* 0 */
    uint32_t size; /* 4 */
    uint32_t id; /* 8 */
    uint32_t generation; /* 12 */
    int32_t surface_fd; /* 16 */
    uint32_t state; /* 20 */
    uint32_t reason; /* 24 */
    uint32_t focused; /* 28 */
    uint32_t changed; /* 32 */
    uint32_t video_mode; /* 36 */
    char name[32]; /* 40 */
    union {
      struct {
       union {
        struct ciuki_input_event event; /* 72: INPUT */
        struct ciuki_dosvm_text text; /* TEXT input buffer/capacity; output OEM cells */
        struct ciuki_dosvm_clip clip; /* CLIP_REQUEST/CLIP_COMPLETE */
        struct ciuki_dosvm_policy policy;
        struct { uint32_t kernel, game, memory, reserved[7]; } launch; /* CREATE input */
        struct { uint32_t bands[2], redraw, flags, reserved[6]; } presentation; /* bands output; redraw input 0/1 */
        struct { uint32_t timeout_ms, frame_ms, events, reserved[7]; } wait; /* WAIT_PRESENT */
       };
       struct ciuki_rect damage; /* 112 */
      };
      /* PRESENT_BANDS with flags=SCANLINES returns 480 exact row bits.
       * The bitmap overlays the entire event/presentation/damage tail.
       * Legacy callers (flags=0) retain the original damage rectangle.
       * An older kernel rejects flags with EINVAL; retry with flags=0. */
      struct { uint32_t bits[15], reserved; } scanlines;
      struct ciuki_dosvm_text_chunk text_chunk;
    };
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
#if !defined(_WIN32)
_Static_assert(sizeof(long double) == 12, "ABI long double size");
_Static_assert(_Alignof(long double) == 4, "ABI long double alignment");
#endif
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
_Static_assert(sizeof(struct ciuki_dirent) == 808, "ciuki_dirent size");
_Static_assert(_Alignof(struct ciuki_dirent) == 4, "ciuki_dirent alignment");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_ino) == 0, "ciuki_dirent.d_ino offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_off) == 8, "ciuki_dirent.d_off offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_reclen) == 16, "ciuki_dirent.d_reclen offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_namlen) == 18, "ciuki_dirent.d_namlen offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_type) == 20, "ciuki_dirent.d_type offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_attributes) == 21, "ciuki_dirent.d_attributes offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, reserved) == 22, "ciuki_dirent.reserved offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_name) == 24, "ciuki_dirent.d_name offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_size) == 792, "ciuki_dirent.d_size offset");
_Static_assert(__builtin_offsetof(struct ciuki_dirent, d_mtime) == 800, "ciuki_dirent.d_mtime offset");
_Static_assert(sizeof(struct ciuki_file_info) == 88, "ciuki_file_info size");
_Static_assert(_Alignof(struct ciuki_file_info) == 4, "ciuki_file_info alignment");
_Static_assert(__builtin_offsetof(struct ciuki_file_info, file_size) == 24, "ciuki_file_info.file_size offset");
_Static_assert(__builtin_offsetof(struct ciuki_file_info, created) == 40, "ciuki_file_info.created offset");
_Static_assert(__builtin_offsetof(struct ciuki_file_info, short_name) == 72, "ciuki_file_info.short_name offset");
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
_Static_assert(sizeof(struct ciuki_framebuffer_info) == 48, "ciuki_framebuffer_info size");
_Static_assert(_Alignof(struct ciuki_framebuffer_info) == 4, "ciuki_framebuffer_info alignment");
_Static_assert(__builtin_offsetof(struct ciuki_framebuffer_info, size) == 0, "ciuki_framebuffer_info.size offset");
_Static_assert(__builtin_offsetof(struct ciuki_framebuffer_info, width) == 4, "ciuki_framebuffer_info.width offset");
_Static_assert(__builtin_offsetof(struct ciuki_framebuffer_info, height) == 8, "ciuki_framebuffer_info.height offset");
_Static_assert(__builtin_offsetof(struct ciuki_framebuffer_info, pitch) == 12, "ciuki_framebuffer_info.pitch offset");
_Static_assert(__builtin_offsetof(struct ciuki_framebuffer_info, bpp) == 16, "ciuki_framebuffer_info.bpp offset");
_Static_assert(__builtin_offsetof(struct ciuki_framebuffer_info, red_size) == 20, "ciuki_framebuffer_info.red_size offset");
_Static_assert(__builtin_offsetof(struct ciuki_framebuffer_info, red_pos) == 24, "ciuki_framebuffer_info.red_pos offset");
_Static_assert(__builtin_offsetof(struct ciuki_framebuffer_info, green_size) == 28, "ciuki_framebuffer_info.green_size offset");
_Static_assert(__builtin_offsetof(struct ciuki_framebuffer_info, green_pos) == 32, "ciuki_framebuffer_info.green_pos offset");
_Static_assert(__builtin_offsetof(struct ciuki_framebuffer_info, blue_size) == 36, "ciuki_framebuffer_info.blue_size offset");
_Static_assert(__builtin_offsetof(struct ciuki_framebuffer_info, blue_pos) == 40, "ciuki_framebuffer_info.blue_pos offset");
_Static_assert(__builtin_offsetof(struct ciuki_framebuffer_info, generation) == 44, "ciuki_framebuffer_info.generation offset");
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

_Static_assert(sizeof(struct ciuki_dosvm_command) == 136, "ciuki_dosvm_command size");
_Static_assert(_Alignof(struct ciuki_dosvm_command) == 4, "ciuki_dosvm_command alignment");
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, surface_fd) == 16, "ciuki_dosvm_command.surface_fd offset");
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, name) == 40, "ciuki_dosvm_command.name offset");
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, launch.kernel) == 72, "ciuki_dosvm_command.kernel offset");
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, launch.game) == 76, "ciuki_dosvm_command.game offset");
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, launch.memory) == 80, "ciuki_dosvm_command.memory offset");
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, event) == 72, "ciuki_dosvm_command.event offset");
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, presentation.bands[0]) == 72, "ciuki_dosvm_command.bands low offset");
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, presentation.bands[1]) == 76, "ciuki_dosvm_command.bands high offset");
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, presentation.redraw) == 80, "ciuki_dosvm_command.redraw offset");
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, wait.timeout_ms) == 72, "ciuki_dosvm_command.wait timeout offset");
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, wait.frame_ms) == 76, "ciuki_dosvm_command.wait frame offset");
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, wait.events) == 80, "ciuki_dosvm_command.wait events offset");
_Static_assert(__builtin_offsetof(struct ciuki_dosvm_command, damage) == 112, "ciuki_dosvm_command.damage offset");


/* F2-41 inventory layout: target and host agree at pack(4). */
_Static_assert(sizeof(struct ciuki_device_resource) == 24, "ciuki_device_resource size");
_Static_assert(_Alignof(struct ciuki_device_resource) == 4, "ciuki_device_resource alignment");
_Static_assert(__builtin_offsetof(struct ciuki_device_resource, type) == 0, "ciuki_device_resource.type offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_resource, flags) == 4, "ciuki_device_resource.flags offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_resource, start) == 8, "ciuki_device_resource.start offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_resource, end) == 16, "ciuki_device_resource.end offset");
_Static_assert(sizeof(struct ciuki_device_record) == 504, "ciuki_device_record size");
_Static_assert(_Alignof(struct ciuki_device_record) == 4, "ciuki_device_record alignment");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, id) == 0, "ciuki_device_record.id offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, class_id) == 4, "ciuki_device_record.class_id offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, bus_type) == 8, "ciuki_device_record.bus_type offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, bus) == 12, "ciuki_device_record.bus offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, slot) == 16, "ciuki_device_record.slot offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, function) == 20, "ciuki_device_record.function offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, vendor) == 24, "ciuki_device_record.vendor offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, device) == 28, "ciuki_device_record.device offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, sub_vendor) == 32, "ciuki_device_record.sub_vendor offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, sub_device) == 36, "ciuki_device_record.sub_device offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, pci_class) == 40, "ciuki_device_record.pci_class offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, subclass) == 44, "ciuki_device_record.subclass offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, prog_if) == 48, "ciuki_device_record.prog_if offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, revision) == 52, "ciuki_device_record.revision offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, state) == 56, "ciuki_device_record.state offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, activation_seq) == 60, "ciuki_device_record.activation_seq offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, generation) == 64, "ciuki_device_record.generation offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, resource_count) == 68, "ciuki_device_record.resource_count offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, flags) == 72, "ciuki_device_record.flags offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, name) == 76, "ciuki_device_record.name offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, driver) == 140, "ciuki_device_record.driver offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, reason) == 172, "ciuki_device_record.reason offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_record, resources) == 252, "ciuki_device_record.resources offset");
_Static_assert(sizeof(struct ciuki_driver_record) == 136, "ciuki_driver_record size");
_Static_assert(_Alignof(struct ciuki_driver_record) == 4, "ciuki_driver_record alignment");
_Static_assert(__builtin_offsetof(struct ciuki_driver_record, name) == 0, "ciuki_driver_record.name offset");
_Static_assert(__builtin_offsetof(struct ciuki_driver_record, build) == 32, "ciuki_driver_record.build offset");
_Static_assert(__builtin_offsetof(struct ciuki_driver_record, policy) == 64, "ciuki_driver_record.policy offset");
_Static_assert(__builtin_offsetof(struct ciuki_driver_record, bound_devices) == 128, "ciuki_driver_record.bound_devices offset");
_Static_assert(__builtin_offsetof(struct ciuki_driver_record, state) == 132, "ciuki_driver_record.state offset");
_Static_assert(sizeof(struct ciuki_device_lease) == 60, "ciuki_device_lease size");
_Static_assert(_Alignof(struct ciuki_device_lease) == 4, "ciuki_device_lease alignment");
_Static_assert(__builtin_offsetof(struct ciuki_device_lease, owner) == 0, "ciuki_device_lease.owner offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_lease, type) == 32, "ciuki_device_lease.type offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_lease, state) == 36, "ciuki_device_lease.state offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_lease, generation) == 40, "ciuki_device_lease.generation offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_lease, start) == 44, "ciuki_device_lease.start offset");
_Static_assert(__builtin_offsetof(struct ciuki_device_lease, end) == 52, "ciuki_device_lease.end offset");
_Static_assert(sizeof(struct ciuki_devices_info) == 38536, "ciuki_devices_info size");
_Static_assert(_Alignof(struct ciuki_devices_info) == 4, "ciuki_devices_info alignment");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, version) == 0, "ciuki_devices_info.version offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, size) == 4, "ciuki_devices_info.size offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, count) == 8, "ciuki_devices_info.count offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, flags) == 12, "ciuki_devices_info.flags offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, generation) == 16, "ciuki_devices_info.generation offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, driver_count) == 20, "ciuki_devices_info.driver_count offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, lease_count) == 24, "ciuki_devices_info.lease_count offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, boot_flags) == 28, "ciuki_devices_info.boot_flags offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, input_policy) == 32, "ciuki_devices_info.input_policy offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, cpu_family) == 36, "ciuki_devices_info.cpu_family offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, cpu_model) == 40, "ciuki_devices_info.cpu_model offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, cpu_stepping) == 44, "ciuki_devices_info.cpu_stepping offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, uptime_ms) == 48, "ciuki_devices_info.uptime_ms offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, cpu_vendor) == 56, "ciuki_devices_info.cpu_vendor offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, cpu_brand) == 72, "ciuki_devices_info.cpu_brand offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, boot_options) == 136, "ciuki_devices_info.boot_options offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, devices) == 264, "ciuki_devices_info.devices offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, drivers) == 32520, "ciuki_devices_info.drivers offset");
_Static_assert(__builtin_offsetof(struct ciuki_devices_info, leases) == 34696, "ciuki_devices_info.leases offset");

/* f5-03a: frozen i386 Win32 records (2026-10-14). */
#define CIUKI_W32_VERSION 0x1u
#define CIUKI_W32_FS_INDEX 0x9u
#define CIUKI_W32_FS_SELECTOR 0x4Bu
#define CIUKI_W32_TEB_SIZE 0x2000u
#define CIUKI_W32_LOW 0x10000u
#define CIUKI_W32_APP_LIMIT 0x70000000u
#define CIUKI_W32_BUILTIN_LIMIT 0x7FFE0000u
#define CIUKI_W32_SHARED 0x7FFE0000u
#define CIUKI_W32_GATE 0x7FFE0100u
#define CIUKI_W32_LIMIT 0x80000000u
#define CIUKI_W32_GRANULARITY 0x10000u
#define W32_IDENTITY 0x1u
#define W32_COUNTER 0x2u
#define W32_VM_ALLOC 0x3u
#define W32_VM_FREE 0x4u
#define W32_VM_PROTECT 0x5u
#define W32_VM_QUERY 0x6u
#define W32_DISPATCHER 0x7u
#define W32_CONTINUE 0x8u
#define W32_RAISE 0x9u
#define W32_THREAD 0xAu
#define W32_DIAGNOSTIC 0xBu
#define W32_MEM_COMMIT 0x1000u
#define W32_MEM_RESERVE 0x2000u
#define W32_MEM_DECOMMIT 0x4000u
#define W32_MEM_RELEASE 0x8000u
#define W32_MEM_FREE 0x10000u
#define W32_MEM_PRIVATE 0x20000u
#define W32_MEM_MAPPED 0x40000u
#define W32_MEM_IMAGE 0x1000000u
#define W32_MEM_TOP_DOWN 0x100000u
#define W32_PAGE_NOACCESS 0x1u
#define W32_PAGE_READONLY 0x2u
#define W32_PAGE_READWRITE 0x4u
#define W32_PAGE_WRITECOPY 0x8u
#define W32_PAGE_EXECUTE 0x10u
#define W32_PAGE_EXECUTE_READ 0x20u
#define W32_PAGE_EXECUTE_READWRITE 0x40u
#define W32_PAGE_EXECUTE_WRITECOPY 0x80u
#define W32_PAGE_GUARD 0x100u
#define W32_PAGE_NOCACHE 0x200u
#define W32_STATUS_SUCCESS 0x0u
#define W32_STATUS_UNSUCCESSFUL 0xC0000001u
#define W32_STATUS_NOT_IMPLEMENTED 0xC0000002u
#define W32_STATUS_ACCESS_VIOLATION 0xC0000005u
#define W32_STATUS_INVALID_PARAMETER 0xC000000Du
#define W32_STATUS_NO_MEMORY 0xC0000017u
#define W32_STATUS_CONFLICTING_ADDRESSES 0xC0000018u
#define W32_STATUS_ACCESS_DENIED 0xC0000022u
#define W32_STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define W32_STATUS_INVALID_IMAGE_FORMAT 0xC000007Bu
#define W32_STATUS_DEVICE_BUSY 0x80000011u
#define W32_EXCEPTION_GUARD 0x80000001u
#define W32_EXCEPTION_ALIGNMENT 0x80000002u
#define W32_EXCEPTION_BREAKPOINT 0x80000003u
#define W32_EXCEPTION_SINGLE_STEP 0x80000004u
#define W32_EXCEPTION_ILLEGAL 0xC000001Du
#define W32_EXCEPTION_BOUNDS 0xC000008Cu
#define W32_EXCEPTION_DIVIDE 0xC0000094u
#define W32_EXCEPTION_OVERFLOW 0xC0000095u
#define W32_EXCEPTION_PRIVILEGED 0xC0000096u
#define W32_EXCEPTION_STACK 0xC00000FDu
#define W32_EXCEPTION_NONCONTINUABLE 0xC0000025u
#define W32_EXCEPTION_SSE 0xC00002B4u
#define W32_CONTEXT_FULL 0x10007u
#define W32_CONTEXT_FLOAT 0x10008u
#define W32_CONTEXT_EXTENDED 0x10020u
#define W32_THREAD_RESERVATION 0x10000u
#define W32_PIT_HZ 0x1234DEu
#define W32_PIT_DIVISOR 0x4A9u

struct ciuki_w32_header {
    uint32_t version;
    uint32_t size;
    uint32_t operation;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ciuki_w32_header) == 16, "ciuki_w32_header frozen size");

/* f5-05B additive loader services. OPEN returns a private read-only raw
 * snapshot; IMAGE maps that snapshot through the image-section service.
 * flags: bit 0 exact placement, bit 1 builtin region, bit 2 DATAFILE. address is in/out;
 * bytes is raw snapshot length. UNMAP releases an image view. No native fd
 * or kernel pointer crosses this boundary. WORD uses private generation-keyed
 * compare/enqueue/wake; spurious returns are allowed. */
#define W32_LOADER 0x107u
#define W32_WORD 0x108u
#define W32_LDR_OPEN 1u
#define W32_LDR_IMAGE 2u
#define W32_LDR_UNMAP 3u
#define W32_LDR_EXACT 1u
#define W32_LDR_BUILTIN 2u
#define W32_LDR_DATAFILE 4u
struct ciuki_w32_loader_args {
    struct ciuki_w32_header header;
    uint32_t action;
    uint32_t path;
    uint32_t source;
    uint32_t bytes;
    uint32_t address;
    uint32_t flags;
};
struct ciuki_w32_word_args {
    struct ciuki_w32_header header;
    uint32_t address;
    uint32_t expected;
    uint32_t wake;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ciuki_w32_loader_args) == 40, "loader wire");
_Static_assert(sizeof(struct ciuki_w32_word_args) == 32, "word wire");

struct ciuki_w32_memory_info {
    uint32_t base;
    uint32_t allocation_base;
    uint32_t allocation_protect;
    uint32_t region_size;
    uint32_t state;
    uint32_t protect;
    uint32_t type;
};
_Static_assert(sizeof(struct ciuki_w32_memory_info) == 28, "ciuki_w32_memory_info frozen size");

struct ciuki_w32_vm {
    struct ciuki_w32_header header;
    uint32_t address;
    uint32_t bytes;
    uint32_t flags;
    uint32_t protect;
    uint32_t old_protect;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ciuki_w32_vm) == 40, "ciuki_w32_vm frozen size");

struct ciuki_w32_query {
    struct ciuki_w32_header header;
    uint32_t address;
    struct ciuki_w32_memory_info info;
};
_Static_assert(sizeof(struct ciuki_w32_query) == 48, "ciuki_w32_query frozen size");

struct ciuki_w32_identity {
    struct ciuki_w32_header header;
    uint32_t major;
    uint32_t minor;
    uint32_t build;
    uint32_t platform;
    uint32_t get_version;
    uint32_t directx;
    uint32_t cpu_features;
    char csd[16];
};
_Static_assert(sizeof(struct ciuki_w32_identity) == 60, "ciuki_w32_identity frozen size");

struct ciuki_w32_counter {
    struct ciuki_w32_header header;
    uint32_t value_lo;
    uint32_t value_hi;
    uint32_t frequency_lo;
    uint32_t frequency_hi;
};
_Static_assert(sizeof(struct ciuki_w32_counter) == 32, "ciuki_w32_counter frozen size");

struct ciuki_w32_dispatcher {
    struct ciuki_w32_header header;
    uint32_t entry;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ciuki_w32_dispatcher) == 24, "ciuki_w32_dispatcher frozen size");

struct ciuki_w32_continue {
    struct ciuki_w32_header header;
    uint32_t context;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ciuki_w32_continue) == 24, "ciuki_w32_continue frozen size");

struct ciuki_w32_raise {
    struct ciuki_w32_header header;
    uint32_t exception;
    uint32_t context;
};
_Static_assert(sizeof(struct ciuki_w32_raise) == 24, "ciuki_w32_raise frozen size");

struct ciuki_w32_thread {
    struct ciuki_w32_header header;
    uint32_t entry;
    uint32_t argument;
    uint32_t trampoline;
    uint32_t stack_bytes;
    uint32_t flags;
    uint32_t tid;
};
_Static_assert(sizeof(struct ciuki_w32_thread) == 40, "ciuki_w32_thread frozen size");

struct ciuki_w32_diagnostic {
    struct ciuki_w32_header header;
    uint32_t kind;
    uint32_t pid;
    uint32_t tid;
    uint32_t status;
    uint8_t payload[208];
};
_Static_assert(sizeof(struct ciuki_w32_diagnostic) == 240, "ciuki_w32_diagnostic frozen size");

struct ciuki_w32_exception {
    uint32_t code;
    uint32_t flags;
    uint32_t chain;
    uint32_t address;
    uint32_t count;
    uint32_t information[15];
};
_Static_assert(sizeof(struct ciuki_w32_exception) == 80, "ciuki_w32_exception frozen size");

struct ciuki_w32_float {
    uint32_t control;
    uint32_t status;
    uint32_t tag;
    uint32_t error_offset;
    uint32_t error_selector;
    uint32_t data_offset;
    uint32_t data_selector;
    uint8_t registers[80];
    uint32_t cr0_npx;
};
_Static_assert(sizeof(struct ciuki_w32_float) == 112, "ciuki_w32_float frozen size");

struct ciuki_w32_context {
    uint32_t flags;
    uint32_t debug[6];
    struct ciuki_w32_float floating;
    uint32_t gs;
    uint32_t fs;
    uint32_t es;
    uint32_t ds;
    uint32_t edi;
    uint32_t esi;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t ebp;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
    uint32_t esp;
    uint32_t ss;
    uint8_t extended[512];
};
_Static_assert(sizeof(struct ciuki_w32_context) == 716, "ciuki_w32_context frozen size");

struct ciuki_w32_teb {
    uint32_t exception_list;
    uint32_t stack_base;
    uint32_t stack_limit;
    uint32_t subsystem_tib;
    uint32_t fiber;
    uint32_t arbitrary;
    uint32_t self;
    uint32_t environment;
    uint32_t pid;
    uint32_t tid;
    uint32_t rpc;
    uint32_t static_tls;
    uint32_t peb;
    uint32_t last_error;
    uint8_t private1[3544];
    uint32_t tls_slots[64];
    uint8_t private2[4336];
};
_Static_assert(sizeof(struct ciuki_w32_teb) == 8192, "ciuki_w32_teb frozen size");

struct ciuki_w32_parameters {
    uint32_t version;
    uint32_t size;
    uint32_t image_path;
    uint32_t command_line;
    uint32_t directory;
    uint32_t environment;
};
_Static_assert(sizeof(struct ciuki_w32_parameters) == 24, "ciuki_w32_parameters frozen size");

struct ciuki_w32_shared {
    uint32_t sequence;
    uint32_t tick;
    uint32_t interrupt_lo;
    uint32_t interrupt_hi;
    uint32_t system_lo;
    uint32_t system_hi;
};
_Static_assert(sizeof(struct ciuki_w32_shared) == 24, "ciuki_w32_shared frozen size");


/* F6 approved extension, 2026-10-20; public integers are SDK-owned. */
#define AF_UNSPEC 0
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_NONBLOCK 2048
#define SOCK_CLOEXEC 524288
#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IP_TTL 2
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#define MSG_PEEK 2
#define MSG_DONTWAIT 64
#define MSG_NOSIGNAL 16384
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_BROADCAST 6
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define TCP_NODELAY 1
#define POLLIN 1
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
#define POLLNVAL 32
#define CIUKI_NET_ABI_VERSION 1
#define CIUKI_SOCKET_MAX 48
#define CIUKI_SOCKET_PROCESS_MAX 16
#define CIUKI_SOCKET_IO_MAX 32768
#define CIUKI_UDP_MAX 1472
#define CIUKI_DNS_MAX 4
#define CIUKI_NET_INFO_CONFIG 0u
#define CIUKI_NET_INFO_SOCKETS 1u
#define CIUKI_NET_SOCKET_CLOSED 0u
#define CIUKI_NET_SOCKET_LISTEN 1u
#define CIUKI_NET_SOCKET_CONNECTING 2u
#define CIUKI_NET_SOCKET_CONNECTED 3u
#define CIUKI_NET_SOCKET_CLOSE_WAIT 4u
#pragma pack(push, 4)
struct ciuki_net_socket_info {
    uint32_t pid;
    uint32_t type;
    uint32_t state;
    uint32_t local_address;
    uint16_t local_port;
    uint16_t peer_port;
    uint32_t peer_address;
};
struct ciuki_net_socket_list {
    uint32_t version;
    uint32_t size;
    uint32_t count;
    uint32_t reserved;
    struct ciuki_net_socket_info sockets[CIUKI_SOCKET_MAX];
};
struct ciuki_sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t address;
    uint8_t zero[8];
};
struct ciuki_socket_io {
    uint32_t size;
    uint32_t version;
    int32_t fd;
    uint32_t buffer;
    uint32_t bytes;
    uint32_t flags;
    uint32_t address;
    uint32_t address_length;
    uint32_t reserved[2];
};
struct ciuki_socket_option {
    uint32_t size;
    uint32_t version;
    int32_t fd;
    uint32_t operation;
    int32_t level;
    int32_t name;
    uint32_t value;
    uint32_t length;
    uint32_t reserved[2];
};
struct ciuki_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};
struct ciuki_resolve4 {
    uint32_t size;
    uint32_t version;
    uint32_t name;
    uint32_t name_bytes;
    uint32_t address;
    int32_t status;
    uint32_t reserved[2];
};
struct ciuki_netctl {
    uint32_t size;
    uint32_t version;
    uint32_t operation;
    uint32_t interface_id;
    int32_t authority_fd;
    uint32_t payload;
    uint32_t payload_bytes;
    uint32_t reserved;
};
struct ciuki_socket_timeval {
    int64_t seconds;
    int32_t microseconds;
    uint32_t zero;
};
#pragma pack(pop)
#endif /* CIUKI_ABI_H */
