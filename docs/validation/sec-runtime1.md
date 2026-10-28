# Session runtime protocol conformance

Research and implementation decisions for `docs/directives/sec-runtime1.md`.

Microsoft's *MCSE Training Kit: Windows XP Professional*, Lesson 4,
[Logging On and Off Windows XP Professional](https://flylib.com/books/en/2.732.1.9/1/)
(original Microsoft Press text, mirrored), describes clicking an account on the
Welcome screen: a protected account prompts for its password; an unprotected
account logs on immediately. CiukiOS must still authenticate the empty secret
through LOGON and ACCOUNTS. The account list supplies a non-secret, AUTH-only
blank-logon hint for the existing ready-image exception. It never authorizes
unlock, bypasses authentication, or grants a second blank-account exception.

Microsoft documents the four per-machine/per-user
[Run and RunOnce keys](https://learn.microsoft.com/windows/win32/setupapi/run-and-runonce-registry-keys).
The existing StartUp snapshot already enumerates both hives through the shell
registry adapter; no new autostart mechanism is needed. In a LOGON session,
registry operations belong to the SYSTEM REGISTRY broker, while USER/shell
operations belong to the session w32srv. Preserve that separation and the
broker's authenticated caller checks.

Repository comparison found an earlier failure than the session-only server's
registry refusal: `desk_w32_exchange` already routes registry requests to the
SYSTEM broker. Its account identity lookup performs LOOKUP at generation 0,
then PROFILE at the returned generation. `reg_registry_rpc` omitted the latter
expected generation from the CSC1 envelope for service 2, while the production
ACCOUNT decoder requires body and envelope generations to agree. Correct the
client envelope, keeping generation 0 valid for pure reads. Conformance tests
must exercise the production fragment encoder and service decoder together.

The public 144-byte record keeps its size and reserved word. AUTH LIST flags
now allow bit 2 as the blank-logon hint; management LIST/LOOKUP retain their
existing enabled/type flags. No public SDK or kernel ABI record changes.

## Route audit and host coverage

| Path | Production route and evidence |
| --- | --- |
| Desktop readiness; HKLM/HKCU Run/RunOnce; Add/Remove Programs | `win32_registry.c` → native `desk_w32_exchange` → `reg_remote_win32` → `rb_server_step` → `rb_exchange` → `wr_dispatch`. New native-branch fixture exercises the entire route, including nested broker → ACCOUNTS LOOKUP/PROFILE. Administrator, Standard and Guest use their own retained identities. HKLM RunOnce deletion remains denied for non-administrators; HKCU consumption succeeds. |
| StartUp folders and selection | `startup.c` reads the shared and profile StartUp folders after the registry snapshot, then its profile-local Desktop selection store. Existing desktop startup tests cover enumeration and launch ordering. The worker now emits `startup: snapshot ready entries=N` before execution. |
| Desktop `.exe` launch | `desk_shell_open_record` → `ciuki_win32_shell_launch` → `launch_execute` performs broker readiness before the shell-launch helper. The first-boot acceptance case explicitly launches the already bundled `C:\WINDOWS\CDPLAYER.EXE`, waits for its Win32 start and focused window, then closes it. Guest execution remains for the lead. |
| File associations | Native desktop non-program files use `desk_catalog_file`/native applications; they do not query HKCR. Win32 ShellExecute uses ADVAPI32 RegOpen/Query/Close on HKCR, whose NTDLL server transport routes registry requests to the same SYSTEM broker. No session-server registry fallback is needed. |
| Control Panel | Its Win32 inventory/StartUp adapters share the desktop route. Native settings use libreg; machine System/Programs roots use the SYSTEM broker and profile settings use their authorized local store. The new fixture runs the actual account client through production account framing and handlers, including all management kinds, RESERVE and STATUS after a lost successful reply. Existing security/control suites cover page policy and the native registry API. |
| w32srv → ACCOUNTS | Native `wr_user_start` runs unchanged against the real account frame decoder and PROFILE handler for all three roles. Generation-0 reads return the current committed mapping. |
| libc → ACCOUNTS | `pwd.c` and `account_link.c` run unchanged against the actual account frame decoder, worker and reply encoder. Explicit LOOKUP/PROFILE requests additionally exercise generation 0/current/stale. |
| LOGON → ACCOUNTS | Existing H10/H12 production coordinator/client/server/kernel scenarios are retained; the new Welcome tile scenario additionally connects the actual UI encoder, LOGON handler and ACCOUNT handler. Empty-secret logon succeeds only through authentication; a protected record prompts. |
| LOGON → REGISTRY / OWNER | These direct requests do not exist in `apps/logon`. `links.c` connects only to AUTH. The coordinator uses kernel session/profile/launch records. ACCOUNT's production OWNER client and kernel OWNER decoder are covered by the existing H15 system-owner integration suite, rather than invented LOGON protocol peers. |

The new conformance fixture's substitutes are restricted to kernel
identity/channel delivery, storage and clocks. It does not implement a fake
ACCOUNTS/REGISTRY decoder or handler. In contrast to the older host builds, it
compiles the `__CIUKIOS__ && !__linux__` desktop, libreg and w32srv branches.

The accepted native SDK/kernel ABI remains unchanged. AUTH LIST's bit-2 hint is
private service-protocol data; no public SDK header or `ciuki/abi.h` was edited.
The lead should reconcile these acceptance steps with `sec-user-06` during
integration. No other worktree was modified for coordination.

The owner's original Windows 98 Resource Kit was also checked read-only:
`build/rskt/wrkc27_2.htm` (Starting Dr. Watson) documents a shortcut in the
StartUp folder, `wrkappd_3.htm` (RunOnce.BatchDelay) demonstrates machine
RunOnce installation entries, and `wrkc10_5.htm` documents RunServicesOnce
before the logon prompt. This change fixes the existing session snapshot; it
does not move the pre-existing six-source StartUp policy between boot stages.
The contemporary Microsoft Run/RunOnce page is not used as evidence that
Windows 98 and XP have identical security or service-start ordering.

## SYSTEM REGISTRY profile lookup

A further production comparison found that PROFILE admitted sealed LOGON and
ACCOUNT roles, but omitted sealed REGISTRY. `rb_account_current` is a SYSTEM
REGISTRY caller, not ACCOUNT. An early fixture used the ACCOUNT role and was
corrected to the real REGISTRY role before final verification. The broker
cannot select an authoritative personal hive without this mapping.

Microsoft's [LoadUserProfileA documentation](https://learn.microsoft.com/en-us/windows/win32/api/userenv/nf-userenv-loaduserprofilea)
distinguishes the service's own identity from the user's loaded hive and
permits LocalSystem/profile-management services to load that hive. CiukiOS's
binding requirement is its existing sec-k06 SYSTEM broker ownership of the
private hives (`user-security-kernel.md`, Win32 registry broker). The narrow
implementation decision is to allow read-only PROFILE for a kernel-retained
REGISTRY role only when uid and session are both 0. A plain uid-0 process,
an ordinary session process, and a mismatched role/credential remain denied;
this does not authorize account mutation or reveal verifiers.

## Host verification boundary

The new native-peer fixture and real Welcome UI scenario reach their functional
PASS markers, including all three roles, zero/current/stale PROFILE generations,
and SYSTEM REGISTRY/ordinary uid-0/wrong-credential authorization cases.
`tests.host.test_acceptance.SuiteTests.test_runtime_session_startup_and_first_win32_predicates`
and `test_cases_are_valid_and_bounded` pass (2 tests).

The required kernel, security, desktop and Win32 commands were run sequentially
with `nice -n 19`, retaining each script's sanitizer settings. The kernel stops
at the sandbox LeakSanitizer/ptrace failure, as it also did before this change.
Security runs 99 tests, desktop 248 and Win32 405; their complete gates are not
green. Besides LeakSanitizer restrictions, unchanged host inputs expose a
power-policy sanitizer link overflow, a MIDI diagnostic-count assertion, a
pinned census mismatch and a SIGSYS exit from an i386 division helper. The
existing acceptance Quake path assertion predates the protected installer store
and also fails; that case was not modified. These are recorded for the lead,
not waived. Exact commands/results and local logs are in
`build/codex/sec-runtime1-report.md`. No image build or guest run was performed;
QEMU confirmation remains the lead's acceptance work.
