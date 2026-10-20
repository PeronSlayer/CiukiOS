# F5-07B shell and session research

Researched before implementation on 2026-10-20. This work retains the f5-07A
hives and the f5-09 CIUKLNK3 format. The small `ws_link_store` seam is reserved
for the parallel f5-15A writer; it uses `apps/desktop/shell_link.h` directly.

Primary sources inspected:

- Microsoft Platform SDK, [Command-String Interface](https://techshelps.github.io/Win32/html/m40p2g.htm): bracketed commands, parenthesized comma-separated arguments, quoting delimiters, concatenated commands, PROGMAN service/topic.
- Microsoft [KB Q102575, original article archived](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/102575): PROGMAN/PROGMAN and SHELL/APPPROPERTIES names, CRLF Groups, quoted item title and command, directory/icon/position/index/hotkey/minimize columns; group header includes the show command.
- Microsoft [KB Q138800](https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/138/800.HTM): CreateGroup/ShowGroup/AddItem and ReplaceItem followed by AddItem, selected group semantics; references SDK chapter 17, Shell Dynamic Data Exchange Interface.
- Microsoft [Using DDE](https://learn.microsoft.com/en-us/windows/win32/dataxchg/using-dynamic-data-exchange): synchronous initiation ACK, posted execute ACK, CF_TEXT data and CRLF, global-memory and atom ownership, symmetric termination. The process-local USER proxy copies command/data bytes over authenticated w32srv RPC; process-private global-memory handles never cross address spaces.
- Microsoft [DdeClientTransaction](https://learn.microsoft.com/en-us/windows/win32/api/ddeml/nf-ddeml-ddeclienttransaction): synchronous execute/request, data handles and result status. Unsupported asynchronous transactions fail explicitly.
- Microsoft [ExitWindowsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-exitwindowsex), [WM_QUERYENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-queryendsession), [WM_ENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-endsession): query, veto, successful or cancelled end notification, ENDSESSION_LOGOFF. Modern NT privileges/UIPI/shutdown ordering are not imported.
- Microsoft [InitiateSystemShutdown](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-initiatesystemshutdowna): current NT documentation is not evidence of 9x support; the explicit directive maps local calls to this virtual coordinator, without a physical reboot.
- Owner's Windows 98 Resource Kit, inspected read-only in the main checkout: chapter 10 `wrkc10_5.htm`, RunServicesOnce runs before logon once; chapter 27 `wrkc27_2.htm`, RunOnce WUCheckShortcut; Appendix D `wrkappd_3.htm`, ordered batch RunOnce commands and WININIT.INI DIRNUL. Private book contents are not copied into the repository.

Repository comparison: the desktop owns confirmation and process termination;
native W32_MESSAGE calls are not authorized. USER therefore relays session
notifications on each owning UI thread at its message pump, with w32srv keeping
query/reply state and the desktop enforcing the ten-second deadline. No new
kernel privilege or pointer-message transport is introduced. Unresponsive
clients cannot block the desktop input loop. RunOnce deletion must commit
before launch; a failed hive write must stop handoff. Registry insertion order
and queue priority follow the directive, rather than modern NT ordering rules.
WININIT operands are destination=source moves, in file order, with NUL deletion;
missing sources and refused targets are logged and skipped. Restart uses a
persistent cursor and deletes RunOnce values before launch (at-most-once handoff).
The unavoidable interruption between durable deletion and process creation may
skip an entry, matching delete-before-execute rather than claiming exactly-once
process execution across a crash.

The additive v3 `DESK_V3_WIN32_HWND` (78) record has a four-byte nonzero HWND,
no descriptors, and the ordinary request/window/generation envelope. USER sends
it after CREATE. Desktop stores it on that authenticated client's window slot;
identity cannot change during that generation. At session begin the desktop
copies HWND/PID pairs in descending compositor order. The broker accepts only
pairs matching its own directory, appends unranked invisible windows, and
serializes queries in that order. Forged foreign identities cannot add victims.

The Win32 ntdll transport announces its authenticated PID before each request.
One process-local identity atom in SERVICE.CRS preserves each announced client,
including programs with no windows. Startup reconciliation removes dead owners
using the existing OS snapshot before rebuilding the live set. Native registry
readers do not announce themselves and are not session termination targets.
RunOnce uses a dedicated PE helper (`runonce.exe`) whose sole process-creation
call is `CreateProcessA(NULL, original_command, ...)`, with C:\WINDOWS as cwd.
It reports the child PID over an inherited native channel and waits on its
process handle; the desktop owns the five-minute termination watchdog.

The selected group follows Program Manager's shared focus, not a PID-local
working directory (Q138800). Conversation IDs are drawn from the service's
existing durably reserved ID ranges; a reconnect after a service crash cannot
alias a stale conversation to a newly opened one. The bounded group request
uses Q102575's quoted title/command and comma-separated property columns.

Cold-start identity boundary: the desktop passes `--fresh-clients` until its
first successful service round trip. That server purges prior process-local
atoms/windows before registration, preserving hives and global atoms. Later
supervised service restarts omit the flag and reconcile live identities. This
prevents a prior-boot Win32 PID from authorizing termination of a new native
process that reused the number; a failed first registration retries the purge.

Additional identity review source: [Microsoft Process Handles and
Identifiers](https://learn.microsoft.com/en-us/windows/win32/procthread/process-handles-and-identifiers).
A saved numeric PID alone is not proof of a current Win32 process. The fresh
Desktop boundary above avoids relying on numbers from an earlier OS lifetime.

[DeleteFileA](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-deletefilea)
requires access-denied for a read-only file. The shell POSIX adapter checks
write-mode bits before NUL deletion as well as replacement, so Linux's ordinary
unlink permission rules do not silently erase a read-only WININIT operand.
The fixture checks a real chmod-0444 operand, in addition to injected rename
errors. Modern NT share-delete and ACL rules are not added.

Host integration evidence: the DOS loading replay's scenario-11 settings
focus/restoration assertion was reproduced using unchanged DOS source/test and
`HEAD` versions of the desktop protocol header/implementation. This failure is
outside f5-07B. The source-tree diff for the DOS subject and test was empty;
compiler argv and the matching assertion are retained in the worktree report
logs. No DOS source was changed for this directive.
