# F2 POSIX subset, libc and SDK

Author: Codex. Directive: f2-00. Date: 2026-10-10.
Status: revised after cross-review; lead approval pending.
No implementation or runtime qualification is claimed.

## Scope and authority

**[F2]** Implementations MUST follow this document, the F2 extension of
[execution-abi.md](execution-abi.md#f2-extension-native-abi-version-1), and
[f2-acceptance.md](f2-acceptance.md) together. MUST identifies a requirement,
SHOULD a recommendation and MAY an option. Normative tables MUST be read as
requirements, including their enumerated values and exclusions. Existing F0
probe numbers, registers, errors, address boundaries and FPU rules MUST remain
intact. F2 MUST retain a uniprocessor, non-preemptible kernel with interruptible
blocking points; kernel preemption and user SSE remain excluded in F2.

This is a source-porting subset of
[POSIX.1-2024](https://pubs.opengroup.org/onlinepubs/9799919799/), not a POSIX
conformance claim or a Linux binary ABI. POSIX defines observable interfaces;
the syscall numbering, wire layouts, resource ceilings and desktop objects below
are CiukiOS decisions. **[F2]** Headers MUST NOT advertise `_POSIX_VERSION`,
`_POSIX_SPAWN`, `_POSIX_THREADS` or other complete option groups as implemented.
They MUST define `__CIUKIOS__=1` and `__CIUKI_ABI_VERSION__=1`. Unsupported
interfaces MUST NOT silently succeed through newlib's bare-board sample stubs.

Repository comparison, performed before drafting using Semble and bounded reads:
`src/kernel/core/syscall.c:syscall_dispatch` implements only calls 0–5;
`task.c:task_create_user` creates a private address space per task;
`include/ciuki/task.h:struct task` has no separate process object;
`arch/trap.c:trap_dispatch` terminates a user fault with `0x100 + vector`;
`core/fpu.c:fpu_handle_nm` implements lazy ownership. `core/console.c` maps the
LFB in the kernel. These are F0 mechanisms, not evidence of an ELF loader,
pthreads or public F1 device/file interfaces. **[F2]** Implementation MUST split
process-owned address spaces/descriptors from thread-owned stacks, masks and FPU
state, and MUST consume the qualified F1 VFS, presenter and input interfaces.

## Libc decision and cost

**[F2]** The SDK MUST use full newlib, release **4.5.0.20241231**, with a
CiukiOS system port and a separate CiukiOS pthread library. The release is an
intentional fixed baseline from the [upstream archive](https://sourceware.org/pub/newlib/),
not a claim that it is the newest release.

| Candidate | Upstream properties | Assessment for this contract |
| --- | --- | --- |
| newlib | OS/BSP hooks, `_reent` reentrancy, retargetable library locks; mixed permissive notices, chiefly BSD-style. See the [OS interface and reentrancy manual](https://sourceware.org/newlib/libc.html#Syscalls) and [license collection](https://sourceware.org/newlib/COPYING.NEWLIB). | Smallest adaptation boundary for our own syscall numbers. It does not supply this kernel's pthread implementation. Full stdio/math is more useful here than a nano configuration. |
| picolibc | Embedded C library derived from newlib/AVR libc, compact stdio, compiler TLS and OS hooks; BSD-compatible source selection. See [porting notes](https://github.com/picolibc/picolibc/blob/main/README.md), [OS support](https://github.com/picolibc/picolibc/blob/main/doc/os.md) and [TLS build options](https://github.com/picolibc/picolibc/blob/main/doc/build.md). | Attractive for much smaller machines, but its footprint benefit does not supply processes, pthreads, signals or the missing POSIX layer. Its compiler-TLS integration also exceeds F2's explicit TCB interface. Not selected. |
| musl | MIT-licensed libc for the Linux syscall API, designed for static as well as dynamic linking; integrated pthreads. See [upstream README](https://git.musl-libc.org/cgit/musl/tree/README) and [thread implementation](https://git.musl-libc.org/cgit/musl/tree/src/thread). | Richer POSIX coverage, but replacing syscall numbers alone is insufficient: Linux thread creation, synchronization, signals, TLS and cancellation assumptions would need replacement or emulation. Not selected for F2. |

The cost of newlib is an owned OS adaptation layer: public 64-bit offset/time
types, Linux-numbered errno, true descriptor-backed stdio, thread reentrancy,
locking, signals and a tested pthread subset. It does not make SDL or Wine a
build-only port. Static linking duplicates library text between processes in
F2; there is no shared-library saving. No numerical footprint comparison has
been measured on this kernel.

**[F2]** The port MUST use one initialized `_reent` per thread, reached through
`__getreent()` and `GS:8`, with dynamic reentrancy enabled. It MUST NOT switch a
process-global `_impure_ptr` on a preemptible userspace path. It MUST supply the
reentrant syscall adapters and real malloc, stdio, environment and timezone
locks, including recursive locks where required. Thread termination through
pthreads MUST run TLS destructors and reclaim its `_reent`; raw kernel exit
MUST still reclaim kernel-owned resources without calling user destructors.
The implementation MUST verify these adaptations against the pinned newlib
sources, not assume the generic headers already have the required ABI.
([Newlib reentrancy and locking](https://sourceware.org/newlib/libc.html#Reentrancy))

**[F2]** The SDK MUST record linked text/data/BSS, heap high-water, stack
commitment and per-thread `_reent`/TCB bytes for libc-smoke, Lua and the desktop
on the 128 MiB profile, then repeat the workload at 256 and 512 MiB. Allocation
MUST fail within the measured allocator ledger without consuming existing
kernel emergency reservations. These are measurement requirements; this
contract MUST NOT be used to enlarge the existing physical-memory budgets.

**[F2]** The public ABI header `ciuki/abi.h`, the SDK overlay headers, crt0,
libciuki and libpthread MUST be MIT-licensed (lead decision 2026-10-11), so a
program of any licence can include and statically link them; the kernel stays
GPL-2.0-only and is a separate executable. The import MUST retain every
compiled file's notice and list it in the dependency manifest. Newlib and picolibc MUST NOT be labelled as having one
blanket BSD license. The selected permissive user-library components MAY be
statically linked with GPLv2 programs subject to their notices; the GPLv2 kernel
MUST remain a separate executable. Tool/build licenses and excluded source
directories MUST be distinguished from linked runtime code. This follows
[newlib's own licensing description](https://raw.githubusercontent.com/mirror/newlib-cygwin/master/newlib/README)
and the existing project license rules.

### What later consumers require

| Consumer | Relevant upstream requirement | F2 boundary |
| --- | --- | --- |
| SDL2 (F5) | Platform configuration and video, event, timer, thread and audio backends are separate ports; libc is only part of the environment. [SDL2 porting guide](https://wiki.libsdl.org/SDL2/README-porting). | F2 MUST provide files, allocation, clocks, pthread subset and surface/input/channel building blocks. SDL2 itself and audio remain later work. |
| lwIP (F6) | `sys_arch` supplies threads, mailboxes, semaphores, mutexes and time; it is not dependent on a particular libc. [Upstream porting layer](https://www.nongnu.org/lwip/2_0_x/group__sys__layer.html). | F2 primitives MAY underpin that layer; F2 MUST NOT export socket stubs as working sockets. NIC and stack integration remain F6. |
| ioquake3 (F5/F7) | SDL2 backend, platform/filesystem code and rendering dependencies; the engine and commercial game data have different licensing. [Upstream README](https://github.com/ioquake/ioq3/blob/main/README.md). | Newlib supplies C/math; CiukiOS still needs SDL, graphics, audio and eventually sockets. Static linking or a bytecode VM configuration needs a later engine-specific decision. F2 MUST NOT claim ioquake3 compatibility. |
| Wine (F8) | Kernel threads and a supported host backend; Unix filesystem expectations. [Wine 10.0 requirements](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/README.md). Its [virtual-memory code](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/unix/virtual.c) uses fixed mappings, and its [i386 signal code](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/unix/signal_i386.c) translates machine contexts. | F2 MUST preserve context-bearing faults and 1:1 threads as useful foundations. File mappings, low-address reservations, alternate signal stacks, dynamic loading, broader synchronization and filesystem semantics remain explicit gaps. No Wine release is selected or promised for F8 by this decision. |

The inference from those sources is that newlib makes the first native port
tractable; replacing it or extending its OS layer before F8 remains possible.
**[F2]** Applications MUST use SDK types and wrappers, rather than newlib-private
structures or Linux syscall numbers, so that a later libc change need not change
the kernel ABI.

## Supported library surface and exclusions

**[F2]** The SDK MUST provide the following surface. The detailed restrictions
in this document and execution-abi.md MUST apply to every named wrapper.

| Area | Required library surface |
| --- | --- |
| C runtime | C17 scalar/string/memory/ctype/stdio/conversion/sort/locale/time/math facilities used by Lua; `malloc/calloc/realloc/free`, `aligned_alloc`, `posix_memalign`, `setjmp/longjmp`, `atexit`, `exit`, `_Exit`, `abort`, `getenv/setenv/unsetenv/putenv`. Only `C` and `POSIX` locales; other locale names MUST fail without changing the current locale. |
| Processes | `ciuki_spawn` with explicit inheritance, `wait`, `waitpid`, `getpid`, `getppid`, `_exit`. `wait(s)` MUST equal `waitpid(-1,s,0)`. `ciuki_spawn` MUST return a PID or -1/errno; it is an extension, not `posix_spawn`. |
| Files | All file calls in the syscall table; `lseek`, `fseeko/ftello`, `opendir/fdopendir/readdir/rewinddir/closedir`, `remove`, `tmpfile`, `mkstemp`, `tmpnam`, `isatty`. `rewinddir` MUST reset the directory cookie to zero; `remove` MUST select unlink or rmdir by type. |
| Memory/time | `mmap/munmap/mprotect`, `brk/sbrk`, `clock_gettime`, `clock_getres`, `nanosleep`, `sleep/usleep`, `time/gettimeofday`, `clock`, UTC calendar conversions and `strftime`. |
| Threads | `pthread_create/exit/join/detach/self/equal`, attribute init/destroy and stack-size/detach-state get/set; mutex init/destroy/lock/trylock/unlock with normal and recursive types; condition init/destroy/wait/timedwait/signal/broadcast and clock attribute; `pthread_once`; TLS key create/delete/get/set; `pthread_sigmask` and `pthread_kill`. |
| Signals | `sigaction`, `signal`, `sigprocmask`, `sigemptyset/fillset/addset/delset/ismember`, `raise`, `kill`; the restricted signal set and fault contexts in the ABI extension. |
| Information | `uname`, `getpagesize`, `sysconf` for `_SC_PAGESIZE=4096`, `_SC_OPEN_MAX=128`, `_SC_ARG_MAX=65536`, `_SC_THREAD_KEYS_MAX=64`, `_SC_THREAD_STACK_MIN=65536`. Unknown sysconf names MUST return -1/EINVAL. |

**[F2]** `fork`, `vfork`, `_Fork` and every `exec*` function MUST be excluded.
Exported compatibility stubs MUST return -1/ENOSYS. `posix_spawn*` MUST NOT be
exported as conforming wrappers: POSIX inherits all eligible descriptors, whereas
our baseline explicitly admits selected descriptors. This is a deliberate
departure from [POSIX spawn](https://pubs.opengroup.org/onlinepubs/9799919799/functions/posix_spawn.html).
Adding fork would require address-space duplication/COW and multithreaded-child
lock semantics; adding exec would require atomic image replacement while
retaining process identity. Neither cost belongs in the first gate.

**[F2]** The following MUST remain excluded: users, authentication, Unix
permission enforcement, uid/gid changes, chmod/chown/umask, terminals, termios,
sessions/job control, pipes/poll/select, sockets/DNS, hard and symbolic links,
file-backed mmap, general shared-memory mmap, dynamic ELF linking, `dlopen`,
PIE, compiler `_Thread_local`/`__thread`, pthread cancellation, robust or
process-shared mutexes, priority inheritance, rwlocks, realtime signal queues,
sigaltstack, setcontext and process timers. `system(NULL)` MUST return zero;
`system(nonnull)` and `popen` MUST fail with ENOSYS. `isatty` MUST return zero
with ENOTTY for an open F2 console descriptor and EBADF for an invalid one.
F2's console is a byte stream, not a terminal. These exclusions MUST be visible
in SDK documentation and capability tests; a successful configure check MUST
NOT result from an ENOSYS stub hidden behind a feature macro.

### Pthreads and synchronization

**[F2]** Each pthread MUST correspond to one kernel thread. `pthread_t` MUST
be a 32-bit opaque ID; the kernel MUST NOT reuse it during one boot. Public
mutexes MUST occupy eight u32 words, condition variables four u32 words,
`pthread_once_t` one u32, and thread attributes four u32 (stack bytes, detached,
then two reserved zeros). Static mutex/condition/once initializers MUST be all
zero. Mutex attributes MUST occupy two u32 (type, reserved); normal=0,
recursive=1. Condition attributes MUST occupy two u32 (clock ID, reserved),
with default REALTIME=0. Unsupported attribute values MUST return ENOTSUP.
These are library layouts; the kernel MUST NOT interpret their private words
except the aligned wait word explicitly passed to it.

**[F2]** Pthread functions returning an error MUST return the positive error
number, without setting errno. Creation MUST translate kernel EFAULT/EINVAL
into EINVAL and resource exhaustion into EAGAIN. Join/detach MUST preserve
ESRCH, EINVAL and EDEADLK meanings; join MUST retry kernel EINTR. Mutex trylock
MUST return EBUSY when held; normal recursive acquisition MUST return EDEADLK;
non-owner unlock MUST return EPERM; recursive-count overflow MUST return
EAGAIN. Destroying a busy mutex/condition MUST return EBUSY. Invalid initialized
object/attribute values MUST return EINVAL. Allocation failures in lazy lock
initialization MUST return ENOMEM. Timed condition waits MUST return ETIMEDOUT
only after reacquiring the mutex; ordinary condition waits MUST NOT expose
EINTR. Other failures MUST NOT be converted into success.

**[F2]** Mutex acquisition (including condition-wait reacquisition),
`pthread_once` waiters and newlib internal locks built on wait_word MUST
recheck their state and retry after kernel EINTR. They MUST NOT expose that
internal EINTR to their callers or treat interruption as successful acquisition
or completed initialization. This follows the no-EINTR rule for
[POSIX mutex acquisition](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutex_lock.html);
the raw CiukiOS wait extension itself MUST retain its documented EINTR result.

**[F2]** `ciuki_wait_word`/`ciuki_wake_word` MUST implement the private-process
compare/enqueue protocol defined in the ABI. The API MUST be named as a CiukiOS
extension, never as Linux futex compatibility. Mutex acquisition/release MUST
use acquire/release atomics. A condition wait MUST sample a sequence while
holding its mutex, release the mutex, wait only if the sequence still matches,
and reacquire the mutex before returning. Signal/broadcast MUST change the
sequence before waking waiters. Lost wakes across preemption, wrap-around and
timeout races MUST be tested. Predicate loops remain required because successful
wakes MAY be spurious. This implements the synchronization behavior described
by [POSIX condition waits](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_cond_clockwait.html),
without adopting Linux's syscall interface.

**[F2]** A process MUST have at most 64 pthread keys. Key creation MUST return
EAGAIN at that limit; invalid keys MUST produce EINVAL for set/delete and NULL
for get. Thread exit MUST run up to four destructor passes over non-null values,
clearing each before calling its destructor. `pthread_once` MUST serialize one
initializer and publish its writes before releasing waiting threads. Its
initializer MUST NOT exit or recursively enter the same once control; either
misuse MUST terminate that process with SIGABRT. No user callback MUST run under
a kernel lock. `raise` MUST address the calling thread, not an arbitrary thread
of the process.

## POSIX paths over the drive VFS

**[F2]** `/` MUST denote `C:\`, the boot volume. `/mnt` MUST be a synthetic
directory; `/mnt/d` through `/mnt/z` MUST denote mounted non-system volumes
`D:\` through `Z:\`. There MUST be no `/mnt/c` alias. `/dev` MUST be synthetic
and contain only `null` and `console` in F2. These two reserved root names MUST
take precedence over same-named on-disk entries, which MUST NOT be created or
modified through the POSIX view. `/bin`, `/tmp`, `/home` and `/system` MUST be
ordinary boot-volume directories created by the canonical image builder.
This lets software use one root, `/tmp`, HOME and ordinary relative paths;
SDL/game ports need no drive-letter parsing. Wine's later drive mappings remain
a Wine-backend responsibility, not synthetic POSIX symlinks in F2.

**[F2]** The public path parser MUST use `/` as separator, collapse repeated
slashes and resolve `.` and `..` component by component; `..` at `/` MUST stay
at `/`, and `..` at `/mnt/d` MUST reach `/mnt`. Lookup of an intermediate
component MUST occur before a following `..`; `missing/../x` MUST fail ENOENT.
Trailing `/` MUST require a directory. A backslash or colon in a public path
MUST return EINVAL; DOS drive syntax MUST go through the future DOS bridge.
An empty string MUST fail ENOENT. Absolute and relative paths MUST use the same
resolver, with a snapshot of the process cwd taken at syscall entry.

**[F2]** The resolver MUST preserve spelling and compare names with the same
case-insensitive F1 VFS key; the SDK MUST NOT add locale-dependent case folding
or Unicode normalization. F1's UTF-8-to-UCS-2 representability and FAT naming
rules MUST remain binding. Invalid UTF-8, surrogate encodings and non-BMP
characters MUST fail EILSEQ; forbidden FAT characters and trailing space/dot
components MUST fail EINVAL. The canonical drive path, including drive prefix
and separators, MUST fit 259 UCS-2 code units plus NUL. Component names MUST
fit 255 UCS-2 code units. `PATH_MAX` MUST be 1040 bytes and `NAME_MAX` 765
bytes for UTF-8 buffers; these byte bounds MUST NOT relax either character
bound. An unterminated path within PATH_MAX or either length violation MUST
return ENAMETOOLONG. Case-colliding creation MUST fail EEXIST.

**[F2]** Each process MUST own one cwd reference, initially inherited by spawn;
threads MUST share it. `getcwd` MUST reconstruct the absolute, case-preserved
POSIX path under namespace locks, including after directory rename. A cwd and
an open directory MUST pin its node: rmdir MUST fail EBUSY while pinned.
Cross-volume rename MUST return EXDEV. The future DOS personality MUST keep its
per-VM current drive and per-drive cwd separately; a DOS chdir MUST NOT alter a
native process cwd. These rules extend the
[VFS namespace and open descriptions](vfs-storage-contract.md).

### Files and departures from full POSIX

**[F2]** Native opens MUST use VFS share mode deny-none and MUST honor existing
conflicting share claims with EACCES. Duplicates and inherited descriptors MUST
share position and status flags; FD_CLOEXEC MUST belong to the individual fd.
`pread/pwrite` MUST leave the description position unchanged, including pwrite
on an O_APPEND description; append placement MUST apply only to write.
One append write MUST select EOF and modify data under the node write lock.
([POSIX open](https://pubs.opengroup.org/onlinepubs/9799919799/functions/open.html))

**[F2]** Normal file read/write MUST accept at most 1 MiB per syscall and MAY
return a positive short count. Zero-length transfers MUST validate the fd and
mode, then return zero without accessing the buffer. No write or truncate MUST
exceed FAT's `0xffffffff` byte size: an operation crossing that limit MUST fail
EFBIG before changing data. Seek MUST permit positions through INT64_MAX without
allocating storage; later writes remain subject to EFBIG. Growing a file MUST
zero the gap. EOF reads MUST return zero. Directory byte reads/writes MUST fail
EISDIR; byte I/O on other non-stream objects MUST fail EBADF.

**[F2]** Unlink of an open regular file MUST remove its name and retain its
node/cluster chain until its final open description closes. Its fstat link
count MUST become zero. Rename MUST support replacement of a regular file or
an empty directory of the same type and retain open replaced nodes. A regular
file over a directory MUST fail EISDIR; a directory over a regular file MUST
fail ENOTDIR; moving a directory into its own descendant MUST fail EINVAL.
After path validation, arguments resolving to the identical existing entry
MUST succeed as a no-op before replacement and pinning checks. Case-only rename
MUST be the exception to that no-op and MUST update preserved spelling.
Successful namespace changes MUST be indivisible to other threads.
The final `.` or `..` component MUST be rejected with EINVAL for rename/rmdir;
unlink of any directory MUST fail EISDIR. Other renaming/removing of synthetic
roots or volume mountpoints MUST fail EBUSY. A directory used as cwd MAY be
renamed, but a pinned destination directory MUST NOT be replaced.

POSIX specifies atomic rename and retention of open unlinked files;
[rename](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html)
and [general filesystem concepts](https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/V1_chap04.html)
are the semantic references. The existing FAT contract orders cross-directory
rename by durable source removal before destination publication. **[F2]** That
order MUST remain unchanged. An I/O failure during this sequence MUST return
EIO, revoke writing as required by F1, and MAY leave a removed name/lost chain;
F2 MUST document this failure-atomicity departure instead of promising Unix
rollback or crash persistence. Open-unlinked chains lost on a crash MUST be
classified as permitted lost chains, never cross-links. Namespace locks MUST
hide intermediate successful-rename states from live observers.

**[F2]** `close` MUST release the fd exactly once even when it reports EIO;
it MUST NOT return EINTR. Applications requiring durability MUST use fsync
before close. Fsync MUST include the node's data, directory metadata and F1's
durability barrier; delayed errors MUST remain observable. Ftruncate MUST NOT
change the open position. Stat MUST synthesize uid/gid=0 and informational
mode bits; there is no access-control claim. FAT read-only attributes and
read-only/quarantined volume handling MUST remain effective.

**[F2]** `getdents` MUST emit whole fixed-size records, including `.` and `..`,
with opaque monotonically advancing per-description cookies and zero at EOF.
Concurrent directory mutation MAY change enumeration order or visibility;
malformed records and duplicate cookies within an unmodified enumeration MUST
fail tests. Directory lseek MUST support only offset zero/SEEK_SET, for rewind.
`tmpfile` MUST create with O_EXCL and unlink after opening; temporary names MUST
resolve under `/tmp`. `tmpnam` MUST be documented as non-reserving, and MUST NOT
be used for kernel or SDK security decisions.

**[F2]** `/dev/null` MUST accept read/write opens, return EOF on read and the
requested count on write. `/dev/console` MUST accept only O_WRONLY and deliver
bytes through the bounded, serialized F1 console/serial sink; a read-capable
open MUST fail EACCES. Device creation/truncation/directory flags MUST fail
EINVAL. Console output MUST preserve newline/tab and valid UTF-8 text rather
than reuse call 2's diagnostic sanitization; unsupported glyphs MAY use a
replacement glyph while serial retains the bytes. Console writes MUST report
accepted bytes and MUST NOT forge test-controller records. Console/null seek,
pread and pwrite MUST fail ESPIPE. A boot-launched ordinary program MUST receive
fd 0 opened on null and fds 1–2 on console unless its launcher explicitly
supplies files. Interactive text input MUST go through a desktop client; the
kernel console MUST NOT consume the exclusive F1 input queue a second time.

**[F2]** `sigemptyset` and `sigfillset` MUST write zero or the supported signal
bits respectively. Add/delete/member MUST reject unsupported numbers with
EINVAL. Signal masks MUST include only the supported set; pthread_sigmask MUST
use the calling-thread sigprocmask semantics with positive error returns.
`signal` MUST install a persistent one-argument action with flags/mask zero
and the SDK restorer. `abort` MUST unblock and raise SIGABRT in the caller and,
if a handler returns, install default action and raise it again. No libc locks
MUST be required by the async-signal-safe subset `_exit`, read, write, kill,
sigaction and sigprocmask; other libc/pthread entry points MUST be documented
as unsafe inside handlers.

## SDK build and reproducibility

**[F2]** The target MUST remain `i686-unknown-elf`, `-march=pentiumpro`.
`config/toolchain.json` currently pins clang major 23, ld.lld major 23 and
NASM major 3. The SDK build MUST enforce these pins and record full versions
and executable hashes. The user compile flags MUST be the existing ABI flags
with `-mno-80387` removed, plus `-fno-stack-protector`, `-mstack-alignment=4`
and `-mno-sse -mno-sse2 -mno-mmx`. All C runtime and application objects MUST
use the same flags and 64-bit type definitions. Host Linux headers, startup
objects and libraries MUST NOT enter the target link. This preserves the
[i386 calling convention](https://www.sco.com/developers/devspecs/abi386-4.pdf).

**[F2]** The SDK MUST install the following structure under
`build/tools/ciuki-sdk/`; these paths specify a future deliverable, not files
added by f2-00:

| Path | Contents |
| --- | --- |
| `bin/ciuki-cc` | Wrapper around pinned clang and ld.lld, applying the sysroot, CPU/ABI flags and static link recipe. |
| `sysroot/include/` | Newlib headers plus CiukiOS `sys/`, `pthread.h`, `ciuki/abi.h`, `ciuki/spawn.h`, `ciuki/surface.h`, `ciuki/channel.h`; ABI constants MUST have one source. |
| `sysroot/lib/` | `crt0.o`, `libc.a`, `libm.a`, `libpthread.a`, `libciuki.a`, and a target-only compiler helper archive. |
| `sysroot/lib/ciuki.ld` | ELF32 linker script starting at 0x00400000, separate page-aligned RX/R/RW LOAD segments, non-executable GNU_STACK, retained init/fini arrays, no dynamic or TLS segments. |
| `manifest.json` | Tool hashes, source release/commit/archive SHA-256, patch hashes, configure arguments, target flags, headers/archives hashes and linked-file license inventory. |

**[F2]** Newlib MUST be built out of tree as described by its
[build instructions](https://raw.githubusercontent.com/mirror/newlib-cygwin/master/newlib/README),
using target clang/LLVM tools through explicitly configured wrappers and a
CiukiOS system-port selection. The imported archive MUST be
`newlib-4.5.0.20241231.tar.gz`; its bytes and corresponding upstream commit MUST
be pinned and recorded before any SDK evidence. The build MUST refuse a missing
or mismatched digest, moving branch, or unrecorded patch. GCC-oriented configure
probes and assembly MUST be adapted explicitly, not bypassed with host results.
No libgloss board startup, libnosys fake-success stubs, or upstream syscall
numbers MUST be linked. Full floating and long-long stdio, multithread locks
and 64-bit off_t/time_t MUST be enabled; nano stdio and compiler TLS MUST remain
disabled. Small-reent mode MUST remain off for this first baseline.

**[F2]** The wrapper's final link MUST use ld.lld `-m elf_i386 -static`,
`ciuki.ld`, `crt0.o`, application objects, then a group containing libpthread,
libciuki, libc, libm and the compiler helpers. The wrapper MUST retain the frozen
`-nostdlib` policy and supply those objects explicitly. Undefined symbols,
PT_INTERP/PT_DYNAMIC/PT_TLS, ET_DYN, W+X segments and incompatible instructions
MUST fail T1. Additional user-only compiler helpers MAY be provided in the SDK
archive after license and opcode checks; the frozen kernel helper whitelist
MUST NOT be changed by this allowance. A wrapper self-test MUST compile, link
and inspect a program without running host binaries as target probes.

**[F2]** crt0 MUST consume the entry stack and TCB specified in the ABI,
initialize `_reent`, environ, stdio and pthread main-thread state, run init-array
constructors, call `main(argc,argv,environ)`, and call exit with its result.
Exit MUST run atexit handlers and fini-array destructors and flush stdio before
call 0; `_Exit`/`_exit` MUST use call 0 immediately. `sbrk` MUST serialize query,
overflow checking and brk changes with the allocator lock.

## Application gate and graphical stretch

**[F2]** The required upstream program MUST be **Lua 5.4.8**, statically built
from the [official release](https://www.lua.org/ftp/lua-5.4.8.tar.gz), with its
matching [official test suite](https://www.lua.org/tests/). Both are distributed
under the [MIT license](https://www.lua.org/license.html); their notices MUST
ship with the program. Lua is small enough to be a plausible 128 MiB gate and
exercises allocation/GC, math, strings, stdio/files, time and console output
without networking. This is a suitability judgment, not a measured footprint.

**[F2]** The mandatory upstream run MUST be `lua -e "_U=true" all.lua` in
the unmodified `lua-5.4.8-tests` directory. This is upstream's **basic portable
suite**, not its complete or internal suite; `_U` intentionally omits
nonportable and large-memory cases. The suite archive SHA-256 MUST be
`9581d5a7c39ffbf29b8ccde2709083c380f7bbddbd968dcb15712d2f2e33f4e5`, as published
by Lua. The source archive digest MUST be pinned at import. Assertions MUST NOT
be disabled, test files patched, or additional skip switches injected to obtain
a pass. The canonical binary MUST use portable C configuration, 64-bit Lua
integers and double numbers, with no readline, `LUA_USE_LINUX`, dlopen or shell
dependency. Upstream's `final OK` plus exit zero and the named case evidence in
f2-acceptance.md MUST establish the gate; compile-only evidence is insufficient.

**[F2]** A separate CiukiOS Lua script MUST supplement the upstream run with
known file/rename/unlink hashes, allocation churn, UTC/time checks and stdout/
stderr checks. It MUST be reported as a project test, never an upstream test.
Unavailable complete/internal tests MUST appear as excluded coverage in the
result manifest. Any defect exposed by the portable suite MUST be fixed in the
port, or returned to lead review as a contract change, not silently skipped.

**[F2 stretch]** A second graphical program SHOULD port the existing
doomgeneric platform layer to the surface/channel interfaces, using an
explicitly redistributable test WAD with its own pinned license and hash.
The upstream [doomgeneric interface](https://github.com/ozkl/doomgeneric)
separates framebuffer, input and timing callbacks. It MAY remain without sound
in this stretch; it MUST NOT pull SDL, sockets or commercial game data into
the F2 gate. A deterministic frame/input/exit test MUST identify both engine
and data. The actual ring-3 desktop and client-crash survival remain mandatory
even when this stretch is not attempted.

## Remaining qualification questions

No syscall numbering, public type width, inheritance or process-lifetime choice
is intentionally deferred by this proposal. The unresolved evidence is the
newlib port's actual size and clang-build adjustments, Lua's guest results,
F1 device behavior on the two laptops, and the later Wine host/backend version.
**[F2]** These MUST be reported as unmeasured until tested. Source commit/archive
digests MUST be closed by the import directive before building; measured memory
shortfalls MUST return to the lead without automatically changing budgets.
