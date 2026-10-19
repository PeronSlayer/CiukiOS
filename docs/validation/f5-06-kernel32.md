# F5-06 kernel32 research and host validation

Research checked on 2026-10-19. The implementation is
original CiukiOS code; no Wine implementation was copied.

| Primary source | Decision against the existing implementation |
| --- | --- |
| [CreateFileA](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea) | Preserve INVALID_HANDLE_VALUE, distinguish dispositions and successful already-exists, and use the VFS's symmetric share checks. |
| [CreateProcessA](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessa) | Preserve the command string; module search and publication belong to the kernel service, rather than a POSIX command interpreter. |
| [VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [MapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile) | Wrap F5 VM/sections; preserve allocation versus page granularity and publish outputs only on success. |
| [WaitForMultipleObjects](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects) | Forward atomic waits to F5-04, validating public count limits before the transport (which also supports zero-handle Sleep). |
| [TlsGetValue](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Fibers](https://learn.microsoft.com/en-us/windows/win32/procthread/fibers) | TLS is per TEB; a successful get clears last-error. Fiber switches preserve the contract's x86 state in user mode. |
| [GetPrivateProfileString](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprivateprofilestring), [WritePrivateProfileStringA](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-writeprivateprofilestringa) | Case-insensitive keys/sections, quoted values, double-NUL enumeration/truncation. Apply the contract's Windows 98 first-match/direct-file rules; do not apply NT IniFileMapping. |
| [GetVersion](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getversion) | Consume the existing W3 Windows 98 SE identity, not the current host OS. |
| [SystemTimeToFileTime](https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-systemtimetofiletime) | Check the Gregorian date before converting from the 1601 epoch; preserve output on invalid input. |
| [WSAStartup](https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-wsastartup) | Offline failures must be observable and must not masquerade as connected sockets. |

F1-84 is written but unmerged: `vfs.h` explicitly has no byte-lock service.
LockFile/LockFileEx/UnlockFile/UnlockFileEx therefore use the generated W21
remainder, including the required names absent from the provisional census.
The coverage JSON records them separately as deferred VFS exports; none is
classified as an implemented source export.
F5-02's original-media census is provisional (`win32-census/README.md`);
the research union and self-authored fixture imports can be covered, but are
not a substitute for that absent acceptance-set evidence. F5-04/F5-05B reports
are not present under build/codex in this fresh worktree; their directives,
delivery index, source and production host harnesses were read instead.

See `build/codex/f5-06-report.md` for the executed checks and remaining gaps.

Additional primary checks: [MultiByteToWideChar](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar),
[WideCharToMultiByte](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-widechartomultibyte),
[GlobalAlloc](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-globalalloc),
[GlobalUnlock](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-globalunlock),
[CreateFileMapping](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfilemappinga),
[CreateThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createthread),
[TerminateThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminatethread),
[GetPrivateProfileSection](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprivateprofilesectiona),
[GetPrivateProfileSectionNames](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprivateprofilesectionnamesa).
Single-byte conversion checks capacity before publication; fixed global blocks
ignore unlock, moveable blocks track counts; a thread return becomes ExitThread;
forced termination is deferred to a safe user return. INI enumeration includes
each entry's NUL and excludes only the final extra NUL. Truncation reserves two
NULs. The independently authored tests cross-check that count against
[Wine 10.0's profile tests](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/kernel32/tests/profile.c)
(edge-case cross-check only, no implementation copied).


[Toolhelp snapshots](https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-createtoolhelp32snapshot)
are immutable copied records; process/thread collection excludes IRQ-driven
list mutation. The loader's compatible list supplies caller modules.
[Profile struct reads](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprivateprofilestructa)
and [writes](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-writeprivateprofilestructa)
use checked hex bytes and an appended byte checksum; invalid checksums leave
the destination unchanged.
[Section replacement](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-writeprivateprofilesectiona)
preserves other sections and checks the 64-KiB growth bound before opening a
file for truncation.
[SwitchToFiber](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-switchtofiber)
permits properly synchronized cross-thread switching. The first fiber word is
its user data, matching the period inline GetFiberData convention.

Final transport review against Microsoft's
[ReadFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile),
[WriteFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile)
and [CopyFile](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-copyfilea)
identified two wrapper requirements: chunk requests at the existing native relay
limit while preserving total counts and the caller's OVERLAPPED offset, and
preserve source attributes/timestamps when copying. The wire's positioned-I/O
bit is 0x10000; its low bit is not that operation. Regression tests must observe
the actual records, rather than just testing the file endpoint independently.
The CreateProcess startup-info block must be installed after inherited handles
exist and before publication, on the PE branch. Its earlier placement in the
ELF branch could never execute; the production spawn test will read back the
child's version-3 parameters and verify rollback for an unreadable title.
As documented by CreateProcess, standard-handle values themselves are copied
unchanged; their usability depends on ordinary handle inheritance. Unknown
creation options must fail explicitly, and supported priority flags must be
applied before publication rather than silently ignored. The qualified native
realtime denial remains the recorded contract discrepancy.

CreateProcess's documented unquoted-name ambiguity is resolved by the facade
trying successive whitespace boundaries; the kernel still receives an explicit
candidate and the original unparsed command string. Retries apply only to
missing-file/path results, never to loader or permission failures.

[ResumeThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-resumethread)
must accept the initial thread handle returned to the parent by CREATE_SUSPENDED.
The F5-04 helper's same-process restriction is retained for Get/SetThreadContext
as the contract requires, but removed for handle-based suspend/resume and
termination. Otherwise a successfully created suspended PE child could never
be resumed by its creator. Tests exercise these operations from the parent;
termination by the private TID fallback remains limited to the calling process.

## Amendment 1 continuation research (2026-10-19)

The lead amendment supersedes the earlier partial-delivery limitations above.
Primary Microsoft references checked before implementation:
[DLL search](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order),
[CreateFileA](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea),
[DeleteFileA](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-deletefilea),
[LockFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex),
[CreateProcessA](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessa),
[GlobalMemoryStatusEx](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-globalmemorystatusex),
[MultiByteToWideChar](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar),
[ReadProcessMemory](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-readprocessmemory),
[WriteProcessMemory](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-writeprocessmemory),
[profile writes](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-writeprivateprofilestringa),
[fibers](https://learn.microsoft.com/en-us/windows/win32/procthread/fibers),
[APCs](https://learn.microsoft.com/en-us/windows/win32/sync/asynchronous-procedure-calls),
[time zones](https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-gettimezoneinformation).

Repository comparison: builtin paths currently point to the writable system
DLL directory; native descriptions already retain F1-84 file identities, but
range locks still require legacy descriptions and omit native lock owners.
The continuation must connect these descriptions to the common lock table,
include delete sharing in both directions, and recheck protected ancestry
through an open handle after a native rename. Named Unicode exports follow
the amendment's Windows 9x w-stubs rule; internal Unicode helpers remain usable
by ANSI wrappers. Unicode environment blocks are converted explicitly.
Realtime requests use HIGH's scheduler band, never the audio reservation.
UTC remains the sole zone and unsupported composite/glyph conversion is logged.
Profile read/modify/write requires one server-owned transaction across processes.
Microsoft's current NT/packaged search and INI mapping details do not supersede
the explicit Windows 98 contract and the amendment's protected store order.

### Continuation decisions and host boundaries

The store's PE images and both manifests move together to `/System/Win32`;
`C:\WINDOWS\SYSTEM` remains the writable installer directory returned by
GetSystemDirectory. Loaded modules and the manifest's known builtins retain
precedence over application/current/system/Windows/PATH lookup. Image payload
validation checks hashes, addresses, binary-manifest agreement and the protected
store destinations before any image mutation. The already integrated native
server path remains `/System/Win32/w32srv`. DLL base selection is unchanged.

REALTIME_PRIORITY_CLASS is accepted by CreateProcess and SetPriorityClass,
normalized to HIGH_PRIORITY_CLASS and logged as `prio=realtime→high`. The
scheduler's native realtime denial and audio reservation are unchanged.

Native file descriptions retain a common F1-84 identity and expose a synthetic
legacy description only to the common range-lock table. Shared locks allow
reads and deny all writes, including the owner's; exclusive locks exempt only
the owning description and PID. Inheritance does not transfer this exemption.
64-bit ranges can extend beyond EOF, exact unlocks preserve adjacent claims,
and close/death remove the appropriate description/context claims. The native
ABI still returns its existing errno set. Win32 maps lock failures to 33 and
lock-table exhaustion to 36. Share-delete is a separate third access/deny bit;
metadata-only opens do not acquire read/write/delete access. DeleteFile and
delete-on-close defer removal until the last open description closes, refusing
new opens while pending. Mutation through an open Win32 description rechecks
current ancestry, including file-section writeback and final deletion after a
native rename into System.

Public W exports outside the implemented environment subset use returning,
once-per-export diagnostic refusals with error 120, preserving output buffers.
The three census environment W exports remain implemented. The provisional
CompareStringW/GetLocaleInfoW/GetStringTypeW/LCMapStringW entries remain W21
Unicode refusals, now returning 120 instead of raising a nonreturning stub.
They are explicitly classified in the coverage table. The original-media
census remains absent; no additional Unicode requirement is inferred from it.
CREATE_UNICODE_ENVIRONMENT converts a bounded double-NUL block to CP1252 and
rejects unrepresentable text before publication. Loader paths use UTF-8
internally and UTF-16 in the module list; ANSI module APIs round-trip CP1252,
including accented letters and the euro sign. Composite/glyph conversion
requests return 1004 without output mutation and emit a bounded diagnostic.
UTC is the sole zone, with a diagnostic on timezone query.

GlobalMemoryStatusEx interprets pagefile fields as the commitment budget, not
filesystem free space: this service currently has physical backing only, so
`total_pagefile = total_phys = usable_PMM_pages * 4096` and
`avail_pagefile = avail_phys = free_PMM_pages * 4096`. It advertises no disk
swap capacity. Kernel/audio/other-process consumption reduces free backing.
This facade does not change memory limits or reserve additional physical RAM.
The virtual budget is `[0x10000, 0x7fff0000)`; reserved extents, including
uncommitted reservations, reduce available virtual bytes. The legacy structure
caps each byte count at 0x7fffffff; Ex retains 64-bit counts. Available backing
is bounded by total backing before calculating the 0..100 load percentage;
zero-total reports zero load, and excessive reserved pages saturate virtual
availability at zero. Invalid Ex length and transport failure leave the whole
output unchanged. These are the unshimmed values consumed by memstatus-cap.

INI mutation holds one authenticated-PID lease in w32srv across read, modify,
write and flush; a disconnect/death releases it. Kernel32 and advapi32 share
ntdll's recursive process transport lock and fragment sequence. The host
concurrency test runs the production profile parser and lease client in two
real Linux processes, with the production server dispatcher in their parent;
200 independent updates to one file must all survive. Registry lease tests
also exercise contention, foreign release, malformed requests and owner exit.

The Linux i386 fiber/APC runtime test executes the production assembly switch
and callback dispatcher when the adapter is permitted: stack/local state,
SEH and TEB stack bounds, x87 control word, alertable versus ordinary waits,
FIFO and one-/three-argument callback cleanup. This sandbox kills that adapter
with SIGSYS (the established i386 seccomp boundary), so execution is explicitly
skipped and remains unqualified here. Compilation is still checked. Native H0
kernel-model evidence and the 137-fixture corpus do not replace i386 execution,
QEMU or hardware qualification. The QEMU case records the new store path and
remains authored, unrun.

The loader's basename byte buffers also admit UTF-8 expansion of 255 Unicode
characters; its UTF-16 basename remains bounded by 256 words. A long accented
basename regression preserves the logical VFAT path while the host backing
adapter uses a short file (Linux NAME_MAX counts bytes). PATH directories from
an ACP environment are converted before both kernel process search and user
loader lookup. The exceptional CP1252 mappings were checked against Microsoft's
[1998 vendor mapping retained by Unicode](https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WINDOWS/CP1252.TXT).

Mapped-view transfers use a private section endpoint that bypasses byte-range
claims, as Microsoft's LockFileEx remarks require, while retaining access,
media-generation, protected-ancestry and durability checks. Ordinary native,
DOS and Win32 read/write/truncate calls remain subject to the common locks.

### Executed continuation evidence

The full ASan/UBSan filesystem and kernel host suites pass. Filesystem crash
classification covers 2,339 cuts with no unknown diagnostics. The production
kernel loader passes 9 tests and all 137 fixtures; kernel32 passes 6 executed
tests with its i386 runtime case explicitly skipped for SIGSYS. The standalone
loader suite passes 6 native-model tests and skips 6 existing i386 cases for
the same sandbox boundary. Apps and kernel builds, frame/FPU audits, API
reference generation/check and diff whitespace checks pass.

Kernel-only artifact: `build/f0/VMM.ELF`, 7,081,428 bytes, SHA-256
`7a46d9db9ff3b93b2c6a9117cfca8387e8646e649482efbf94d584793e3345d8`. No OS image was built and no QEMU/hardware run was made.
Detailed command logs and the generated 225-name census table are in
`build/codex/f5-06b-report.md`; the same report is appended under Continuation
in `build/codex/f5-06-report.md`. The final memory virtual budget matches
GetSystemInfo's inclusive maximum 0x7ffeffff: both edge 64-KiB blocks are
excluded, and only intersections with that usable range count as reservations.
