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
