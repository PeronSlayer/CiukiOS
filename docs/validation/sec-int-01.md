# sec-int-01: account service bound to the sec-k01 kernel

Scope: integration of the sec-user-01 SYSTEM account service with the sec-k01
kernel (call 101, service channels, kernel-established roles) and its
installation in the image. Lead decisions u01-D1 (`acct_kernel.same_link` bound
to `ciuki_accounts_same_link`) and u01-D2 (`root_occupied` belongs to sec-k02).
Recorded 2026-10-26 on branch `wt/sec-int01` (base `6c254bc37`).

## Research and resulting decisions

- Microsoft [Inside the Windows Vista Kernel, part 2](https://learn.microsoft.com/en-us/previous-versions/technet-magazine/cc162480(v=msdn.10))
  and the Windows 2000 [security subsystem architecture](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-2000-server/cc961760(v=technet.10)):
  the local security authority is a user-mode process started by the session-0
  startup chain (Wininit, earlier Winlogon) and Winlogon/LogonUI collect the
  credentials it verifies. Decision: the account service is the user-mode
  ACCOUNT role image `/System/Services/accounts` that PID 1 starts from its
  sealed manifest (the frozen path of `user-security-kernel.md`, amendment 1 of
  sec-k01; the older `/System/Security/accounts` wording is superseded), and it
  verifies only what LOGON relays.
- [LSA mode initialization](https://learn.microsoft.com/en-us/windows/win32/secauthn/lsa-mode-initialization):
  authentication packages are loaded once at system initialization. Decision:
  the service opens its store once and retries a refused open every 30 s; it
  never exits, because PID 1 would respawn it.
- Microsoft [access tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens):
  authority belongs to the execution context, not to a handle value. Decision:
  `same_link` compares the kernel's never-reused per-pair endpoint generation,
  peer process incarnation and service of two live leases; handles and copied
  subject fields are never identity.
- [POSIX rename](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html),
  [rename(2)](https://man7.org/linux/man-pages/man2/rename.2.html) and
  [link](https://pubs.opengroup.org/onlinepubs/9799919799/functions/link.html):
  rename replaces an existing destination; no-replace needs `RENAME_NOREPLACE`
  (Linux only) or `link()` (absent on FAT). Decision: existence check then
  rename, serialized by the single SYSTEM writer of a SYSTEM-only directory.
- [fsync(2)](https://man7.org/linux/man-pages/man2/fsync.2.html): a file's fsync
  does not commit its directory entry; the directory needs its own fsync.
  Decision: the storage barrier fsyncs the base directory and every directory
  changed since the previous barrier (the OWNER staging tree included). On the
  CiukiOS FAT driver a directory fsync commits the volume transaction.

## Binding (apps/accounts/kernel_adapter.c)

| Callback | Kernel operation |
| --- | --- |
| `lease_acquire` | QUERY_SELF (uid 0, session 0); SERVICE_CONNECT ACCOUNTS answered by this process with the ACCOUNT role; RECOVERY STATUS boot flags (Safe Mode), failure fails the lease |
| `retain_peer`, `peer_alive`, `subject_get`, `release_peer` | lease on an accepted endpoint kept open; QUERY_PEER generation equality on every liveness decision; retirement deferred until the last lease/exchange |
| `same_link` | `ciuki_accounts_same_link` with the sealed binding: both leases live, same pair generation, peer pid/process generation and service |
| `active_uid`, `enrollment_authorized` | SESSION_QUERY over the issued ids; owner = uid of the session's live processes (QUERY_PROCESS); a live session without a process is busy for every uid |
| `auth_binding` | retained live LOGON peer (uid 0, session 0), current LOGON/ACCOUNT role generations, nonzero epoch; UNLOCK: ACTIVE session, locked uid = its owner |
| `role_generations` | ACCOUNT from the lease CONNECT; LOGON from the ACCOUNT->LOGON link (SERVICE_CONNECT LOGON) |
| `auth_outcome` | CSC1 AUTH_OUTCOME=1 on the ACCOUNT->LOGON link, 5000 ms for LOGON's acknowledgment |
| `gate_state` | EPERM (LOGON-only kernel state) |
| `session_lock` | SESSION_QUERY + SECURE_ENTER(LOCK) (kernel grants it to LOGON only) |
| `service_link` | operations 12..14 with the frozen 64-byte record |
| `exchange_begin`, `deliver_reply`, `exchange_end` | one exchange per link, eight total; CSC1 reply frames on the original link (the kernel checks the pair generation) |
| `monotonic_ms`, `entropy`, `allocate`, `picture_valid` | CLOCK_MONOTONIC, GETENTROPY, heap, `/System/Security/Pictures` |

OWNER is `owner_interim.c` until sec-k02: every operation returns ENOSYS. With
no bootstrap evidence the open fails EIO, with evidence it fails ENOSYS at
orphan recovery, so the image's service stays closed and answers every request
with a status-only EIO reply. No owner transaction can be published.

## Boot behaviour in this image

The kernel starts the role image whenever it is present, before the desktop.
With LOGON absent (ENOENT) PID 1 still starts the interim uid-1000 desktop with
no logon screen, so the service is installed and started: the desktop, its
session and its logon behaviour are unchanged. Visible differences: one more
SYSTEM process (`/System/Services/accounts`, about 150 KB installed) in process
lists, the kernel log lines `[accounts] sealed service starting; OWNER interim
(publication refused)` and `[accounts] open refused; every request is answered
with EIO: -5`, and an idle loop of two SERVICE_ACCEPT calls every 250 ms plus
one open attempt every 30 s. Removing the payload restores the previous boot.

## Host evidence

`tests/host/security/test_account_adapter.py` links the production adapter,
serving loop, interim OWNER and native storage with the unchanged service core
and drives them through `proc_syscall` on the kernel host harness, with LOGON
and LOGON_UI as kernel-registered roles and a LOGON acknowledgment responder.
It covers the lease, retained subjects and `same_link` (two handles, different
links, forged handles and metadata, revocation, deferred retirement, descriptor
reuse), LIST over AUTH and ACCOUNTS, status-only EIO/EINVAL/EPERM replies,
session attestation, CHALLENGE and blank-password AUTH with AUTH_OUTCOME,
UNLOCK bound to the session owner, both CANCEL outcomes, peer death, one image
loop pass and an unacknowledged outcome timing out without faulting the service.
Results and exact commands: `build/codex/sec-int01-report.md`.
