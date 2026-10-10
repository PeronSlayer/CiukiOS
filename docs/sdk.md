# CiukiOS F2 C SDK

The SDK builds static i686 ELF programs against full newlib 4.5.0.20241231.
The kernel ABI remains defined solely by `src/kernel/include/ciuki/abi.h`.
This document records the f2-06 port recipe and host evidence; guest acceptance
and resource measurements on 128/256/512 MiB machines remain lead-owned.

## Build offline

Verify available host memory first. The lead runs the clean SDK build in a
capped scope, with at most two compiler jobs:

```sh
free -m
systemd-run --user --scope -p MemoryMax=3G -p MemorySwapMax=1G -- \
  bash scripts/build_sdk.sh --archive /path/to/newlib-4.5.0.20241231.tar.gz --jobs 2
```

`make sdk SDK_ARGS='--archive /path/to/newlib-4.5.0.20241231.tar.gz --jobs 2'`
uses that same capped recipe. The implementer runs the script directly under
the directive's explicit sandbox exception, without QEMU or systemd.
All temporary files, configure outputs and compiler intermediates are placed
inside `build/tools/ciuki-sdk/`; the script never downloads sources or uses
`/tmp`. A missing archive, wrong digest, changed/unrecorded patch, unpinned tool
or mutable commit identifier is rejected before replacing the previous SDK.

