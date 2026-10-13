# F5 Win32 personality: Windows games of 1996–2004 on CiukiOS

Author: Claude (for the lead). Directive: f5-00. Date: 2026-10-13.
Status: draft for the lead's review, the Codex cross-review and the owner's
decisions listed under "Open questions for the owner". No code, build, QEMU
or hardware result is claimed by this document.

## Scope and authority

**[F5]** marks a requirement that becomes mandatory in step F5 and stays
binding afterwards; **[F6]** and **[F7]** mark the parts of this contract that
depend on those steps. MUST identifies a requirement, SHOULD a
recommendation and MAY an option. Normative tables MUST be read as
requirements, including their enumerated values and exclusions. Numbers,
limits, layouts and budgets below are proposed CiukiOS policy informed by
Microsoft's documentation; they are not Microsoft requirements and not
measurements.

**[F5]** Implementations MUST follow this document together with
[execution-abi.md](execution-abi.md) (F2 extension and its amendments),
[posix-subset.md](posix-subset.md), [vfs-storage-contract.md](vfs-storage-contract.md),
[device-firmware-ownership.md](device-firmware-ownership.md),
[f4-audio-media-contract.md](f4-audio-media-contract.md),
[boot-memory.md](boot-memory.md), [hardware-baseline.md](hardware-baseline.md),
[test-architecture.md](test-architecture.md), the desktop protocol in
[../desktop.md](../desktop.md) and, for networking, the F6 draft
[network-contract.md](network-contract.md). The F0–F4 QEMU gates MUST keep
passing on every integrated image. The native F2 ABI MUST NOT change for
native ELF programs: every kernel addition in this contract is additive and
applies only to processes whose personality is `win32`.

The owner's decision of 2026-10-13 (dev diary 2026-10-13-01) makes F5 the
Win32 personality — "an API Win32 compatible and working, to install and run
the Windows games of the era without additional steps" — and makes F8 the
optional native ports. It supersedes the phase assignment of the F5 and F8
rows in posix-subset.md's consumer table; those rows remain a historical
record of the F2 boundary.

**Directive numbering.** `f5-01` already exists
([f5-01-native-ide-and-compiler.md](../directives/f5-01-native-ide-and-compiler.md),
Tiny C Compiler and CiukIDE, assigned under the earlier F5 scope and merged
on 2026-10-12). To avoid overwriting it, this contract numbers its
directives **f5-02 to f5-18**. The lead MAY renumber them (question Q1).

## Goal and non-goals

### Goal

- **G1.** A player installs a Windows game of 1996–2004 with its original
  installer — from a CD, an ISO image or a downloaded setup program — and
  plays it from the Start menu, as on Windows 98 SE or ME, with no extra
  configuration step. Uninstalling through the Control Panel works when the
  game registered an uninstaller. The game data stays the player's copy.
- **G2.** PE32 executables and DLLs for i386 (`IMAGE_FILE_MACHINE_I386`), GUI
  and console subsystems, linked by the compilers and linkers of the era
  (Microsoft Visual C++ 4.x–7.x, Borland/Inprise, Watcom, Delphi, MinGW) and
  compressed by the common executable packers when they only use documented
  APIs.
- **G3.** The API surface games of the era used: kernel32, user32, gdi32,
  advapi32, winmm, DirectDraw 1–7, DirectSound 1–8 (playback), DirectInput
  3–8 (keyboard and mouse), the common controls, common dialogs and shell
  functions installers need, COM in-process servers, version and INI APIs.
  Direct3D 7/8 and OpenGL are F7; Winsock and DirectPlay are F6.
- **G4.** One canonical image for every machine (owner decision 2026-10-11);
  128 MiB is the reference RAM for the gate.
- **G5.** Containment: a Win32 crash, hang or exit never takes down the
  desktop, another process or a DOS VM, and always restores the desktop's
  display mode and input.
- **G6.** Honest diagnostics: every missing DLL, export or unimplemented
  behaviour is reported by name, on screen and in the serial/boot log.

### Non-goals

- **N1.** General Windows desktop compatibility: office suites, browsers,
  development tools, .NET, Java, services, multiple users, access control.
  Security APIs succeed without enforcement, as on Windows 9x.
- **N2.** Kernel-mode code of any kind: VxDs, `.sys` drivers, `DeviceIoControl`
  to drivers, ring-0 copy protection (SafeDisc, SecuROM, StarForce) and
  anti-cheat. Retail titles that require them are out of scope; the
  personality MUST fail such a request with a named diagnostic and MUST NOT
  emulate or bypass a protection scheme.
- **N3.** Win16 (NE) executables, including 16-bit installer stubs, unless
  the owner answers question Q3 otherwise. DOS programs keep running in DOS
  windows (F3); starting them from a Win32 process is a MAY (f5-15).
- **N4.** Windows binary drivers (display, audio, printer, codec drivers
  shipped as VxD or `.drv`).
- **N5.** NTFS-specific semantics, alternate streams, ACLs, junctions; 64-bit
  images; the NT native API as a public application interface.
- **N6.** Windows Installer (MSI), Windows Update, Internet Explorer and HTML
  rendering, DirectShow filter graphs beyond MCI AVI playback, DirectMusic
  and DirectPlay voice (DirectMusic MAY follow the gate if a chosen title
  needs it).
- **N7.** Physical port I/O, physical memory access and DMA by applications.
  Selected ports MAY be emulated by a shim (W20); none reaches hardware.
- **N8.** Printing, TAPI, MAPI, scanners, joystick and gamepad hardware (no
  CiukiOS driver exists; DirectInput reports no game controllers).
- **N9.** Game payloads in the canonical image or in any release artefact.
- **N10.** Bit-exact reproduction of Windows 98 internals: the 9x shared
  arena, VxD services, Win16Mutex, thunks, the 9x heap layout.
- **N11.** Performance parity with Windows 98 on identical hardware is a
  measurement recorded for every title, not by itself a gate (question Q7).

## Sources

Primary references checked on 2026-10-13. The decisions below cite them
where they rely on them; anything not backed by a source here is labelled
as CiukiOS policy.

| Source | Used for |
| --- | --- |
| Microsoft, [PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format) | Headers, sections, base relocations, import/export/delay-load/TLS/resource/load-config directories, alignment rules. |
| Microsoft, [DLL search order](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order), [DLL redirection](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-redirection), [DllMain](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-entry-point-function) | Search order with safe DLL search mode off (era behaviour), KnownDLLs, loader-lock rules. |
| Microsoft, [Thread Local Storage](https://learn.microsoft.com/en-us/windows/win32/procthread/thread-local-storage), [Fibers](https://learn.microsoft.com/en-us/windows/win32/procthread/fibers), [Process and thread functions](https://learn.microsoft.com/en-us/windows/win32/procthread/process-and-thread-functions) | TLS slots, fibers, process/thread lifetime. |
| Microsoft, [Structured exception handling](https://learn.microsoft.com/en-us/windows/win32/debug/structured-exception-handling), [Exception dispatching](https://learn.microsoft.com/en-us/windows/win32/debug/exception-dispatching), [EXCEPTION_RECORD](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record), [CONTEXT](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-context), [TEB](https://learn.microsoft.com/en-us/windows/win32/api/winternl/ns-winternl-teb) | Exception records, contexts, dispatch order, documented TEB offsets. |
| Microsoft, [VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualProtect](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [Memory limits for Windows releases](https://learn.microsoft.com/en-us/windows/win32/memory/memory-limits-for-windows-releases) | Reserve/commit, 64 KiB granularity, protections, 2 GiB user space for 32-bit processes. |
| Microsoft, [Wait functions](https://learn.microsoft.com/en-us/windows/win32/sync/wait-functions), [Asynchronous procedure calls](https://learn.microsoft.com/en-us/windows/win32/sync/asynchronous-procedure-calls), [MsgWaitForMultipleObjects](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjects), [Process security and access rights](https://learn.microsoft.com/en-us/windows/win32/procthread/process-security-and-access-rights) | Waitable objects, 64-handle waits, alertable waits, message waits. |
| Microsoft, [Registry](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry), [Registry hives](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-hives), [Uninstall registry key](https://learn.microsoft.com/en-us/windows/win32/msi/uninstall-registry-key), [Application registration (App Paths)](https://learn.microsoft.com/en-us/windows/win32/shell/app-registration), [GetPrivateProfileString](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprivateprofilestring) | Keys installers and games use; INI files. |
| Microsoft, [Naming files, paths and namespaces](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file), [GetShortPathName](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getshortpathnamew), [GetDiskFreeSpace](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespacea), [MoveFileEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexa) | Path syntax, short names, free-space arithmetic, delayed replacement. |
| Microsoft, [OSVERSIONINFOA](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-osversioninfoa), [CreateToolhelp32Snapshot](https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-createtoolhelp32snapshot), [ExitWindowsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-exitwindowsex), [QueryPerformanceFrequency](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancefrequency), [timeBeginPeriod](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod), [Multimedia timers](https://learn.microsoft.com/en-us/windows/win32/multimedia/multimedia-timers) | Version identity, process snapshots, restart requests, timing. |
| Microsoft, [Windows GDI](https://learn.microsoft.com/en-us/windows/win32/gdi/windows-gdi), [DirectDraw](https://learn.microsoft.com/en-us/windows/win32/directdraw/directdraw), [DirectSound (legacy)](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416960(v=vs.85)), [DirectInput (legacy)](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416842(v=vs.85)), [Winsock 2](https://learn.microsoft.com/en-us/windows/win32/winsock/windows-sockets-start-page-2) | API semantics of GDI and the DirectX 7/8-era components. The DirectX 7.0 and 8.1 SDK help files are the authoritative period documents; the import directive f5-02 MUST record which archived copies were consulted. |
| R. Chen, [mapping the same shared memory twice](https://devblogs.microsoft.com/oldnewthing/20120127-00/?p=8433) | Windows 95 and NT return different pointers for repeated views; neither may be relied upon. |
| Internet Archive items listed in "Acceptance set" | Installer files, sizes and SHA-1 digests published by the archive; installer engines identified on 2026-10-13 from the first MiB of each file. |

The Microsoft pages above are current documentation: where Windows 98
behaved differently from what they describe, the difference MUST be
established from the period SDK documentation or by a recorded experiment on
Windows 98 SE, and noted in the directive's report before it is implemented.

## Repository comparison

Checked on `main` `0d0a73ea` with bounded reads of the contracts and of
`src/kernel/arch/desc.c`, `src/kernel/core/fpu.c` and
`src/kernel/include/ciuki/abi.h`. These are existing mechanisms, not
evidence of Win32 support.

| Existing mechanism | Consequence for F5 |
| --- | --- |
| execution-abi.md "Native executable format": ELF32 `ET_EXEC` only; "PE32 is not accepted by the native loader; it is a later compatibility personality (decision D4)". | The kernel recognises PE at spawn and creates a `win32` process (W1); the ELF loader is unchanged. |
| Native layout: user range `0x00000000–0xBFFFFFFF`, ELF at `0x00400000–0x0FFFFFFF`, heap below `0x20000000`, mmap arena `0x20000000–0xBFBFFFFF`; mmap only private anonymous, no `MAP_FIXED`, no W+X, no file-backed or shared mappings. | A separate Win32 virtual-memory service with fixed placement, reserve/commit, W+X, guard pages and section views (W8, W9). F2 mmap/brk are refused in `win32` processes. |
| `desc.c`: GDT index 7 is the per-thread GS TLS descriptor (selector `0x3b`), index 8 the per-VM LDT (`_Static_assert(ARCH_LDT_GDT_INDEX == CIUKI_TLS_GDT_INDEX + 1)`). | A per-thread FS descriptor for the TEB at GDT index 9, selector `0x4b` (W5). The F0 protection probe's GDT-size expectation changes accordingly. |
| `fpu.c`: CR4.OSFXSR/OSXMMEXCPT cleared ("F0 policy: SSE stays disabled for ring 3"); F2 ucontext carries a 108-byte FNSAVE image and "User SSE MUST stay disabled". | SSE is enabled per process for `win32` processes only, with XMM state in FXSAVE images (W6). Native behaviour, including #UD on SSE, is unchanged. |
| F2 signals: one active handler per thread; a synchronous fault inside a handler terminates the process; sigreturn restricts editable state. | Windows exception dispatch nests freely and resumes through a full context. A dedicated delivery path replaces signals for synchronous faults in `win32` processes (W7). |
| `abi.h`: calls 0–5 and 16–77 assigned (77 `DISPLAY_ACCEL`). The F6 draft proposes 75–87, already colliding with 75–77. | One multiplexed call `CIUKI_SYS_W32` with versioned records (W2); its number is assigned by the lead when f5-03 freezes it, after the F6 renumbering. |
| F2 limits: 64 processes, 256 threads system-wide, 16 threads and 128 fds per process, 1024 open descriptions system-wide. | Win32 handles live in their own per-process table and kernel object ledger (W10). The per-process thread limit for `win32` processes is raised to 32, subject to the memory measurement in f5-04. |
| Desktop protocol v1/v2.1: one window per channel, 16 channels, ASCII titles of 1–63 bytes, client-local absolute motion, no cursor control, no full-screen request. | Desktop protocol v3 (f5-09): several windows per channel, UTF-8 titles, client placement, styles, cursor shape/visibility/capture/clip/warp, relative motion, full-screen lease. |
| Call 74 (f1-23): query is public, mode set only by the desktop; modes 640×480–1600×1200, 16/24/32 bpp, linear only; 10 s switch watchdog. Presenter: synchronous XRGB8888 copy, no vsync. Savage 2D acceleration compile-time opt-in since `03bf0427`. | Games never set modes themselves: the desktop switches on their behalf under a revocable lease; 8 bpp and low resolutions are emulated by the presenter (W15). |
| `/dev/audio` (f4-01): at most eight streams, 2048-frame rings, U8/S16LE mono/stereo, 8–48 kHz, default S16LE stereo 48 kHz; ≤60 ms latency budget. | Each Win32 process mixes all its DirectSound/waveOut sources in user mode into one stream (W16). |
| Input events: set-1 scan codes, E0 keys OR `0x100`, Pause `0x200`, relative MOTION from the kernel; the desktop converts motion to client-local coordinates; `keymap.c` has US and Italian layouts. | DirectInput key codes are derived from set-1 codes; relative motion is forwarded under capture (W17). |
| VFS: 9x-style drive letters, case-insensitive and case-preserving names, MAX_PATH 259, CP437 short names generated with the fatgen103 `~N` algorithm, share modes, byte-range locks, open descriptions. The POSIX view rejects `\` and `:`. | kernel32 uses a drive-path file service with share modes (f5-04), not the POSIX path view. Lookup by the on-disk 8.3 alias MUST be verified (W12). |
| F4: ATAPI and ISO 9660 read-only volumes with drive letters; NTFS read-only. | CD and ISO installs use the existing drive letters; `GetDriveType` reports `DRIVE_CDROM`. |
| Call 72: DOS-window authority by exact path `/Programs/dosbox` and parent. Calls 70/71: process list and termination authority. | The same exact-path rule grants service authority to `w32srv`; Task Manager lists and ends Win32 processes without change. |

## Decisions

The decisions are numbered W1–W24 to avoid confusion with the foundations
decisions D1–D11.

- **W1 — Personality, not emulation.** **[F5]** A Win32 program MUST run as
  an ordinary CiukiOS process whose personality is `win32`, natively on the
  CPU, scheduled, isolated and accounted like any other process. The kernel
  MUST select this personality at spawn when the file has a valid `MZ`
  header whose `e_lfanew` designates `PE\0\0` with machine `0x14C` and
  optional-header magic `0x10B`; every other file keeps the existing ELF path
  and errors. No CPU emulator, second kernel or virtual machine is involved.
- **W2 — Layers.** **[F5]** The personality MUST consist of: (a) additive
  kernel services reached through one multiplexed system call
  `CIUKI_SYS_W32` with versioned, size-checked records; (b) user-mode system
  DLLs in PE format, loaded from a read-only builtin store; (c) one native
  service process `w32srv`; (d) desktop protocol v3. Kernel additions MUST be
  limited to what cannot be done safely in user mode: address-space control,
  the FS descriptor, exception delivery, waitable objects and handles,
  section objects, service endpoints and presentation leases.
- **W3 — Version identity.** **[F5]** The personality MUST report Windows 98
  Second Edition: major 4, minor 10, build 2222, platform
  `VER_PLATFORM_WIN32_WINDOWS`, CSD string `" A "`, and `GetVersion()` with
  bit 31 set. A shim MAY report Windows 95, ME, NT 4.0, 2000 or XP per title
  (W20). The DirectX version reported before F7 is question Q4.
- **W4 — 2 GiB address space.** **[F5]** Win32 processes MUST see the layout
  in "Address space": application memory below `0x70000000`, builtin DLLs in
  `0x70000000–0x7FFDFFFF`, nothing at or above `0x80000000`.
  `IMAGE_FILE_LARGE_ADDRESS_AWARE` MUST be ignored.
- **W5 — TEB through FS.** **[F5]** Each Win32 thread MUST run with FS
  selecting a per-thread DPL 3 data descriptor at GDT index 9 (selector
  `0x4b`) whose base is that thread's TEB; the kernel reloads it on every
  switch, like the GS descriptor of F2.
- **W6 — SSE for Win32 processes.** **[F5]** When CPUID reports FXSR and SSE,
  CR4.OSFXSR and CR4.OSXMMEXCPT MUST be set while a `win32` thread runs and
  cleared while a native thread runs; XMM and MXCSR state MUST be preserved
  across switches. DirectX 6.1 and the games of 1999–2004 select SSE code
  paths ([AnandTech, 1999](https://at-web1.www.anandtech.com/show/254/6));
  some check only CPUID, which an application can always read on these
  processors, and would fault on #UD. Whether Windows 98 SE itself set
  CR4.OSFXSR is not established by a primary source found on 2026-10-13;
  f5-03 MUST record the answer (period documentation or a recorded
  experiment) without changing this requirement.
- **W7 — Windows exception dispatch.** **[F5]** Synchronous faults in a
  `win32` thread MUST be delivered as Windows exceptions: the kernel writes an
  `EXCEPTION_RECORD` and a `CONTEXT` on the faulting thread's stack and
  resumes it at the personality's user-mode dispatcher, which walks the
  FS:[0] handler chain. Continuing MUST go through a "continue with context"
  operation. No signal-handler state is involved, so nested exceptions and
  handlers that fault are ordinary.
- **W8 — Win32 virtual memory.** **[F5]** A separate virtual-memory service
  MUST implement reservation and commitment at caller-chosen addresses with
  64 KiB allocation granularity and 4 KiB pages, the Win32 protections
  including execute-read-write and guard pages, and exact region queries. The
  F2 rule against W+X does not apply to `win32` processes: on these non-PAE
  processors any readable page is executable, and the games and packers of
  the era write code at run time.
- **W9 — Sections.** **[F5]** The kernel MUST provide section objects: image
  sections (one per builtin DLL file, read-only pages shared by every Win32
  process), pagefile-backed shared memory (named or unnamed), and file views.
- **W10 — Kernel objects.** **[F5]** Handles, waitable objects (events,
  mutexes, semaphores, waitable timers, processes, threads, message-queue
  objects), the named-object namespace, APC queues and thread suspension
  MUST live in the kernel. Waits on several objects, including "wait all",
  MUST be atomic in the kernel.
- **W11 — `w32srv`.** **[F5]** A native service process MUST own the registry,
  global atoms, the top-level window directory and window-handle allocation,
  the shell DDE service and the virtual restart. It is not on any wait or
  message hot path of a running game.
- **W12 — File-system view.** **[F5]** Win32 paths MUST map directly onto the
  existing VFS drive letters. The image builder MUST create `C:\WINDOWS`
  and `C:\Program Files` as listed in "File-system view".
- **W13 — Registry storage.** **[F5]** Hives MUST be stored on the system
  volume in a CiukiOS text format with two checksummed slots per hive, never
  in the Windows 9x or NT binary hive formats, which no game reads directly.
- **W14 — Builtin store.** **[F5]** The personality's DLLs MUST live in the
  read-only store `C:\System\Win32\` and MUST take precedence over files of the
  same name anywhere else (KnownDLL semantics), so an installer copying
  `ddraw.dll` or running a DirectX setup cannot replace them.
- **W15 — Presentation.** **[F5]** Games draw into system-memory surfaces that
  the kernel presenter copies to the framebuffer: through the desktop for
  windowed games, and directly from the game under a revocable present lease
  for full-screen exclusive games. 8 bpp palettised and low-resolution modes
  are emulated by the presenter. Mapping the framebuffer into a game is not
  part of the baseline (question Q5).
- **W16 — Audio.** **[F5]** Each Win32 process MUST mix its DirectSound and
  waveOut sources in user mode into one `/dev/audio` stream. MIDI output MUST
  use a software synthesizer selected under question Q6.
- **W17 — Input.** **[F5]** Keyboard input MUST keep the set-1 code space end
  to end: window messages derive scan codes and virtual keys from it,
  DirectInput key codes equal set-1 codes with E0 keys `|0x80`. Relative
  mouse motion MUST reach a process that holds capture or exclusive mouse
  access.
- **W18 — Time.** **[F5]** `QueryPerformanceFrequency` MUST return 1,193,182
  (the 8254 input clock) and the counter MUST derive from the PIT, never from
  the time-stamp counter, whose rate changes with SpeedStep on the T23's
  Pentium III-M. `GetTickCount` and `timeGetTime` MUST have 1 ms resolution.
- **W19 — Installers.** **[F5]** InstallShield, Wise, NSIS 2.x, IExpress
  with advpack, WinZip self-extractors and vendor installers MUST be
  supported for the acceptance set; MSI MUST be refused with a clear message;
  "restart Windows" MUST be a virtual restart of the personality.
- **W20 — Shims.** **[F5]** Per-title compatibility shims MUST be named,
  versioned and selected by executable identity from a read-only database,
  never by hidden code paths.
- **W21 — Diagnostics.** **[F5]** Missing DLLs and exports, unimplemented
  functions and unhandled exceptions MUST produce a named on-screen message
  in Windows 98 wording and a bounded `[win32]` record on serial and in the
  boot log.
- **W22 — Implementation.** **[F5]** The recommended option is the hybrid
  of "Implementation options": a CiukiOS-written core and selected reused
  open-source DLLs for the installer-facing long tail. The owner decides
  (question Q2).
- **W23 — Acceptance.** **[F5]** Ten named demo or shareware titles with
  freely downloadable installers form the gate; payloads never enter the
  image; each title installs, starts, plays and exits on QEMU and on both
  laptops.
- **W24 — Win16 excluded pending Q3.** **[F5]** An NE executable MUST be
  refused with "This program requires 16-bit Windows support, which CiukiOS
  does not provide" and a `[win32] refuse ne` record.

## Personality architecture

```text
 game.exe, setup.exe, application DLLs          (PE32, unmodified)
 ------------------------------------------------------------------
 builtin DLLs, C:\System\Win32\ (PE32, read-only, shared image pages)
   core: ntdll kernel32 user32 gdi32 advapi32 winmm ddraw dsound dinput
   upper: comctl32 comdlg32 shell32 shlwapi ole32 oleaut32 riched20
          cabinet advpack setupapi version lz32 msacm32 msvcrt ...
 ------------------------------------------------------------------
 CIUKI_SYS_W32 (kernel personality)   F2 calls (VFS, channels, audio,
   VM, FS/TEB, exceptions, objects,    surfaces, clocks, process list)
   sections, endpoints, leases
 ------------------------------------------------------------------
 w32srv (native ELF)   desktop (native ELF, protocol v3)   kernel
```

**[F5]** The core DLLs MUST reach the kernel only through `CIUKI_SYS_W32`
and the existing F2 calls, through the `int 0x80` gate, from code in the
builtin store. Application code MUST NOT need either: a game issuing
`int 0x80` itself gets whatever the F2 ABI does with its registers, as on
Windows a game issuing a private system call gets undefined results.
`int 2Eh` and `sysenter` from application code MUST raise
`EXCEPTION_ACCESS_VIOLATION` and `EXCEPTION_ILLEGAL_INSTRUCTION` respectively.

**[F5]** A Win32 process MUST NOT use newlib or the F2 SDK runtime. The core
DLLs are freestanding PE images built with the pinned clang (f5-02); they
call the kernel directly. The F2 descriptor table of a Win32 process exists
but is used only by the personality (files, channels, audio, surfaces).

**[F5]** `w32srv` MUST be a native F2 program at `/System/Win32/w32srv`,
started by the desktop at its own start when that file exists, and granted
service authority by the kernel only for that exact path with the
supervisor's desktop as parent (the rule of call 72). Its loss MUST NOT
terminate running Win32 processes: registry, window-directory and atom
operations then fail with `ERROR_SERVICE_NOT_ACTIVE` (1062) until the desktop
restarts it, and the restarted server reloads the last committed hives.

## Process, thread and object model

### Process creation and identity

**[F5]** Spawning a PE file — from the desktop, from Files, or through
`CreateProcess` — MUST create a `win32` process whose initial thread starts
in the builtin loader with a process-parameters block: image path, command
line (one string, as given), current directory, environment block,
`STARTUPINFO` fields, and the handles of the service endpoints (desktop,
`w32srv`). The kernel MUST NOT parse the command line. Failure before the
first instruction MUST leave no visible process, as F2 spawn requires.

**[F5]** Process and thread identifiers returned by the Win32 API MUST be the
CiukiOS PID and TID, so Task Manager, `[win32]` records and Toolhelp agree.
`GetCurrentProcess()` and `GetCurrentThread()` MUST return the pseudo-handles
`(HANDLE)-1` and `(HANDLE)-2`. Exit codes MUST be 32-bit values;
`GetExitCodeProcess` MUST return `STILL_ACTIVE` (259) while the process runs.
`TerminateProcess`, `ExitProcess` (with `DLL_PROCESS_DETACH` notifications
on the normal path only) and termination by Task Manager MUST converge on the
existing idempotent teardown, then close every Win32 handle, release
sections, leases, endpoints and audio streams.

**[F5]** `CreateProcess` MUST support `bInheritHandles` with per-handle
inheritance flags, `CREATE_SUSPENDED`, `CREATE_NEW_CONSOLE` (ignored),
priority-class flags, an explicit environment block and current directory,
and the module-name/command-line search rules of the period documentation
(application directory, current directory, system directory, Windows
directory, `PATH`; `.exe` appended when absent). Starting an ELF file
through `CreateProcess` MUST start it as a native process with no inherited
Win32 handles. A DOS `MZ` file without a PE header MUST fail with
`ERROR_BAD_EXE_FORMAT` (193) unless the DOS-window launch MAY of f5-15 is
implemented.

### Threads, TEB and FS

**[F5]** Each Win32 thread MUST be one kernel thread with a TEB of 8 KiB
(two pages) allocated by the kernel in the application region and mapped
read-write. The FS descriptor MUST have base = TEB and limit = 8191. At
least the following documented offsets MUST be valid; other fields are
private to the core DLLs:

| Offset | Field | Rule |
| --- | --- | --- |
| `0x00` | `ExceptionList` | Head of the SEH chain; `0xFFFFFFFF` terminates it. |
| `0x04`, `0x08` | `StackBase`, `StackLimit` | Updated as the stack is committed. |
| `0x10` | `FiberData` | Current fiber, or the TEB version value when not a fiber. |
| `0x14` | `ArbitraryUserPointer` | Free for the application. |
| `0x18` | `Self` | Linear address of the TEB. |
| `0x20`, `0x24` | `ClientId` | PID and TID. |
| `0x2C` | `ThreadLocalStoragePointer` | Array of static TLS blocks (compiler `__declspec(thread)` code reads it through `fs:[2Ch]`). |
| `0x30` | `ProcessEnvironmentBlock` | Process block (below `0x70000000`). |
| `0x34` | `LastErrorValue` | `GetLastError`/`SetLastError`. |
| `0xE10` | `TlsSlots[64]` | `TlsAlloc` slots 0–63. |

The first seven fields are the `NT_TIB` layout shared by Windows 95/98 and
NT; offsets `0x2C`, `0x30` and `0xE10` follow the documented `TEB`
structure. Windows 9x thread blocks differed beyond `NT_TIB`; any title that
depends on a 9x-only field MUST be handled by a named shim after the
difference has been established (see "Sources").

**[F5]** Thread creation MUST reserve the stack from the PE header
(`SizeOfStackReserve`, default 1 MiB, rounded to 64 KiB) or from the
`CreateThread` argument, commit `SizeOfStackCommit` plus one guard page, and
grow the commitment on access to the guard page until one page remains,
which then raises `EXCEPTION_STACK_OVERFLOW` (`0xC00000FD`). Initial x87
control word MUST be `0x027F` (53-bit precision, all exceptions masked) and
MXCSR `0x1F80`. `TlsAlloc` MUST provide 64 slots; 9x had 80 and the NT
expansion slots are not required.

**[F5]** Suspension (`SuspendThread`/`ResumeThread` with counts up to 127),
`GetThreadContext`/`SetThreadContext` on a suspended thread of the same
process (integer, control, segment, x87 and extended-register flags;
debug-register flags accepted and ignored with zero values), and
`ExitThread`/`TerminateThread` MUST be implemented. Suspending a thread
MUST take effect at its next return to user mode and MUST NOT stop it inside
the kernel holding a lock.

### Handles and kernel objects

**[F5]** Every Win32 process MUST have a kernel handle table, separate from
its F2 descriptor table, with handle values that are non-zero multiples of
four, at most 4096 handles per process and a system-wide ledger of 8192
Win32 objects charged at creation. `CloseHandle` on an invalid handle MUST
fail with `ERROR_INVALID_HANDLE` (6) and MUST NOT raise an exception (9x
behaviour). `DuplicateHandle` MUST work within a process and between
processes when the caller holds a handle to both. Access masks MUST be
recorded and returned, but not enforced beyond the object's operations.

| Object | Required semantics |
| --- | --- |
| Event | Manual and auto reset; `SetEvent`, `ResetEvent`, `PulseEvent` (wakes current waiters only). |
| Mutex | Owner thread, recursion count, release by non-owner fails `ERROR_NOT_OWNER` (288), owner death makes the next wait return `WAIT_ABANDONED`. |
| Semaphore | Count and maximum; release beyond maximum fails `ERROR_TOO_MANY_POSTS` (298). |
| Waitable timer | Absolute or relative due time, period, manual/synchronization, optional completion APC. |
| Process, thread | Signalled at exit; exit code retained while a handle exists. |
| Message queue | One per thread with window or message activity; wake bits per `QS_*` class (below). |
| Section | Image, pagefile-backed or file-backed (see "Memory"). |
| File | A VFS open description with Win32 share and access flags. |
| Change notification | MAY: `FindFirstChangeNotification` signalled by VFS change hooks. |

**[F5]** Named events, mutexes, semaphores, waitable timers and sections
MUST share one case-sensitive, flat namespace of names up to 260 characters
(backslashes are ordinary characters; `Global\` and `Local\` prefixes are
stripped). Opening an existing name with `Create*` MUST return a handle to
it and set `ERROR_ALREADY_EXISTS` (183); a name used by an object of another
type MUST fail with `ERROR_INVALID_HANDLE`. Games use named mutexes to detect
a second instance; this MUST work across processes.

### Waits, APCs and message queues

**[F5]** `WaitForSingleObject(Ex)`, `WaitForMultipleObjects(Ex)` (1–64
handles, any or all) and `SignalObjectAndWait` MUST be kernel operations
with millisecond timeouts rounded up to the 1 ms timer quantum, `INFINITE`,
and results `WAIT_OBJECT_0+n`, `WAIT_ABANDONED_0+n`, `WAIT_TIMEOUT` (258),
`WAIT_IO_COMPLETION` (192) and `WAIT_FAILED`. "Wait all" MUST acquire every
object atomically or none. Auto-reset events, mutexes and semaphores MUST be
consumed only by the waiter that is satisfied. A wait MUST be interruptible
by process termination.

**[F5]** User APCs (`QueueUserAPC`, waitable-timer completion routines,
`ReadFileEx`/`WriteFileEx` completion routines) MUST be queued per thread and
run only when the thread performs an alertable wait (`SleepEx`, the `Ex` wait
functions with `bAlertable`, `MsgWaitForMultipleObjectsEx` with
`MWMO_ALERTABLE`), which then returns `WAIT_IO_COMPLETION` after all queued
APCs ran. APCs MUST run on the thread's own stack in user mode with no kernel
lock held.

**[F5]** The message-queue object MUST record wake bits for posted messages,
sent messages, input (keys, mouse moves, buttons), paint, timers and hot
keys. `MsgWaitForMultipleObjects(Ex)` MUST wait on the handles plus the
calling thread's queue with the requested `QS_*` mask, as one kernel wait.
`PostMessage` and `PostThreadMessage` to a thread of another process MUST
be a kernel queue operation of fixed-size records (window, message,
wParam, lParam, time, point); `SendMessage` to another process MUST queue a
sent-message record and wait for the reply while still dispatching messages
sent to the caller. Pointer parameters MUST NOT be marshalled across
processes except for `WM_COPYDATA`, `WM_SETTEXT` and `WM_GETTEXT`, whose
buffers (at most 64 KiB) the kernel copies. Each queue MUST hold at most
10,000 posted messages, the Windows default, and fail further posts with
`ERROR_NOT_ENOUGH_QUOTA` (1816).

### Fibers, TLS and scheduling

**[F5]** Fibers (`ConvertThreadToFiber`, `CreateFiber`, `SwitchToFiber`,
`DeleteFiber`, `GetFiberData`) MUST be implemented entirely in user mode by
swapping the stack, `ExceptionList`, `StackBase`, `StackLimit` and
`FiberData` fields of the TEB together with the callee-saved registers and
the x87 control word.

**[F5]** Win32 priorities MUST map onto the existing scheduler classes
without ever reaching `device`: idle class and `THREAD_PRIORITY_IDLE` to
`idle`; normal and high classes to `normal`; `THREAD_PRIORITY_TIME_CRITICAL`
and the realtime class to `interactive`. The threads of the process that owns
the focused window or a full-screen lease MUST run at `interactive`, like a
focused DOS VM, and return to their mapped class when focus leaves.
`Sleep(0)` and `SwitchToThread` MUST yield; `Sleep(n)` MUST NOT return
early. `SetProcessAffinityMask` MUST accept mask 1 and fail other masks.

## Memory

### Address space

**[F5]** A Win32 process MUST have this layout; the kernel half above
`0xC0000000` is unchanged:

| Range | Use |
| --- | --- |
| `0x00000000–0x0000FFFF` | No access (null-pointer region). |
| `0x00010000–0x6FFFFFFF` | Application region: EXE at its preferred base (usually `0x00400000`), application DLLs, heaps, stacks, TEBs, PEB, `VirtualAlloc`, section views, emulated surfaces. |
| `0x70000000–0x7FFDFFFF` | Builtin DLL region: image sections of the builtin store at fixed, non-overlapping bases recorded in the store manifest. |
| `0x7FFE0000–0x7FFE0FFF` | Shared user data page, read-only: tick count, interrupt time, system time, kept current by the kernel every millisecond. |
| `0x7FFE1000–0x7FFFFFFF` | No access. |
| `0x80000000–0xBFFFFFFF` | Never mapped in a Win32 process. |

**[F5]** An EXE without base relocations MUST load at its preferred base or
fail with `ERROR_BAD_EXE_FORMAT`; nothing else is mapped in the application
region before the EXE. The main thread stack MUST be placed below the EXE
when it fits in `0x00010000–ImageBase`, otherwise first-fit from the bottom.
`VirtualAlloc` without an address MUST use bottom-up first fit at 64 KiB
granularity (top-down with `MEM_TOP_DOWN`) inside the application region.

Windows 9x placed its system DLLs and shared views above 2 GiB; games of the
era therefore tolerate system code at high addresses. This contract keeps
everything below 2 GiB (W4) because titles built for Windows 2000 and XP
were never tested with pointers above 2 GiB. A 9x shared-arena shim is
deliberately not provided.

### Virtual-memory service

**[F5]** The kernel MUST implement these operations for the calling process;
the read and write rows MUST also work on another process through a process
handle (`ReadProcessMemory`, `WriteProcessMemory`):

| Operation | Required behaviour |
| --- | --- |
| reserve | Address 0 (placement as above) or a 64 KiB-aligned address; `ERROR_INVALID_ADDRESS` (487) on overlap. |
| commit | Pages inside a reservation; committing committed pages succeeds; contents zero on first commit. `MEM_RESERVE` and `MEM_COMMIT` together in one call. |
| decommit, release | Decommit any page range; release only a whole reservation from its base with size 0. |
| protect | `PAGE_NOACCESS`, `READONLY`, `READWRITE`, `WRITECOPY` (treated as read-write for private memory), `EXECUTE`, `EXECUTE_READ`, `EXECUTE_READWRITE`, `EXECUTE_WRITECOPY`, plus `PAGE_GUARD` and `PAGE_NOCACHE` (accepted, no effect). Returns the old protection of the first page. |
| query | `MEMORY_BASIC_INFORMATION` for free, reserved, committed, image and mapped regions, with allocation base, allocation protection, state, type and region size exact to the page. |
| read/write other process | Copy between processes for valid committed ranges; partial copies report bytes transferred. |

**[F5]** A guard page MUST raise `STATUS_GUARD_PAGE_VIOLATION`
(`0x80000001`) once and lose its guard attribute, except in the stack
reservation where the kernel moves the guard down transparently. Execute
protections MUST be recorded and reported but cannot be enforced on the
non-PAE processors of the baseline; no title may rely on enforcement.

**[F5]** Commitment MUST be charged against physical memory at commit time,
as F2 requires for anonymous mappings: no overcommit and no swap. Reserving
costs only address space and page-table metadata. `GlobalMemoryStatus` MUST
report total physical memory, available physical memory, a page-file total
equal to the commit limit, and 2 GiB of virtual address space; every field
MUST be capped at 2 GiB − 1 so that period arithmetic does not overflow.

### Sections, heaps and file mappings

**[F5]** Image sections: the kernel MUST map each builtin DLL from a single
section whose read-only pages are shared by all Win32 processes, and whose
writable pages are private copies on first write. Builtin DLLs MUST be
linked at their fixed base in the builtin region and therefore never
relocated. Application DLLs and EXEs MAY share pages when the same file
loads at the same base in several processes; this is a SHOULD measured in
f5-04, because installers spawn several processes that load the same
installer DLLs.

**[F5]** `CreateFileMapping`/`MapViewOfFile(Ex)`/`UnmapViewOfFile`/
`FlushViewOfFile` MUST support: pagefile-backed sections, named or
unnamed, shared coherently by every process that maps them; read-only file
views; and writable file views whose modified pages are written back on
`FlushViewOfFile`, unmap and process exit. Two processes MAY map the same
pagefile-backed section; the same file MUST NOT have two writable views in
different processes (`ERROR_ACCESS_DENIED`), a restriction recorded as a
departure. Views MUST be placed at 64 KiB granularity; `MapViewOfFileEx`
with an address MUST honour it or fail.

**[F5]** The process heap and `HeapCreate`/`HeapAlloc`/`HeapReAlloc`/
`HeapFree`/`HeapSize`/`HeapValidate`/`HeapWalk` MUST be implemented in the
core with serialization by default and `HEAP_NO_SERIALIZE` honoured;
`GlobalAlloc`/`LocalAlloc` MUST support fixed and moveable blocks with lock
counts and `GMEM_ZEROINIT`. Freed memory MUST NOT be zeroed or poisoned by
default (titles of the era read freed blocks; a shim MAY change this).

## Exceptions and processor state

**[F5]** A user fault in a `win32` thread MUST be translated as follows,
then delivered as in W7 to the dispatcher entry point the loader registered
for the process:

| CPU event | Exception code | Information |
| --- | --- | --- |
| `#PF` not present or protection | `EXCEPTION_ACCESS_VIOLATION` `0xC0000005` | `[0]` 0 read, 1 write; `[1]` CR2. |
| `#PF` on a guard page | `STATUS_GUARD_PAGE_VIOLATION` `0x80000001` | Same as above. |
| `#GP` from a privileged instruction (`cli`, `sti`, `hlt`, `in`, `out`, `ins`, `outs`, control/debug-register moves) | `EXCEPTION_PRIV_INSTRUCTION` `0xC0000096`, unless an I/O shim emulates the port | — |
| Other `#GP`, `#SS`, `#NP` | `EXCEPTION_ACCESS_VIOLATION` | `[0]` 0, `[1]` `0xFFFFFFFF`. |
| `#UD` | `EXCEPTION_ILLEGAL_INSTRUCTION` `0xC000001D` | — |
| `#DE` | `EXCEPTION_INT_DIVIDE_BY_ZERO` `0xC0000094` when the divisor is zero, `EXCEPTION_INT_OVERFLOW` `0xC0000095` otherwise | — |
| `#BP` (`int3`) | `EXCEPTION_BREAKPOINT` `0x80000003` | EIP of the `int3` byte. |
| `#DB` single step | `EXCEPTION_SINGLE_STEP` `0x80000004` | — |
| `#MF` | `0xC000008D`–`0xC0000093` from the x87 status word (denormal, divide by zero, inexact, invalid, overflow, stack check, underflow) | — |
| `#XM` | `STATUS_FLOAT_MULTIPLE_FAULTS` `0xC00002B4` or `..._TRAPS` `0xC00002B5` | — |
| `#AC` | `EXCEPTION_DATATYPE_MISALIGNMENT` `0x80000002` | — |

**[F5]** The `CONTEXT` MUST be the i386 layout with `CONTEXT_FULL`,
`CONTEXT_FLOATING_POINT` (108-byte x87 save area plus `Cr0NpxState`) and,
when FXSR is present, `CONTEXT_EXTENDED_REGISTERS` (512-byte FXSAVE image).
The kernel MUST deliver: the record and context on the user stack below the
interrupted ESP (aligned to 4), then EIP at the dispatcher with the two
pointers as arguments. If that stack area is not writable, the process MUST
terminate with exit code `0xC00000FD` and a `[win32] crash` record. The
dispatcher, not the kernel, MUST walk the handler chain, run unwinds,
`UnhandledExceptionFilter` and `SetUnhandledExceptionFilter` filters.

**[F5]** "Continue with context" MUST validate the context before restoring
it: segment registers MUST equal the thread's user selectors (FS the TEB
selector), EFLAGS may change only the arithmetic flags, TF, DF and AC, EIP and
ESP MUST be user addresses below `0x7FFE0000` or in the builtin region, and
x87/FXSAVE reserved fields and MXCSR reserved bits MUST be sanitized. An
invalid context MUST terminate only that process. `RaiseException` MUST use
the same delivery with the caller's context, without a CPU fault.

**[F5]** When a `win32` thread is the outgoing or incoming lazy-FPU owner,
the `#NM` handler MUST execute `FXSAVE`/`FXRSTOR` with CR4.OSFXSR set so that
XMM registers are saved and restored, and MUST then leave CR4 as the
incoming thread requires (W6). A native thread MUST still observe #UD on SSE
instructions. Asynchronous termination (Task Manager, owner death) MUST
continue to use the F2 teardown and never run user code.

## PE loader

The loader is user-mode code in the core `ntdll`; the kernel only creates
image sections and maps views. Every rule below MUST be covered by a
fixture of the PE fixture set (f5-02) before its directive closes.

### Accepted images

**[F5]** The loader MUST accept a PE32 image when: `e_magic` is `MZ`;
`e_lfanew` is 4-aligned and the headers lie within the file; the signature
is `PE\0\0`; `Machine` is `0x14C`; the optional header magic is `0x10B` and
`SizeOfOptionalHeader` covers the data directories it declares (at most 16);
`FileAlignment` is a power of two from 512 to 64 KiB, or equal to
`SectionAlignment` when that is below 4 KiB; `SectionAlignment` is a power
of two ≥ `FileAlignment`; `SizeOfImage` ≤ 512 MiB; section headers neither
overlap in virtual space nor exceed `SizeOfImage`; raw data lies within the
file or is zero-filled when beyond it; every RVA used by a directory, with
its size, lies inside the image without 32-bit wrap. Subsystem MUST be GUI
(2) or console (3); native (1), POSIX (7) and Windows CE subsystems MUST be
refused. `MajorSubsystemVersion` above 5 MUST be accepted with a log record,
not refused. Checksums and the `DllCharacteristics` flags of later Windows
versions MUST be ignored. Section characteristics MUST give each page its
protection; a writable and executable section MUST map read-write-execute.

**[F5]** Malformed images MUST fail with `ERROR_BAD_EXE_FORMAT` (193) and a
`[win32] load` record naming the first failed check, never with a crash of
the loader or a partially initialised process. Images whose sections overlap
the headers or each other, executable packers' unusual but valid layouts
(raw size 0 with virtual size > 0, sections sharing raw data, headers inside
the first section's alignment gap) MUST load as Windows does.

### Relocations

**[F5]** When an image cannot load at its preferred base it MUST be relocated
with `IMAGE_REL_BASED_HIGHLOW` (3) and `IMAGE_REL_BASED_ABSOLUTE` (0) entries;
`HIGH`, `LOW` and `HIGHADJ` (1, 2, 4) SHOULD be supported; any other type
MUST fail the load. A DLL whose preferred base is taken MUST be relocated;
a DLL without relocations whose base is taken MUST fail with
`ERROR_BAD_EXE_FORMAT`. Relocation MUST be applied to private copies of the
affected pages only.

### Imports, exports and binding

**[F5]** Import descriptors MUST be resolved by name (using the hint first,
then a binary search of the export name table) and by ordinal, through the
import address table; an empty or absent original-first-thunk table (Borland
style) MUST use the IAT itself. Forwarded exports (`DLL.Name` and
`DLL.#ordinal`) MUST be followed up to 16 levels. Bound imports MUST be
ignored and re-resolved (the timestamps belong to other Windows builds).
Delay-load descriptors MUST be left to the image's own helper, which needs
`LoadLibraryA` and `GetProcAddress` only. The IAT MUST be writable during
resolution and then given its section's protection.

**[F5]** A missing DLL MUST fail process start with the Windows 98 message
"The dynamic link library NAME could not be found in the specified path …"
and a missing export with "The NAME.EXE file is linked to missing export
DLL:FUNCTION." Both MUST also be recorded with the DLL and symbol name (W21).
`LoadLibrary` of a missing DLL MUST fail with `ERROR_MOD_NOT_FOUND` (126);
`GetProcAddress` of a missing export with `ERROR_PROC_NOT_FOUND` (127)
without a dialog.

### TLS

**[F5]** Static TLS (`IMAGE_DIRECTORY_ENTRY_TLS`) MUST be supported for the
EXE and for DLLs loaded at process start and, unlike Windows 98, for DLLs
loaded later with `LoadLibrary` (a superset that removes a known crash of
the era); the TLS index MUST be written to `AddressOfIndex`, each thread's
block initialised from the template plus zero fill, and TLS callbacks MUST
be called with the `DllMain` reasons in load order before the entry point.

### Initialisation, loader lock and unloading

**[F5]** `DllMain` MUST be called with `DLL_PROCESS_ATTACH` in dependency
order (dependencies first; cycles broken in import order), with
`DLL_THREAD_ATTACH`/`DETACH` for every thread unless
`DisableThreadLibraryCalls` was used, and with `DLL_PROCESS_DETACH` in
reverse order on `FreeLibrary` to zero and on `ExitProcess`. A failed
`DLL_PROCESS_ATTACH` during start MUST abort the process with
`ERROR_DLL_INIT_FAILED` (1114); during `LoadLibrary` it MUST unload that DLL
and fail. One recursive process-wide loader lock MUST serialise loads,
unloads and attach/detach notifications; `GetModuleHandle`,
`GetModuleFileName` and `GetProcAddress` MUST work inside `DllMain`.
Reference counts MUST be per module; builtin DLLs MUST NOT be unloaded.

### Search order and the builtin store

**[F5]** A DLL name MUST resolve in this order: (1) already loaded module of
the same base name; (2) builtin store, for every name listed in the store
manifest (KnownDLL semantics, W14); (3) the directory of the EXE; (4) the
current directory; (5) the system directory `C:\WINDOWS\SYSTEM`; (6) the
Windows directory `C:\WINDOWS`; (7) the `PATH` directories. This is the
documented order with safe DLL search mode disabled, as on Windows 98.
A full path MUST load that file unless its base name is a builtin. The
manifest MUST list, per builtin, file name, version resource values
reported to installers, SHA-256 and base address; T1 MUST check that the
bases do not overlap and lie in the builtin region.

**[F5]** Redistributable runtime libraries that installers place in the
system directory (`msvcrt.dll`, `mfc42.dll`, `msvcp60.dll`, `mss32.dll`,
`smackw32.dll`, `binkw32.dll`, codecs of the game) MUST load as ordinary
application DLLs when they are not builtins. Whether `msvcrt.dll` is a builtin
is decided in f5-14 by measurement; either choice MUST be recorded in the
manifest.

## File-system view

**[F5]** Win32 paths MUST resolve through the existing VFS drive mapping:
`C:` is the system volume, further volumes, CD and ISO drives keep the
letters the VFS assigned and logged. Both `\` and `/` separate components;
`.` and `..` resolve lexically as in Windows; trailing dots and spaces of a
component MUST be stripped; `\\?\` and `\\.\` prefixes MUST fail with
`ERROR_FILE_NOT_FOUND` except `\\.\` device names listed in f5-06 (none in
the baseline); UNC paths MUST fail with `ERROR_BAD_NETPATH` (53) until F6.
`MAX_PATH` is 260 including the terminator, as in the VFS.

**[F5]** Names MUST be case-insensitive and case-preserving through the VFS.
Every directory entry MUST be openable by its long name and by its on-disk
8.3 alias; `GetShortPathName`, `GetLongPathName` and the
`cAlternateFileName` field of `FindFirstFile` MUST return the alias that is
on disk. If the F1 VFS does not already resolve aliases, f5-06 MUST add it to
the VFS (an amendment of vfs-storage-contract.md, reviewed before code).
Wildcards MUST follow Windows matching, including `*.*` matching names
without a dot and patterns matching either the long name or the alias.
ANSI file APIs MUST use code page 1252; conversion to the VFS's UTF-8 MUST
be lossless for 1252, and a character outside the volume's alias code page
MUST still be valid in a long name.

**[F5]** The canonical image builder MUST create the following, with these
exact spellings (8.3 names upper-case, so no long-name entry exists for
them, as Windows 98 created them):

| Path | Use and API mapping |
| --- | --- |
| `C:\WINDOWS` | `GetWindowsDirectory`; `WIN.INI`, `SYSTEM.INI`, `WININIT.INI` when created by programs. |
| `C:\WINDOWS\SYSTEM` | `GetSystemDirectory`; installers' shared DLLs; never searched before the builtin store. |
| `C:\WINDOWS\SYSTEM32` | Exists (Windows 98 had it); not the system directory. |
| `C:\WINDOWS\TEMP` | `GetTempPath` default; emptied at every virtual restart of files older than the boot. |
| `C:\WINDOWS\FONTS` | `CSIDL_FONTS`; the personality's fonts are installed here read-only. |
| `C:\WINDOWS\Start Menu\Programs` | `CSIDL_PROGRAMS`; shortcuts appear in the CiukiOS Start menu (f5-09). |
| `C:\WINDOWS\Desktop` | `CSIDL_DESKTOPDIRECTORY`; shortcuts appear on the CiukiOS desktop. |
| `C:\Program Files` and `C:\Program Files\Common Files` | `ProgramFilesDir`, `CommonFilesDir`; alias `PROGRA~1`. |
| `C:\Documents` | `CSIDL_PERSONAL` (owner namespace decision 2026-10-12; no `My Documents`). |
| `C:\System\Win32` | Builtin store, read-only to Win32 processes. |

**[F5]** Writes, deletions and renames under `C:\System` from Win32
processes MUST fail with `ERROR_ACCESS_DENIED` (5). The image MUST carry no
other Windows files: no Microsoft binaries, fonts or data.

**[F5]** `CreateFile` MUST map access and share flags onto VFS share modes
(deny none, read, write, all) and fail conflicts with
`ERROR_SHARING_VIOLATION` (32); `LockFile(Ex)`/`UnlockFile(Ex)` MUST use the
VFS byte-range locks and fail conflicts with `ERROR_LOCK_VIOLATION` (33).
`FILE_FLAG_OVERLAPPED` on files MUST complete synchronously, set the event
and queue the completion APC; `FILE_FLAG_DELETE_ON_CLOSE` MUST be supported;
`MoveFileEx` with `MOVEFILE_DELAY_UNTIL_REBOOT` MUST fail with
`ERROR_CALL_NOT_IMPLEMENTED` (120) as on Windows 98, whose installers write
`WININIT.INI` instead (see "Installers"). `GetDiskFreeSpace` MUST cap total
and free bytes at 2 GiB − 1 so that installers' 32-bit arithmetic does not
overflow; `GetDiskFreeSpaceEx` MUST report true values. `GetDriveType`,
`GetVolumeInformation` (label, serial, `FAT32`/`CDFS` names, maximum
component length 255) and `GetLogicalDrives` MUST reflect the VFS.

**[F5]** File times: the VFS stores FAT timestamps; until question Q9 is
answered the personality's time zone MUST be UTC with
`TIME_ZONE_ID_UNKNOWN`, `GetLocalTime` equal to `GetSystemTime`, so FAT
timestamps written by native, DOS and Win32 programs agree.

## Registry and INI files

**[F5]** `w32srv` MUST hold two hives — `SYSTEM.CRG` (HKLM, including
`HKLM\Software\Classes`) and `USER.CRG` (HKU\.DEFAULT, which is HKCU) — in
`C:\WINDOWS\`, each as two slots with a version, a sequence number and a
CRC-32 over a UTF-8 text body (one key path per section, typed values),
following the two-slot rule of `C:\SYSTEM\AUDIO.CFG`. A write MUST be
committed by writing the inactive slot, `fsync`, then switching the
sequence; a torn or invalid slot MUST fall back to the other one with a
`[win32] registry` record. Commits MUST happen at most every 2 s while dirty,
on `RegFlushKey`, on the exit of the writing process and before a virtual
restart or shutdown. The hive files are CiukiOS files; no Windows hive format
is read or written.

**[F5]** Root keys MUST be `HKEY_CLASSES_ROOT` (alias of
HKLM\Software\Classes), `HKEY_CURRENT_USER` (alias of HKU\.DEFAULT; one user),
`HKEY_LOCAL_MACHINE`, `HKEY_USERS`, `HKEY_CURRENT_CONFIG` (synthesised
display settings) and `HKEY_DYN_DATA` (empty, present as on Windows 98).
Value types `REG_SZ`, `REG_EXPAND_SZ`, `REG_BINARY`, `REG_DWORD`,
`REG_MULTI_SZ` and `REG_NONE` MUST round-trip exactly. Limits: key names
255 characters, value names 255, value data 64 KiB (above Windows 98's
documented 16,300-byte limit, a superset), 4 MiB of text per hive.
`RegOpenKey*`, `RegCreateKey*`, `RegQueryValue*`, `RegSetValue*`,
`RegEnumKey*`, `RegEnumValue`, `RegDeleteKey` (recursive, as on Windows 98),
`RegDeleteValue`, `RegQueryInfoKey`, `RegFlushKey`, `RegCloseKey` and
`RegNotifyChangeKeyValue` (MAY) MUST be implemented with the documented
error codes.

**[F5]** The image builder MUST populate at least these keys; values derive
from the image and its configuration, never from a Windows installation:

| Key | Values |
| --- | --- |
| `HKLM\Software\Microsoft\Windows\CurrentVersion` | `ProgramFilesDir`, `CommonFilesDir`, `Version` = `Windows 98`, `VersionNumber` = `4.10.2222`, `SubVersionNumber` = ` A `, `ProductName` = `Microsoft Windows 98` (installers compare it; question Q4 covers the wording), `RegisteredOwner` = `Ciuki`, `RegisteredOrganization` = `CiukiOS`, `MediaPath` = `C:\WINDOWS\MEDIA`, `SystemRoot` = `C:\WINDOWS`. |
| `...\CurrentVersion\App Paths`, `...\Uninstall`, `...\Run`, `...\RunOnce`, `...\SharedDLLs`, `...\Fonts`, `...\Explorer\Shell Folders`, `...\Explorer\User Shell Folders` (HKLM and HKCU) | Present; folder values as in "File-system view". |
| `HKLM\Software\Microsoft\DirectX` | `Version`, `InstalledVersion` as decided under Q4. |
| `HKLM\Software\Classes` | `CLSID` registrations of the builtin COM classes (DirectDraw, DirectSound, DirectInput, ShellLink, …), `.lnk`, `.exe`, `.txt` associations. |
| `HKLM\System\CurrentControlSet\Control\MediaResources`, `...\Services\Class\Display`, `HKCC\Display\Settings` | Synthesised audio and display device descriptions (`Resolution`, `BitsPerPixel`) from the active mode. |
| `HKCU\Control Panel\Desktop`, `HKCU\Control Panel\International`, `HKCU\Control Panel\Mouse` | Defaults of an English (United States) Windows 98 installation. |
| `HKLM\Software\CiukiOS\Win32` | Personality configuration and the user override layer of the shim database. |

**[F5]** The profile APIs (`GetPrivateProfileString`, `…Int`, `…Section`,
`…SectionNames`, `…Struct`, `WritePrivateProfile*` and the `WIN.INI`
variants) MUST read and write INI files directly, with Windows 98 parsing
(case-insensitive sections and keys, first match wins, comments `;`, values
trimmed, quotes stripped, 64 KiB maximum file size for `WritePrivateProfile*`
growth). `WIN.INI` and `SYSTEM.INI` MUST exist with minimal `[windows]`,
`[fonts]`, `[boot]` and `[drivers]` sections; the registry mapping of
`IniFileMapping` (NT only) MUST NOT be applied.

## Version identity and system information

**[F5]** `GetVersion`, `GetVersionEx(A/W)`, `GetSystemInfo`
(`dwPageSize` 4096, `dwAllocationGranularity` 65536, application address
range as above, one processor, `PROCESSOR_INTEL_PENTIUM`, level and
revision from CPUID), `IsProcessorFeaturePresent` (MMX, SSE, `RDTSC`,
`CMPXCHG8B`, from CPUID), `GetComputerName` (`CIUKIOS`), `GetUserName`
(`Ciuki`), `GetSystemMetrics` (from the active mode and theme),
`GetSystemDefaultLCID`/`GetUserDefaultLCID` (`0x0409`, question Q15),
`GetACP` (1252) and `GetOEMCP` (437) MUST report a consistent Windows 98 SE
system. `CreateToolhelp32Snapshot` with processes, threads and modules of
the caller MUST be implemented from the process list (call 70) and the
loader's module list; other processes' module lists MAY be empty.
CPUID itself is not interceptable on these processors and reports the real
CPU.

## DLL set by phases

**[F5]** The builtin set MUST grow in the phases below; each phase is gated
by its directive's acceptance and by the API census of the acceptance set
(f5-02), which lists every imported DLL and function of every acceptance
executable and installer. A census entry that no phase covers MUST be
resolved (implemented, or stubbed with a W21 record) before the F5 gate.

| Phase | DLLs | Backed by | Directive |
| --- | --- | --- | --- |
| P1 core | `ntdll` (loader, exceptions, heaps, critical sections, `Rtl*` the other DLLs need), `kernel32` | `CIUKI_SYS_W32`, F2 file/clock calls, VFS drive paths | f5-05, f5-06 |
| P2 system | `advapi32` (registry, trivial security, `GetUserName`), `version`, `lz32` | `w32srv` | f5-07, f5-14 |
| P3 windows and drawing | `user32`, `gdi32` (windows, messages, dialogs, standard controls, menus, cursors, DIB engine, fonts, palettes), `imm32` stub | Desktop protocol v3, surfaces, presenter | f5-09, f5-10 |
| P4 installers | `comctl32`, `comdlg32`, `shell32` (folders, shortcuts, `ShellExecute`, `SHFileOperation`, `SHBrowseForFolder`, PROGMAN DDE), `shlwapi`, `ole32`/`oleaut32` (in-process COM, `CoCreateInstance`, `IMalloc`, `VARIANT`, `BSTR`), `riched20`/`riched32`, `cabinet`, `advpack`, `setupapi` (INF subset), `msvcrt` (by measurement), `dsetup`/`dsetup32` builtins | P1–P3 APIs only | f5-14, f5-15 |
| P5 multimedia | `winmm` (waveOut, timers, MIDI out, mixer, MCI wave/sequencer/AVI, `joy*` without devices), `dsound`, `msacm32` with IMA and MS ADPCM, `msvfw32`/`avifil32` with Cinepak, MS RLE and MS Video 1 | `/dev/audio`, OPL or wavetable synth (Q6) | f5-12 |
| P6 DirectDraw and input | `ddraw` (DirectDraw 1–7), `dinput`, `dinput8` | Presenter lease, protocol v3 input | f5-11, f5-13 |
| P7 network (F6) | `wsock32`, `ws2_32`, `dplayx`, `dpwsockx` | F6 sockets; IPX under Q13 | f5-18 |
| P8 3D (F7) | `d3dim`/`d3dim700` behind `ddraw`, `d3d8`, `opengl32`, `glu32` | F7 Mesa/DRM contract | F7 |

**[F5]** Before P8, `ddraw`'s `QueryInterface` for any `IDirect3D*`
interface MUST return `E_NOINTERFACE`, `Direct3DCreate8` MUST return NULL,
and `opengl32` MUST exist and fail `ChoosePixelFormat` for accelerated
formats, so that games with a software renderer fall back to it and games
without one report "no 3D accelerator" instead of failing to load.

## Display model

### Windowed programs

**[F5]** Every visible top-level Win32 window MUST be one window of desktop
protocol v3 (f5-09), with its own client surface (XRGB8888 or a format of
the presenter amendment), title in UTF-8, frame style (captioned, popup
without frame, tool window), owner relationship, topmost flag and
client-requested position. Child windows, menus, tooltips and combo-box
drop-downs MUST be drawn by `user32`/`gdi32` into the surface of their
top-level window; pop-up menus and drop-downs that extend beyond it MAY be
separate protocol windows. Painting MUST follow Windows semantics
(`WM_PAINT` from an update region, `WM_ERASEBKGND`, `BeginPaint`/`EndPaint`,
`InvalidateRect`, `UpdateWindow`), and only damaged rectangles MUST be sent
to the desktop.

**[F5]** Protocol v3 MUST carry: cursor shape (a 32×32 monochrome or colour
cursor bitmap with hot spot, cached by id), cursor visibility
(`ShowCursor` counts), mouse capture (`SetCapture`), cursor clipping
(`ClipCursor`), cursor warping (`SetCursorPos`, applied only for the focused
process), relative motion delivery while captured or clipped, activation and
focus messages, minimise/restore, and a `WM_CLOSE` request from the frame's
close button. A window whose process is suspended or not answering MUST be
marked unresponsive by the existing PING rule, not by the personality.

### Full-screen exclusive mode

**[F5]** `IDirectDraw*::SetCooperativeLevel` with
`DDSCL_EXCLUSIVE|DDSCL_FULLSCREEN` followed by `SetDisplayMode(w, h, bpp)`
MUST request a full-screen lease from the desktop over protocol v3. The
desktop MUST choose the hardware mode: the listed mode with the same width,
height and depth if present; otherwise the smallest listed mode at 16 or 32
bpp whose size is at least the requested size, preferring an exact integer
multiple (2× for 320×200, 320×240, 400×300 and 512×384). The desktop MUST
switch with call 74 (its authority is unchanged), stop compositing, and ask
the kernel to delegate a present lease to the game process.

**[F5]** The present lease (presenter amendment, f5-08) MUST let exactly
one process present its surfaces to the whole screen, with the kernel
converting the source format and scaling, until the lease is revoked. The
desktop MUST revoke it on Alt+Tab, Ctrl+Esc, the Windows key,
Ctrl+Alt+Del (Task Manager), a modal system dialog, the game's
`RestoreDisplayMode` or `SetCooperativeLevel(DDSCL_NORMAL)`, and the game
process's exit or death. Revocation MUST be synchronous for the kernel: no
present from the former holder reaches the screen after it. The desktop then
restores its own mode and repaints. The game MUST receive
`WM_ACTIVATEAPP(FALSE)`, its DirectDraw surfaces MUST report
`DDERR_SURFACELOST` until `Restore()` succeeds after reactivation, and its
lease MUST be re-granted on reactivation with the same mode.

**[F5]** A game that crashes or is terminated while holding a lease MUST
leave the desktop at its previous mode with input restored within 2 s,
and the f1-23 10 s watchdog MUST remain the last resort. The game process
MUST never obtain the display grant, the mode-set authority or a mapping of
the framebuffer (question Q5 asks the owner whether a later amendment may
map it under the lease).

### Formats, palettes, scaling and pacing

**[F5]** The presenter amendment MUST add source formats RGB565, XRGB1555
and 8-bit indexed with a 256-entry palette object, beside XRGB8888, and
integer scaling (1×, 2×, 3×) with centring and black borders, converting in
one pass into the qualified framebuffer format. Palette changes
(`IDirectDrawPalette::SetEntries`, GDI palette realisation in 8 bpp modes)
MUST take effect on the next present of the whole surface. A gamma ramp
(`IDirectDrawGammaControl`) SHOULD be applied as a per-channel lookup table
in the same pass.

**[F5]** DirectDraw display modes MUST be enumerated as the union of the
hardware modes and the emulated modes 320×200, 320×240, 400×300, 512×384,
640×400, 640×480, 800×600 and 1024×768 at 8, 16 and 32 bpp, limited to sizes
the active adapter can show at ≥ 1× scale. 16 bpp surfaces MUST be RGB565
unless a shim selects 555. Flip and blit timing: the presenter has no vertical
blank (execution-abi.md), so `WaitForVerticalBlank`, `Flip` with
`DDFLIP_WAIT` and `GetScanLine` MUST be paced by a 60 Hz software clock
derived from the monotonic clock, with `GetMonitorFrequency` reporting 60.
Real retrace waiting through the VGA input-status register is a later option
that needs a device-contract amendment.

### DirectDraw

**[F5]** DirectDraw MUST implement `IDirectDraw`, `2`, `4`, `7`;
`IDirectDrawSurface` through `7`; `IDirectDrawPalette`, `IDirectDrawClipper`,
`IDirectDrawColorControl` (accepted, no effect) and `IDirectDrawGammaControl`,
with these semantics: primary surface and flip chains of 2–3 buffers;
off-screen plain surfaces in "video" or "system" memory (both system memory
owned by the personality, accounted against a reported `dwVidMemTotal` equal
to the adapter's VRAM from call 74); `Lock`/`Unlock` returning a pointer and
pitch; `Blt` and `BltFast` with source and destination colour keys, colour
fill, stretching with nearest-neighbour sampling, mirroring and
`DDBLT_WAIT`; `GetDC`/`ReleaseDC` with GDI drawing on surfaces; clippers
bound to windows for windowed games; `EnumDisplayModes`, `EnumSurfaces`,
`GetCaps` and `GetDisplayMode`. Capabilities MUST report the blit, colour-key,
stretch and flip capabilities that the software implementation provides as
hardware capabilities, so titles that refuse "emulation only" drivers run.
Overlay surfaces, Z-buffers, textures and `DDSCAPS_3DDEVICE` belong to F7 and
MUST fail with `DDERR_NOOVERLAYHW` or `DDERR_INVALIDCAPS`. The Savage 2D
engine (f1-26) MAY accelerate presenter conversion and scaling once it is
qualified; it MUST NOT be exposed to games.

## Audio

**[F5]** Each Win32 process MUST open at most one `/dev/audio` stream on the
first audio use, at S16LE stereo 44,100 Hz unless every source uses 22,050
or 48,000 Hz, and mix into it in user mode: DirectSound secondary buffers,
waveOut queues and the MIDI synthesizer. The mixer MUST run on one personality
thread at `interactive` class, keep at most two device quanta (256 frames each)
queued beyond what the device consumed, and derive play cursors from the
consumed-frame counter of `AUDIO_GET_STATUS`. A process that cannot open a
stream (eight streams in use, `ENOMEM`) MUST receive
`DSERR_ALLOCATED`/`MMSYSERR_ALLOCATED`, not a fabricated success.

**[F5]** DirectSound MUST implement `IDirectSound`, `IDirectSound8`,
`IDirectSoundBuffer`, `IDirectSoundBuffer8`, `IDirectSound3DBuffer` and
`IDirectSound3DListener` (distance attenuation and pan only),
`IDirectSoundNotify` and `DirectSoundEnumerate` with one device "CiukiOS
audio": primary buffer format and `SetCooperativeLevel` (all levels
accepted), static and streaming secondary buffers, 8/16-bit mono/stereo
PCM at 4,000–48,000 Hz, `Lock`/`Unlock` with wrap-around pointers, `Play`
with looping, `Stop`, `GetCurrentPosition` within one quantum (5.3 ms)
of the consumed position, `SetVolume` (hundredths of a decibel), `SetPan`,
`SetFrequency`, `DuplicateSoundBuffer` and notification events. Capture
interfaces MUST fail with `DSERR_NODRIVER`.

**[F5]** winmm MUST implement waveOut (open, prepare, write, reset, pause,
restart, position, volume), `timeGetTime`, `timeGetDevCaps`,
`timeBeginPeriod`/`timeEndPeriod` (accepted, 1 ms always), `timeSetEvent`
and `timeKillEvent` (periodic and one-shot callbacks on a personality thread
at `interactive` class, at most 16 per process), midiOut with one device,
`mixerGetNumDevs`/`mixerOpen`/line and control queries for master and wave
volume (mapped to the stream gain, never to the master volume, which stays
the desktop's), `auxGetNumDevs` (0), `joyGetNumDevs` (16) with every
`joyGetPos*` returning `JOYERR_UNPLUGGED`, and MCI `waveaudio`, `sequencer`
(MIDI files) and `avivideo` for the codecs of phase P5. MCI `cdaudio` (Red
Book playback from the ATAPI drive) is question Q8.

**[F5]** MIDI output MUST be synthesised in user mode by the synthesizer
chosen under question Q6 and mixed like any source; at most one
synthesizer instance per process.

## Input

**[F5]** Keyboard messages MUST be built from protocol v3 KEY and TEXT
events: `WM_KEYDOWN`/`WM_KEYUP`/`WM_SYSKEYDOWN`/`WM_SYSKEYUP` with lParam
scan code = set-1 code & 0xFF, bit 24 set for E0 keys, repeat count and
transition bits; virtual-key codes from the active desktop layout (US or
Italian, `keymap.c`) with `MapVirtualKey`, `VkKeyScan`, `ToAscii`,
`GetKeyboardLayout` (`0x04090409` or `0x04100410`) consistent with it;
`WM_CHAR` from TEXT events in code page 1252. `GetKeyState`,
`GetAsyncKeyState` and `GetKeyboardState` MUST reflect the messages the
thread processed and the global state respectively, cleared on focus loss
and on RESYNC.

**[F5]** DirectInput 3–8 MUST implement the system keyboard and mouse
devices (`GUID_SysKeyboard`, `GUID_SysMouse`), data formats `c_dfDIKeyboard`
and `c_dfDIMouse`/`c_dfDIMouse2`, immediate (`GetDeviceState`) and buffered
(`GetDeviceData`, buffer size property) data, `SetEventNotification`,
cooperative levels foreground/background and exclusive/non-exclusive, and
`Acquire`/`Unacquire` with `DIERR_INPUTLOST`/`DIERR_NOTACQUIRED` on focus
loss. DIK codes MUST be the set-1 code for normal keys and `0x80 | code` for
E0 keys (Pause MUST map to `DIK_PAUSE` 0xC5). Mouse axes MUST be relative
counts from the kernel's motion deltas, never derived from absolute
positions, and exclusive mouse access MUST hide the cursor and clip it to the
window. `EnumDevices` for game controllers MUST return none.

## Networking boundary (F6)

**[F5]** Until F6 qualifies sockets, `wsock32` and `ws2_32` MUST exist:
`WSAStartup` MUST succeed for versions 1.1 and 2.2, socket creation MUST fail
with `WSAENETDOWN` (10050), host lookups with `WSAHOST_NOT_FOUND`, and
DirectPlay MUST enumerate no service providers. A game's single-player mode
MUST NOT be prevented by these failures.

**[F6]** f5-18 MUST implement Winsock 1.1/2 on the F6 socket calls: TCP and
UDP over IPv4, blocking and non-blocking sockets, `select`,
`WSAAsyncSelect` (window-message notifications, widely used by games),
`WSAEventSelect`, `gethostbyname`/`getaddrinfo` through `resolve4`,
`gethostname`, broadcast UDP, and DirectPlay's TCP/IP service provider. Win32
IPX (`AF_IPX`, used by the LAN modes of StarCraft, Diablo and Age of Empires)
needs an IPX endpoint on the F6 Ethernet bridge; it is question Q13.

## Installers

**[F5]** The installer engines found in the acceptance set on 2026-10-13
(see "Acceptance set") MUST complete an unattended-equivalent interactive
installation: Wise (Half-Life: Uplink), InstallShield (Heroes of Might and
Magic III demo), IExpress with `advpack` and cabinets (Age of Empires
trial), WinZip self-extractors that run a vendor `Setup.exe` (Unreal
Tournament demo, RollerCoaster Tycoon demo), vendor installers (Jazz
Jackrabbit 2 shareware, Total Annihilation demo) and installation from CD
images (Diablo and StarCraft shareware). NSIS 2.x MUST be covered by an
installer fixture generated from a pinned NSIS release (zlib licence) with a
pinned script (f5-17). Each requires:

- extraction to `GetTempPath`, `CreateProcess` of the extracted setup and a
  wait on its process handle; self-deleting uninstallers that copy
  themselves to the temporary directory, start the copy and exit;
- `version.dll` (`GetFileVersionInfo`, `VerQueryValue`, `VerInstallFile`)
  with builtins reporting the versions in the store manifest, so version
  comparisons never decide to overwrite a builtin;
- `lz32` and `cabinet` decompression; `advpack` `RunSetupCommand`,
  `LaunchINFSection(Ex)` and the `setupapi` INF subset they need
  (`CopyFiles`, `AddReg`, `DelReg`, `UpdateInis`, `RunPostSetupCommands`);
- shell folders, `SHGetSpecialFolderPath`, `SHBrowseForFolder`,
  `SHFileOperation`, `ShellExecute(Ex)`, `IShellLink`/`IPersistFile` writing
  `.lnk` files, and the PROGMAN DDE conversation (`CreateGroup`, `AddItem`,
  `DeleteGroup`, `ShowGroup`) that `w32srv` answers by creating or removing
  `.lnk` files under `C:\WINDOWS\Start Menu\Programs`;
- the Uninstall key: the desktop's Control Panel MUST list
  `DisplayName` entries and run `UninstallString` (f5-09);
- "DirectX setup": builtin `dsetup.dll`/`dsetup32.dll` MUST answer
  `DirectXSetup*` and `DirectXSetupGetVersion` with "already installed" at the
  version of question Q4, and a `DXSETUP.EXE` run by an installer MUST end
  successfully without copying files into `C:\System\Win32`.

**[F5]** "Restart Windows": `ExitWindowsEx` with `EWX_REBOOT` or
`EWX_LOGOFF`, and the restart prompts installers show, MUST trigger a
virtual restart: the desktop asks the user ("Setup wants to restart
Windows. CiukiOS will close all Windows programs and finish the
installation."), closes every Win32 process (WM_QUERYENDSESSION, then
termination after 10 s), `w32srv` commits the hives, processes the
`[rename]` section of `C:\WINDOWS\WININIT.INI` (copy or delete operations,
then renames the file `WININIT.BAK`), runs `RunOnce` and `RunServicesOnce`
entries once in order and deletes them, then resumes. A real machine reboot
MUST NOT be required by any acceptance title. `EWX_SHUTDOWN` and
`EWX_POWEROFF` MUST end only the Win32 programs and leave power decisions to
the desktop.

**[F5]** An `.msi` file, `msiexec.exe` or `msi.dll` use MUST show "This
program uses Windows Installer, which CiukiOS does not support" and record
`[win32] refuse msi`. A 16-bit setup stub MUST follow W24.

## Compatibility shims

**[F5]** The shim database MUST be a read-only text file
`C:\System\Win32\SHIMS.INI` in the image, plus an override layer under
`HKLM\Software\CiukiOS\Win32\AppCompat` written only by the Control Panel.
An entry MUST match by executable file name and at least one of: PE
`TimeDateStamp`, `SizeOfImage`, file size or SHA-256. Each shim MUST have a
stable name and an integer version; an entry names `shim@version` pairs and
their parameters. Changing a shim's behaviour MUST bump its version, and
every acceptance title using that shim MUST be requalified before the image
that carries the change counts as evidence for it. Applied shims MUST appear
in the process's `[win32] start` record.

| Shim (initial set) | Effect |
| --- | --- |
| `version` | Report Windows 95, ME, NT 4.0, 2000 or XP instead of 98 SE. |
| `ddraw-555` | Report and use XRGB1555 for 16 bpp surfaces. |
| `ddraw-scale` | Force an integer scale or a hardware mode. |
| `ddraw-windowed` | Run a full-screen title in a desktop window of its mode size. |
| `heap-zero`, `heap-pad` | Zero new heap blocks; pad allocations by N bytes. |
| `memstatus-cap` | Cap reported memory to N MiB (titles that overflow above 64 or 256 MiB). |
| `io-pit`, `io-gameport` | Emulate reads of PIT channel 0 and port `201h` (no joystick) on `EXCEPTION_PRIV_INSTRUCTION`, without hardware access. |
| `dll-native` | Prefer the application's copy of a named non-core DLL. |
| `fpu-cw` | Set the initial x87 control word. |
| `w-stubs` | Make named Unicode functions fail with `ERROR_CALL_NOT_IMPLEMENTED`, as Windows 9x did. |

## Error reporting and diagnostics

**[F5]** An exception that no handler and no filter handles MUST show a
desktop dialog modelled on Windows 98: "NAME has performed an illegal
operation and will be shut down." with a Details view (exception code, module
name and offset, EIP, registers, eight stack words) and a Close button, then
terminate the process with the exception code. The same facts MUST be
written as one `[win32] crash` record on serial and in the boot log, and as
a text file of at most 16 KiB in `C:\WINDOWS\CRASH\` (eight files, oldest
replaced). No memory dump is written.

**[F5]** Unimplemented functions that exist only as stubs MUST, on first
call per process, record `[win32] stub DLL!FUNCTION` and return the documented
failure value with `ERROR_CALL_NOT_IMPLEMENTED` (120); a stub MUST never
return success without performing the operation. Personality warnings MUST
be rate-limited to 64 records per process. Records MUST NOT contain file
contents, registry data or user text beyond names of DLLs, functions, files
of the title and window classes.

**[F5]** Every Win32 process start MUST emit
`[win32] start pid=<pid> exe=<path> pe=<timestamp>:<sizeofimage> shims=<list>`
and every exit `[win32] exit pid=<pid> code=<hex> reason=<normal|crash|killed|restart>`
with peak commit, peak handles and threads. Task Manager MUST show the
executable name of each Win32 process.

## Memory budget at 128 MiB

**[F5]** These are targets for a 128 MiB machine with the desktop running and
no DOS VM; f5-04 and f5-17 MUST measure them on QEMU at 128 MiB and on one
laptop and record the values. A target that measurement exceeds MUST return
to the lead; it MUST NOT be hidden by raising the reference RAM.

| Item | Target |
| --- | --- |
| Kernel Win32 additions (code, object ledger, per-thread FS and TEB pages excluded) | ≤ 256 KiB plus 512 bytes per Win32 object |
| `w32srv` resident, including loaded hives | ≤ 3 MiB |
| Core builtin images (P1, P3, P5, P6), shared once system-wide | ≤ 5 MiB |
| Installer builtin images (P2, P4), shared once system-wide, loaded only by installers | ≤ 14 MiB |
| Fonts and rasteriser, shared | ≤ 2 MiB |
| Private personality data per running game (DLL data, heaps of the personality, TEBs, mixer, message queues) | ≤ 4 MiB |
| Emulated display surfaces | Title-dependent, reported; 640×480×8 double-buffered 0.6 MiB, 800×600×16 triple-buffered 2.8 MiB |
| Memory left to one game, after the above and the desktop | ≥ 64 MiB of commit |

**[F5]** Running a Win32 game and a DOS VM at the same time is not required
at 128 MiB; admission of a DOS VM MUST keep its existing 32 MiB headroom
rule, with Win32 commitments counted as native commitments.

## Acceptance set

**[F5]** The gate MUST use these ten titles. All are demo or shareware
releases whose publishers distributed them free of charge; the download
source is the Internet Archive item named, with the file size and SHA-1 the
archive publishes. f5-17 MUST download each file once into
`build/downloads/win32-workloads/`, verify SHA-1, record SHA-256, and record
the licence or distribution text shipped inside. No title enters the
canonical image or any release artefact; the runner attaches them as a
fixture disk or CD image (f1-16 numbering).

| # | Title (year, publisher) | Archive item / file (bytes, SHA-1) | Installer observed | Expected to exercise (the f5-02 census confirms) |
| --- | --- | --- | --- | --- |
| 1 | Diablo shareware (1997, Blizzard) | `DiabloSharewareWindowsBlizzardEntertainment1997` / `DIABLOSW.iso` (137576448, SHA-1 `bf1a62b24ce01ce39993955bff4b3c4d4ab9d647`) | CD image, vendor setup | ISO 9660 install, DirectDraw 640×480×8, palette, DirectSound, SMK video |
| 2 | StarCraft shareware (1998, Blizzard) | `StarCraftUSAShareware` / `StarCraft Shareware (USA).zip` (248478379, `a29ccfb4b34d55034bbae6bde23199fc4766f288`) | CD contents, vendor setup | DirectDraw 8 bpp, DirectSound, Smacker video, `storm.dll` |
| 3 | Age of Empires trial (1997, Microsoft) | `aoe-trial-1997` / `MSAoE.exe` (24786864, `ccd19bd57e7070ffe7c0b20ed9265d680fbe36c7`) | IExpress (`ADVPACK.DLL`, CAB) with DirectX setup | advpack/cabinet, DirectDraw 8 bpp, MIDI music, DirectX setup |
| 4 | Half-Life: Uplink (1999, Valve) | `Half-lifeUplink` / `hluplink.exe` (50872079, `605e110670ca3d55b2093b7750d1d0138cbf1f4c`) | Wise | Software renderer on DirectDraw, DirectSound, `WSAStartup` |
| 5 | Unreal Tournament demo 348 (1999, Epic) | `unreal-tournament-demo-version-348` / `UTDEMO348.EXE` (55647232, `faf2c18852a1a53c59db490e044e0d3e100e8fed`) | WinZip self-extractor + vendor setup | Software renderer, DirectSound, large commit |
| 6 | Worms Armageddon demo (1999, Team17) | `WormsArmageddonDemo` / `WormsArmageddonDemo.zip` (13903680, `5a53c889037b91566c211b16d527c69afd037be8`) | To be identified at import | DirectDraw 8 bpp, DirectSound |
| 7 | Heroes of Might and Magic III demo (1999, New World Computing) | `HeroesOfMightAndMagicIiiTheRestorationOfErathia` / `h3demo.exe` (98265058, `8d44f794a3fe80bea524f90b38fed3325ca3d1cb`) | InstallShield | DirectDraw 800×600×16, streamed music, common controls in setup |
| 8 | RollerCoaster Tycoon demo (1999, Chris Sawyer Productions) | `RollercoasterTycoonDemo` / `RCTYCOON.EXE` (19596288, `f985bb782e974572805ee2990cfa2d1fd4e93eee`) | WinZip self-extractor + `Setup.exe` | DirectDraw 8 bpp, many sound buffers, mouse-driven UI |
| 9 | Jazz Jackrabbit 2 shareware (1998, Epic MegaGames) | `jazz2shareware` / `jazz2swf.exe` (11019236, `d2ab0a3107360563c5c3bb897a4adfb9d319c04f`) | Vendor installer calling `DirectXSetupA` | DirectDraw 8/16 bpp, DirectSound, `dsetup` builtin |
| 10 | Total Annihilation demo (1997, Cavedog) | `TotalAnnihilation_201405` / `Total Annihilation.exe` (21540864, `ce8a2c21488fbe807cd855b8effcc127053c3e85`) | To be identified at import | DirectDraw 8 bpp at 640×480 and above, DirectSound |

Sizes and SHA-1 digests were read from each item's metadata
(`https://archive.org/metadata/<item>`) on 2026-10-13; the download URL is
`https://archive.org/download/<item>/<file>`. Installer engines were
identified from strings in the first MiB of each file (Wise: "Wise
Installation Wizard", `GLBSInst`; InstallShield: "InstallShield"; IExpress:
`ADVPACK.DLL`, `IDR_CABFILE`; WinZip: "WinZip Self-Extractor"). The Worms
archive and the Total Annihilation demo, whose first MiB names no engine,
are identified at import.

**[F5]** Reserve and later titles, not part of the F5 gate: Quake III Arena
demo (`QuakeIiiArenaDemo` / `Q3ADemo.exe`, OpenGL only: F7 gate); Need for
Speed II SE demo (`NFS2SEA` / `NFS2SEA.EXE`, a Win16 self-extractor observed
on 2026-10-13: blocked by W24 and question Q3); Grand Theft Auto 2 freeware
release (`pc_gta2`); Tomb Raider: The Last Revelation demo
(`TombRaiderTheLastRevelationDemo`, Direct3D: F7). A reserve title replaces a
gate title only by owner decision.

## Gates and test architecture

### Tiers

**[F5/T0 host]** Host tests MUST cover: the PE parser and loader logic
against the PE fixture set, including every negative case of "Accepted
images" and the boundaries (maximum `SizeOfImage`, 16 data directories,
forwarder depth 16); the kernel object model (waits any/all, abandonment,
APC ordering, queue wake bits) in the host harness that already tests kernel
code; registry slot commit, torn-slot recovery and text round trip; INI
parsing against recorded Windows 98 results; path mapping, wildcards and
alias selection; DirectDraw blits, colour keys and palette conversion against
pinned reference digests; the DirectSound mixer sample-exactly against
reference PCM; DIK, VK and lParam mapping tables for US and Italian layouts;
shim matching. The PE fixtures are freestanding programs built by the pinned
clang for `i686-pc-windows-msvc` with `lld-link /nodefaultlib` and import
libraries generated from `.def` files (f5-02); they also run as T3 payloads.

**[F5/T1 image]** Static checks MUST verify the builtin store manifest
(hashes, bases inside `0x70000000–0x7FFDFFFF`, no overlaps, version
resources), the `C:\WINDOWS` skeleton and default hives, licence notices of
every reused component, and the absence of any acceptance payload or
Microsoft binary in the image.

**[F5/T3 QEMU]** Suite `f5-win32` on the `qemu-t23` profile (pentium3, 128
MiB, AC97, `-vga std`), selected through fw_cfg, one case per run, each
bounded to 10 minutes (owner directive 2026-10-12: no long campaigns):
`pe-fixtures` (every fixture self-reports PASS lines through the debug
output), `objects`, `exceptions` (nested SEH, unwind, guard pages, stack
overflow, x87 and SSE faults, context continue), `memory`, `registry`
(including power-loss simulation by QEMU kill between commits), `windowed`
and `fullscreen` fixtures (mode switch, Alt+Tab ten times, crash while
holding a lease), `audio` (WAV capture of a known DirectSound/waveOut/MIDI
sequence), `installers` (the NSIS fixture and one engine per run), and one
case per acceptance title. The 10 titles MUST NOT run in one session; the
lead schedules them per directive and at the gate.

**[F5/T4 hardware]** The same canonical image on the T23 and the E500 with
serial capture (38400 8N1); acceptance installers on a FAT32 partition or
a CD-R written from the pinned files; the operator follows the per-title
script of f5-17 and the serial records decide the result.

### Per-title pass criteria

**[F5]** A title passes on a machine when, on one image SHA-256:

1. **Install**: the original installer completes with its default choices,
   creates its Start-menu entry and Uninstall key when it registers one, and
   writes no file under `C:\System`;
2. **Start**: launched from the Start menu, the title reaches its main menu
   (screen digest region or `[win32]` window-class record) within 120 s on
   hardware;
3. **Play**: a scripted input sequence (f5-17 input player on QEMU, operator
   script on hardware) starts a game or a built-in demo and plays for at least
   3 minutes with sound: non-silent PCM consumed by the stream, no
   `[win32] crash`, no unhandled stub on the play path;
4. **Focus**: for three titles chosen in f5-17 (at least two full-screen
   8 bpp titles), ten Alt+Tab round trips without corruption or crash;
5. **Exit**: the title's own Quit returns to the desktop at the desktop's
   mode with exit code 0 (or the title's documented code), and the process,
   handle, section, stream and channel ledgers return to their pre-start
   values within 2 s;
6. **Uninstall**, when the title registers an uninstaller: it runs from the
   Control Panel and removes the Uninstall key and Start-menu entries;
7. **Record**: frame rate (median and 5th percentile over the play segment),
   audio underruns, peak commit and shims applied are recorded; the playable
   thresholds of question Q7 apply on hardware once the owner sets them.

**[F5]** The F5 gate closes when every title passes on QEMU and on both
laptops on one image, with F0–F4 QEMU gates passing on that image; where the
E500 audio backend is not qualified, the sound criteria are `not_run` there
and the gate stays open for that machine.

## Implementation options

Three ways exist to provide the user-mode DLLs. All three need the same
kernel personality (W5–W10) and desktop and presenter work; they differ in
who writes the roughly fifty DLLs of phases P1–P6.

**Option A — write every DLL from scratch.** CiukiOS-owned, MIT-licensed
code sized by the API census of the acceptance set. Advantages: smallest
footprint, direct use of kernel objects, the desktop protocol and the
presenter; every line is reviewed and host-testable; no upstream to track.
Costs: the installer-facing long tail (common controls, common dialogs,
rich edit, shell namespace, COM and OLE automation, INF processing,
cabinets, codecs) is large and full of undocumented behaviour that period
installers depend on; each acceptance installer becomes a debugging project.

**Option B — port an existing complete Win32 user-mode implementation with
its own host layer.** The mature open-source implementation is Wine
(LGPL-2.1-or-later; the release examined on 2026-10-13 was `wine-10.0`). In
current releases its DLLs are PE modules whose system services are provided
by a Unix-side `ntdll`, `win32u` and display/audio drivers loaded with
`dlopen`, plus the `wineserver` process; its README requires an operating
system with kernel-level threads. Porting it as a whole means making CiukiOS
a POSIX host for that layer: dynamic ELF loading, `MAP_FIXED` and
file-backed `mmap`, Unix-domain sockets with descriptor passing, `poll`,
alternate signal stacks, compiler TLS, process creation with `exec`
semantics — almost every exclusion of posix-subset.md — plus a CiukiOS
display, audio and input driver. Advantages: the widest compatibility and
upstream fixes. Costs: the largest kernel and libc expansion of any option,
the server's per-process overhead at 128 MiB, and fault containment and
latency that depend on code tuned for Linux.

**Option C — hybrid (recommended).** CiukiOS writes the core — the PE loader
and `ntdll`, `kernel32`, `user32`, `gdi32`, `advapi32`, `winmm`, `ddraw`,
`dsound`, `dinput` — directly on `CIUKI_SYS_W32`, `w32srv`, the desktop
protocol, the presenter and `/dev/audio`. The installer-facing upper DLLs of
phase P4 and the codec DLLs of P5 (`comctl32`, `comdlg32`, `shell32` subset,
`shlwapi`, `ole32`, `oleaut32`, `riched20`, `cabinet`, `advpack`,
`setupapi`, `version`, `lz32`, `msacm32` and its ADPCM drivers, `msvfw32`
and its codecs, possibly `msvcrt`) are taken unmodified where possible from
one pinned Wine release, built as PE images with the pinned clang. These
DLLs call documented Win32 functions of the core plus a small set of Wine
debug and helper exports, which the CiukiOS `ntdll` provides; patches MUST
be kept minimal and recorded. Wine's conformance tests (`dlls/*/tests`, PE
programs that check documented and observed Windows behaviour) SHOULD be
built for the core DLLs and run in the `f5-win32` suite as additional
evidence; failing tests are recorded, not hidden.

Reasons for recommending C:

1. **Memory and containment.** The core is sized by the census, runs on the
   kernel objects and presenter directly, and needs no POSIX host layer or
   per-session server on the hot path; it fits the 128 MiB budget and keeps
   W1's isolation.
2. **The long tail is where reuse pays.** Installer UI, shell, COM, cabinets
   and codecs have little to do with CiukiOS integration and much to do with
   accumulated behaviour; reusing them removes the largest risk of option A.
3. **Testability.** Core code is CiukiOS code with host tests in the existing
   harnesses; reused DLLs come with conformance tests that run on the guest.
4. **Licensing stays clean.** Reused DLLs remain separate LGPL-2.1-or-later
   files with their notices and corresponding source recorded in the
   dependency manifest; the GPL-2.0-only kernel and the MIT SDK are separate
   works; core DLLs are CiukiOS code (licence decided with Q2).
5. **The boundary is the documented Win32 API**, which is stable, so the
   core and the reused DLLs can be upgraded independently, and option B
   stays open for a later step if the core's compatibility plateaus.

Risks of C, to be tracked: `user32`/`gdi32` fidelity determines how the
reused common controls behave (mitigated by the conformance tests and the
installer cases); the reused DLLs may import more internal exports than
expected (f5-14 starts with an import audit and stops for lead review when
the list exceeds 40 functions); Wine's PE build expects a mingw-w64 header
set, which f5-02 pins and records.

## Directives

Implementer suggestions follow AGENTS.md (Codex `gpt-6.1-sol` by default,
`xhigh` for kernel work). Every directive is host-testable; the lead runs
QEMU and hardware. Each directive's acceptance below is in addition to the
relevant MUSTs of this contract.

- **f5-02 — PE toolchain, fixture set and API census** (Sol, high). Pin the
  clang `i686-pc-windows-msvc`/`i686-w64-mingw32` targets, `lld-link`,
  `llvm-dlltool` and a mingw-w64 header release (versions and hashes in
  `config/toolchain.json`); `.def`-based import libraries for builtins; the
  PE fixture set (≥ 60 fixtures: every loader rule, SEH, TLS, resources,
  malformed images); a host census tool listing DLLs and functions imported
  by any PE (installers, and the executables they install, extracted with
  host tools where possible, otherwise after the first guest installation);
  the census of the acceptance set committed as text (names only, no
  payload bytes). *Acceptance:* reproducible fixtures
  (identical hashes on two builds), census complete for the ten titles and
  their installers, all host tests pass.
- **f5-03 — Kernel personality core** (Sol, xhigh). PE recognition at spawn,
  `win32` personality flag, `CIUKI_SYS_W32` numbering and records, address
  layout, the virtual-memory service, TEB allocation and the GDT index 9 FS
  descriptor, per-process CR4.OSFXSR with FXSAVE contexts, exception delivery
  and continue, shared user data page, PIT performance counter, refusal of
  F2 `mmap`/`brk` in `win32` processes; execution-abi.md amendment.
  *Acceptance:* host tests of every record and validation rule; F0–F4 probes
  unchanged (GDT probe updated for index 9); QEMU `exceptions` and `memory`
  cases with fixtures; native SSE still raises #UD.
- **f5-04 — Kernel objects, sections and service endpoints** (Sol, xhigh).
  Handle tables and ledger, all objects of "Handles and kernel objects",
  waits, APCs, message-queue objects and cross-process messages, suspension
  and contexts, image/pagefile/file sections with shared builtin pages,
  service registration for `desktop` and `w32srv` with per-process
  endpoints, priority mapping and focus boost, memory measurements of the
  budget table. *Acceptance:* host model tests (wait-all atomicity,
  abandonment, APC order, 10,000-message quota), QEMU `objects` case, 100
  create/exit cycles with ledgers back to baseline.
- **f5-05 — Loader and `ntdll` core** (Sol, xhigh). Everything in "PE
  loader", SEH dispatch and unwind, `UnhandledExceptionFilter` hook, heaps,
  critical sections, the `Rtl*` and helper exports needed by the reused DLLs
  (from the f5-14 audit), builtin store manifest. *Acceptance:* all f5-02
  fixtures pass in QEMU; malformed fixtures fail with the named check.
- **f5-06 — `kernel32`** (Sol, high). Files and the VFS drive-path service
  (share modes, locks, aliases, wildcards, free space), memory APIs and file
  mappings, processes and `CreateProcess` search rules, threads, waits,
  fibers, TLS, time and performance counter, version and system information,
  environment and command line, code pages, profile APIs, Toolhelp32,
  console subsystem minimum (handles to a log), error-code mapping.
  *Acceptance:* census coverage for kernel32 = 100 % (implemented or W21
  stub), host and QEMU tests, Windows 98 INI parity cases.
- **f5-07 — `w32srv`, registry and `advapi32`** (Sol, high). Hives, commit
  and recovery, default keys, atoms, window directory and handle allocation,
  PROGMAN DDE, virtual restart with `WININIT.INI` and `RunOnce`.
  *Acceptance:* torn-write recovery in QEMU, restart of a killed `w32srv`
  without losing committed keys, registry API error codes.
- **f5-08 — Presenter and display amendments** (Sol, xhigh). Source formats
  RGB565, XRGB1555, 8-bit indexed with palettes, integer scaling and gamma
  LUT; delegated present lease with synchronous revocation; mode switching
  on behalf of a lease holder through the desktop; execution-abi.md and
  f1-acceptance amendments. *Acceptance:* pinned conversion digests per format
  and scale; revocation race test; T23 check by the lead of 640×480×8 and
  320×200×8 at 2×.
- **f5-09 — Desktop protocol v3 and desktop integration** (Sol, high).
  Multiple windows per channel, UTF-8 titles, styles, placement, cursors,
  capture, clip, warp, relative motion, full-screen lease handling and
  hot keys, Start-menu and desktop `.lnk` entries, Control Panel "Add/Remove
  Programs", the crash and restart dialogs. *Acceptance:* desktop host suite
  extended with pinned frames; native clients of v1/v2.1 unchanged.
- **f5-10 — `user32` and `gdi32`** (Sol, xhigh; the largest directive, MAY be
  split by the lead). Windows, classes, messages, timers, dialogs from
  resources, standard controls, menus, message boxes, clipboard, cursors and
  icons, painting; DCs, DIB engine for 1/4/8/16/24/32 bpp, DIB sections,
  bitmaps, pens, brushes, regions, `StretchBlt`, palettes, fonts with a
  rasteriser and metric-compatible faces (question Q10). *Acceptance:*
  reference-image tests, dialog layout tests in dialog units, the selected
  conformance tests recorded, the NSIS fixture's UI completes.
- **f5-11 — DirectDraw** (Sol, xhigh). "DirectDraw" and "Full-screen exclusive
  mode". *Acceptance:* fixture games (windowed and full-screen, 8/16/32 bpp,
  colour keys, GetDC, Alt+Tab ten times), Diablo shareware reaches its menu
  on QEMU.
- **f5-12 — Audio: `winmm`, DirectSound, MIDI, ACM, AVI** (Sol, high).
  *Acceptance:* sample-exact host mixer tests, QEMU WAV capture of a scripted
  sequence, cursor accuracy within one quantum, MIDI file through MCI.
- **f5-13 — Input: messages and DirectInput** (Sol, high). *Acceptance:*
  mapping tables for both layouts, buffered-data overflow semantics, focus
  loss, relative motion under exclusive access in QEMU.
- **f5-14 — Reused upper DLLs** (Sol, high). Pin the release and archive
  hash, import audit, PE build of the P4/P5 list, notices and source offer,
  patches recorded, `msvcrt` measurement and decision. *Acceptance:* T1
  licence and hash checks; each DLL loads in QEMU and passes its selected
  conformance tests or records the failures.
- **f5-15 — Installer flows and shell integration** (Sol, high). Everything
  in "Installers"; MAY: starting DOS programs from Win32 through the
  desktop's DOS window. *Acceptance:* the NSIS fixture and the ten acceptance
  installers complete on QEMU; virtual restart processes a fixture
  `WININIT.INI`.
- **f5-16 — Shims, error reporting and diagnostics** (Sol, high; Luna,
  medium, for the database text). *Acceptance:* matching tests, crash dialog
  and record for each exception class, stub records rate-limited.
- **f5-17 — Acceptance suite** (Sol, high). Workload manifest and download
  with digests, fixture disk and CD image builder, QEMU input player and
  screen-region checks selected by fw_cfg, per-title scripts for operators,
  records and `docs/validation/` template. *Acceptance:* one title end to end
  on QEMU under the runner's limits, the manifest reproducible.
- **f5-18 — Winsock and DirectPlay on F6** (Sol, high; after the F6 socket
  gate). *Acceptance:* `WSAAsyncSelect` and UDP broadcast fixtures; one LAN
  title between the two laptops when Q13 allows. Not part of the F5 gate.

Dependencies: f5-02 first; f5-03 → f5-04 → f5-05 → f5-06; f5-07, f5-08 and
f5-09 in parallel after f5-04; f5-10 after f5-06 and f5-09; f5-11 after
f5-08 and f5-10; f5-12 and f5-13 after f5-06 and f5-09; f5-14 after f5-05
(audit) and f5-10 (runtime); f5-15 after f5-07 and f5-14; f5-16 and f5-17
may start after f5-05.

## Open questions for the owner

- **Q1 — Numbering.** f5-01 is the Tiny C Compiler/CiukIDE directive of the
  previous scope. Keep f5-02–f5-18 as written, or renumber?
- **Q2 — Implementation option.** Approve option C (CiukiOS core plus the
  reused LGPL upper DLLs of one pinned upstream release, as described under
  "Implementation options"), or prefer A or B? Which
  licence for the CiukiOS-written core DLLs (MIT like the SDK, or
  LGPL-2.1-or-later like the reused DLLs)?
- **Q3 — Win16.** 16-bit setup stubs exist (the NFS II SE demo's
  self-extractor is Win16; InstallShield 3 setups are Win16). Add a Win16
  subsystem in F5, defer it to a later step, or exclude it?
- **Q4 — Reported identity.** Confirm Windows 98 SE as the default. Which
  DirectX version should the registry and `dsetup` report before F7: 7.0a
  (closest to what F5 provides; some 2001–2004 installers refuse to continue) or 8.1 (lets them
  install, but they find no Direct3D 8)?
- **Q5 — Framebuffer mapping.** May a later amendment map the framebuffer
  into a full-screen game under the revocable lease (faster, authentic
  `Lock` on the primary surface), or must games always present through the
  kernel copy?
- **Q6 — MIDI sound.** FM synthesis on the existing OPL3 model (small,
  period-authentic on laptops of the era) or a General MIDI wavetable bank
  (2–8 MiB of RAM, licence of the bank to choose)?
- **Q7 — "Playable" on hardware.** Thresholds for the gate, e.g. median
  ≥ 20 fps and 5th percentile ≥ 12 fps at the title's default resolution,
  at most one audio underrun per minute?
- **Q8 — CD audio.** Is Red Book audio playback (MCI `cdaudio`) required in
  F5? None of the ten titles needs it; many retail games of the era do.
- **Q9 — Local time.** Keep UTC for Win32 programs (consistent with the
  native and DOS sides), or introduce a configured time zone for all of
  CiukiOS?
- **Q10 — Fonts.** Approve open-licence metric-compatible faces (for
  example Liberation fonts, SIL OFL) and a CiukiOS bitmap face for
  "MS Sans Serif"/"System", rasterised by FreeType (FTL or GPLv2)?
- **Q11 — Retail titles.** Confirm that titles needing kernel-mode copy
  protection remain out of scope (N2), and that the gate uses only the ten
  freely distributed titles.
- **Q12 — RAM.** Is 128 MiB the only qualification size for F5, or must the
  64 MiB scope target of hardware-baseline.md also be measured?
- **Q13 — Win32 IPX.** Should F6 provide `AF_IPX` sockets for Win32 games
  over the same Ethernet bridge as the DOS VMs, for StarCraft, Diablo and Age
  of Empires LAN play?
- **Q14 — Acceptance set.** Approve the ten titles and the reserves, in
  particular moving Quake III Arena to the F7 gate (it requires OpenGL)?
- **Q15 — Locale.** English (United States) locale and code page 1252 with
  the desktop's US or Italian keyboard layout, or an Italian locale?

## Interfaces required from other contracts

- `execution-abi.md`: the `win32` personality flag, `CIUKI_SYS_W32` and its
  records, the GDT index 9 descriptor, per-process CR4 SSE policy, FXSAVE
  contexts, exception delivery, sections, endpoints and present leases
  (f5-03, f5-04, f5-08 amendments).
- `boot-memory.md`: the Win32 process layout as a third address-space kind,
  and measured budget values.
- `vfs-storage-contract.md`: lookup by 8.3 alias if missing, drive-path
  service for Win32 with share modes and locks, change hooks (MAY).
- `device-firmware-ownership.md`: no new device ownership; presenter lease
  semantics; any future retrace polling.
- `f4-audio-media-contract.md`: one stream per Win32 process within the
  eight-stream limit; no change to the mixer.
- `desktop.md`: protocol v3 and the desktop features of f5-09.
- `network-contract.md` (F6): renumbering of its proposed calls after
  75–77, and the Winsock and IPX questions.
- F7 contract: Direct3D 7/8 behind `ddraw`/`d3d8` and `opengl32` on Mesa.