The archive SHA-256 is
`33f12605e0054965996c25c1382b3e463b0af91799001f5bb8c0630f2ec8c852`.
The release tag is `newlib-4.5.0`, resolved to immutable commit
`5e5e51f1dc56a99eb4648c28e00d73b6ea44a8b0` using the
[upstream repository mirror's tag/commit links](https://repo.or.cz/newlib-cygwin.mirror.git).
The supplied newlib and both Lua 5.4.8 archives were independently hashed and
matched their supplied pins. Lua is recorded in `config/sdk-pins.json` for its
later directive; this build does not build or qualify Lua.

## Compile a program

```sh
build/tools/ciuki-sdk/bin/ciuki-cc sdk/tests/hello.c -o build/tools/ciuki-sdk/tests/hello.elf
build/tools/ciuki-sdk/bin/ciuki-cc -c program.c -o program.o
build/tools/ciuki-sdk/bin/ciuki-cc program.o -o program.elf
build/tools/ciuki-sdk/bin/ciuki-cc --self-test
python3 sdk/tests/check_sdk.py
```

The wrapper supplies its own sysroot, clang resource headers, frozen i686 /
Pentium Pro flags, four-byte stack alignment, x87 math, no SSE/MMX, no PIE,
no stack protector, and `-nostdlib`. It invokes ld.lld with `-m elf_i386 -static`,
`crt0.o`, application objects, then an archive group in this order:
`libpthread`, `libciuki`, `libc`, `libm`, `libcompiler`. There are no host startup
objects or automatically selected host libraries. Debug paths are normalized
to the repository root. Link maps accompany the ELF files.

Public pthread types and constants come from the ABI; compiler TLS is excluded.
Use pthread keys for thread-local application data. `ciuki_spawn` takes a path,
argv, envp, explicit fd mappings, mapping count and flags, and returns a PID or
-1/errno. Surface, input, display and channel interfaces are in `ciuki/surface.h`
and `ciuki/channel.h`. The generated `ciuki/raw.h` exports untranslated u32
results with six u32 register arguments; omitted registers are passed as zero.
Most programs should use the typed wrappers instead.

Only C/POSIX locales and UTC are supported. `/` is the boot volume and temporary
files live under `/tmp` in the guest. `tmpnam` only produces a name; it does not
reserve it. Use `mkstemp` or `tmpfile` to create temporary files. A console fd is
a byte stream; `isatty` returns zero/ENOTTY for it.

Fork/vfork/_Fork and exec variants return -1/ENOSYS. `system(NULL)` returns zero;
`system(command)` and `popen` fail with ENOSYS. OS entropy, permission-changing and link/pipe
compatibility entry points also fail visibly. Sockets/DNS, terminals/termios,
sessions, job control, poll/select, POSIX spawn, file-backed/shared/fixed mmap,
dynamic loading/PIE, compiler TLS, cancellation, robust/process-shared mutexes,
priority inheritance, rwlocks, realtime queues, alternate signal stacks,
setcontext and process timers remain excluded. The SDK does not advertise
complete POSIX option groups. Except for `_exit`, read/write, kill, sigaction
and sigprocmask, libc/pthread calls must not be used from signal handlers.

## Research and port decisions

Research preceded implementation. The pinned archive's `configure.host`,
`newlib/configure`, `libc/include/reent.h`, `sys/reent.h`, `sys/_types.h`,
`sys/lock.h`, malloc/stdio/env/time sources and i386 assembly were checked
against these upstream interfaces:

- [Newlib OS and reentrancy manual](https://sourceware.org/newlib/libc.html#Syscalls)
  and [upstream configure.host](https://raw.githubusercontent.com/mirror/newlib-cygwin/master/newlib/configure.host):
  select an equivalent system port while preserving the contracted ELF triple;
  build only the newlib subdirectory out of tree, excluding libgloss/libnosys
  and every upstream board syscall directory. Supply `_open_r` and the other
  reentrant hooks explicitly; do not use global errno adapters.
- [Upstream reent.h](https://raw.githubusercontent.com/mirror/newlib-cygwin/master/newlib/libc/include/reent.h)
  and [sys/lock.h](https://raw.githubusercontent.com/mirror/newlib-cygwin/master/newlib/libc/include/sys/lock.h):
  dynamic `_REENT` uses `__getreent()` from `GS:offsetof(TCB,reent)`. Real normal
  and recursive retargetable locks use pthread mutexes; allocation, stdio,
  environment, timezone and exit registration retain their required locks.
  The per-thread bootstrap `_reent` fits after the TCB in its TLS page, allowing
  thread startup to allocate its permanent `_reent` without shared errno.
- [Clang cross-compilation](https://clang.llvm.org/docs/CrossCompilation.html):
  set target and CPU explicitly and exclude host include/library discovery.
  Configure is cross mode and performs target compile/link probes, never target
  execution. LLVM archive tools preserve their invocation names: resolving
  `llvm-ranlib` to the `llvm-ar` executable changes its command-line behavior.
- [Newlib license collection](https://sourceware.org/newlib/COPYING.NEWLIB):
  retain upstream source and per-file notices, rather than labelling all newlib
  as one BSD license. `manifest.json` includes compiled source/notice hashes,
  installed headers/archives, patches, tools, SDK sources and linked members.

The eight pinned patches are:

1. Equivalent CiukiOS `configure.host` selection: dynamic reentrancy,
   reentrant hooks, external startup, no upstream syscalls/signal wrappers.
   Remove the generic ELF MISSING_SYSCALL_NAMES default, which otherwise
   redirects reentrant adapters to ordinary calls; enable HAVE_FCNTL/BLKSIZE.
2. Preserve ABI clock constants in `time.h`; newlib's default REALTIME=1 and
   CLOCKS_PER_SEC=1000 conflict with the ABI. Expose only the required clock
   prototypes without advertising a complete POSIX timer option.
3. Replace GCC's legacy LONG_LONG/ULONG_LONG limit spellings with C17
   LLONG/ULLONG spellings in narrow and wide integer conversions.
4. Map the BSD database implementation's EFTYPE failures to EINVAL, without
   inventing another kernel errno number.
5. Make reentrant stat/time forward declarations use the exact wire records,
   regardless of header inclusion order.
6. Include alloca.h in narrow/wide scanf explicitly; freestanding Clang does
   not supply GCC's implicit alloca builtin declaration.
7. Check an active pthread_once initializer before normal exit, so its forbidden
   exit path aborts the process. _exit/_Exit retain their immediate behavior.
8. Disable shared-x86 fenv's CPUID-based SSE path for CiukiOS and initialize
   its unused MXCSR field to zero. The [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
   and the frozen ABI require OS support for SSE; CPU support alone is insufficient
   while F2 keeps CR4.OSFXSR clear. The entire archive audit caught this inline
   assembly despite compiler -mno-sse flags.

Newlib additionally needs `_DEFAULT_SOURCE` for its internal math constants
under C17 and `asm=__asm__` for its existing GNU inline assembly spelling.
Its existing warning diagnostics are retained without making them fatal;
SDK runtime and applications use the frozen warning policy. Nano stdio/malloc,
small-reent and compiler reent TLS are disabled. Full floating/long-long stdio,
multithreading and retargetable locks are enabled. Newlib's internal file-position
and offset types are widened with the machine type overlay, and public types
are checked against the ABI. The separate system port supplies posix_memalign
and validates C17 aligned_alloc, since the generic POSIX directory is excluded.
UTC-only timezone hooks prevent newlib from applying a user-supplied TZ offset.

## Evidence and integration limits

The build runs its wrapper self-test and host checks. Both hello-world and
libc_smoke are inspected with llvm-readelf/llvm-objdump and the production
opcode classifier, permitting x87 while rejecting SSE/MMX. Tests check three
separate page-aligned RX/R/RW LOADs, a non-executable GNU_STACK, static ELF32,
no undefined symbols, no dynamic/INTERP/TLS segments and all 53 generated F2
stubs. The ABI and pthread layouts compile as native `-m32` static assertions
where available, otherwise as target assertions. This sandbox rejects native
i386 syscall execution; pthread behavior is tested natively on x86-64 with
low-address mock memory, preserving the real u32 wait words and wire records.
The production implementation is tested for EINTR retry, timeout reacquisition,
sequence wrap, once publication, normal/recursive mutex errors, stale keys,
the 64-key limit, four destructor passes and positive pthread errors without
changing errno. Integer compiler helpers also run against native division and
remainder over boundary cases and 1000 deterministic generated vectors.

`build/tools/ciuki-sdk/tests/host-checks.json` records ELF hashes, sizes,
LOAD memory and per-program compile/link times. `manifest.json` records the
complete clean SDK build time and installed bytes. `libc_smoke.c` exercises
stdio, allocator/alignment, strings/conversions, x87 math, setjmp/longjmp,
per-thread errno, environment, files/directories, callbacks, clocks/sleeps and
excluded calls. Its heap figure is the observed sbrk/brk high-water, not an
invented workload size. Stack commitment and kernel memory ledgers must be
measured by the integrated controller. Nothing here qualifies those resources,
the Lua gate, QEMU, hardware or all successful/negative syscall cases.

Call 3 accepts bounded printable ASCII in the current F0 dispatcher. The payload
reports case/count/error fields; the F2 controller must supply CIUKI_TEST framing,
run/sequence/probe identity and terminal status. The contracts specify ownership
and the 240-byte limit but no more detailed F2 payload schema. The supplied
payload deliberately generates no sequence numbers or controller terminal records;
the lead must confirm its field framing during f2-05 integration.

The exact copied ABI header is GPL-2.0-only. SDK-authored runtime code is MIT
licensed and newlib has per-file notices. The lead must resolve that public-header
licensing boundary before claiming that the whole SDK header set is permissive;
this directive forbids changing the ABI header. No kernel file was changed, no
QEMU/systemd command was run by the implementer, and no git operation was used.

## f2-06 clean build figures

The clean build and mandatory checks completed in **17.483 seconds** with
`--jobs 2`. Installed sysroot files total **6,716,801 bytes**. The complete SDK
output, including retained upstream source, objects, notices and tests, occupies
approximately **121 MiB** on disk.

| Program | Compile/link seconds | Text | Data | BSS | ELF bytes including debug | Page-rounded LOAD bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| hello-world | 0.0566 | 11099 | 1472 | 1632 | 304324 | 20480 |
| libc_smoke | 0.0845 | 80908 | 1900 | 2528 | 551600 | 94208 |

Manifest SHA-256:
`aed2cf232525151f2302eeccef5c0ae7bdd2acdb513ff9ea4d95a5a24475fb64`.
It contains eight patch hashes, 1,198 newlib source/header notice entries,
and complete notice references for every linked archive member in both programs.

The wrapper self-test, both ELF inspections, entire runtime/compiler archive
opcode audit, target/native-m32 type assertions, native pthread/helper model and
all-53-stub coverage passed. A wrong local archive digest was rejected with exit
2 before changing the existing SDK manifest. Manifest source, installed-file and
notice hashes were checked against their current bytes. Guest results are
`not_run`.

Files delivered: `sdk/` (build recipe and patches, CRT, libciuki, libpthread,
headers, linker script, compiler wrapper, test sources/checker and notices),
`scripts/build_sdk.sh`, `Makefile`'s sdk target, `config/sdk-pins.json`, and this
guide. The complete SDK file list and hashes are in `manifest.json:sdk_sources`.
The lead owns the development diary entry and integration review.

## Lua application build (f2-07)

After building the SDK, build the pinned Lua archives offline:

```sh
make lua LUA_SOURCE_ARCHIVE=/path/to/lua-5.4.8.tar.gz \
  LUA_TESTS_ARCHIVE=/path/to/lua-5.4.8-tests.tar.gz
```

`make build-full` orders kernel, SDK, Lua and image generation. SDK source,
recipe, ABI and archive timestamps trigger the SDK build; Lua additionally
checks hashes of the SDK manifest, archives, recipe, supplement and outputs.
The default archive directory is `build/downloads/newlib/`. `SDK_ARCHIVE`,
`SDK_ARGS`, `LUA_SOURCE_ARCHIVE`, `LUA_TESTS_ARCHIVE` and `LUA_ARGS` can select
local inputs; neither recipe downloads. SDK, Lua and image recipes use the
existing capped scope. Under the directive's sandbox exception the implementer
runs `python3 apps/lua/build_lua.py --source-archive PATH --tests-archive PATH
--jobs 2` directly. All scratch files remain in the worktree.

Lua 5.4.8 uses upstream's generic configuration, 64-bit integers/double numbers,
`LUA_COMPAT_5_3`, no C89/Linux/POSIX extension switches, readline or dlopen, and
no source patches. Both Lua and luac pass `sdk/tests/check_sdk.py`'s static
ELF32/three-LOAD/undefined-symbol and x87/SSE/MMX checks. The recipe records
archive, SDK, upstream file, supplement and ELF hashes in
`build/apps/lua/manifest.json`. The image builder rejects stale or edited
payloads and records every image file in `build-manifest.json:payloads`, with
mtools read-back T1 checks. See [the Lua port notes](../apps/lua/README.md)
for the research sources, license locations and supplement output.

The F2 controller must run these as separate guest processes, with only fd 0–2
inherited, finite stdin, bounded stdout/stderr and a writable test overlay:

```sh
# cwd /system/tests/lua-5.4.8-tests
# LC_ALL=C TZ=UTC0 HOME=/home TMPDIR=/tmp
/bin/lua -e '_U=true' all.lua
/bin/lua /system/tests/ciuki-f2.lua
```

Upstream `final OK !!!`, normal exit zero, and all four supplement case results
are required. Only `_U`'s documented exclusions are permitted. Complete/internal
test modes are `excluded_by_contract`. A native host build of the same sources
running this portable suite is `host-reference` only; compilation, static image
checks and host-reference results do not qualify the F2 application gate.

## f2-07 host and static evidence

The SDK prerequisite completed with `--jobs 2` in 17.807 seconds, including
its mandatory host checks. Lua's final build and both SDK ELF inspections
completed in 2.256 seconds. Neither archive nor upstream source/test file was
patched. A second invocation reported unchanged input/output hashes without
rebuilding.

| Program | Text | Data | BSS | ELF bytes including debug | Page-rounded LOAD bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Lua | 300898 | 1952 | 2732 | 1598312 | 311296 |
| luac | 193634 | 1856 | 1648 | 1199216 | 200704 |

Lua ELF SHA-256:
`6c06de1d07cee4ce3c0781edc4d1e77161e5c35eec71882240c2375b4918e20c`.
luac ELF SHA-256:
`b4abd86de5c0a808dd0e13f73a8e762560259fca3a56e8411cdf199993d306a5`.
Supplement SHA-256:
`fe909e6f7fca9a8421434dac2dd70a068bb8b849d071fdbdce890bd111744953`.

`python3 -m unittest discover -s tests/host -p test_image_payloads.py -v`
passed all five tests. They cover payload inventory/manifest completeness,
actual FAT long-name and binary/empty-file read-back, rejection of missing,
changed and wrong-sized files, archive path/link rejection, digest failure
before replacing an existing build, and stale upstream-test rejection.

The native **host-reference** build used the same pinned, unmodified Lua
sources and generic int64/double/compatibility configuration. Its sole Lua test
switch was `_U=true`. It exited zero with `final OK !!!`; its merged stdout/stderr
was 7830 bytes, SHA-256
`8ffd2ec845f9442f9c64a1e9771b453fbfd482e666b91ea52a6d0fce5b72f3ab`.
No additional file-test skip was reported. The supplement also exited zero,
with all four `ok=1` cases and the four console lines in the prescribed order;
its merged output was 371 bytes, SHA-256
`82dfb72d503a9cbfbdf7be099db0f0f981f7b6273de84fc4c6d5524f9708e331`.
The compared final file's FNV-1a-32 checksum was `f6671c1c`.
Because glibc hard-codes `/tmp` for tmpnam/tmpfile, a native-only interposer
redirected those two libc calls into the worktree's temporary directory.
It did not change Lua sources, configuration or skip flags, and is not part
of the target port. Host-reference build/interposer scratch was removed.

The static image check reused the lead's existing kernel ELF, SHA-256
`f625c53cca29686c9d3a021164d9c3b64e7893f0b64873b13a5430eeb39471ed`.
The 536870912-byte sparse image passed the existing MBR/FAT32/fsck/loader T1
checks and all 50 file read-back checks. Its SHA-256 was
`e20ab437314be83722a5cb512d1b2c1995620e030ea0618421c310a3793b67e8`.
Its payloads are the three `/bin` programs, kernel and boot options, the Lua,
newlib and SDK license notices, the supplement, and all 41 original upstream
test files. Empty `/tmp`, `/home` and upstream `libs/P1` directories were also
checked. The exact path/hash/size inventory is in `build-manifest.json`.
Git identity was replaced with `unknown` only for this sandbox invocation,
so no git command was executed. The production manifest recipe retains normal
Git provenance and now includes SDK and application sources in its dirty check.

No new f2-07 contract change is needed. The f2-06 public-header licensing issue
documented above remains lead-owned. No QEMU, systemd scope, commit or push was
run here. Clean capped `make build-full`, F0/F1 regression gates, F2 guest Lua
evidence, resource measurements, diary entry and integration review remain
lead-owned; these results establish host/static evidence only.
