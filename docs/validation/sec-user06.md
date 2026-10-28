# sec-user-06 runtime evidence preparation

Scope: directive `docs/directives/sec-user-06.md`, contract row 9 (H14 host parser and fixture
wiring): the security runtime cases (SU-01..14 and the round-1/round-2 additions of
[user-security-contract.md 10.1](../design/user-security-contract.md#101-exact-lead-qemu-checklist-future-implementation-gate)),
the owner's account trials in acc-t23 (items 10-18), the self-test's hardware-only security
figures, the API reference and the supersession pointers. Host evidence only; no QEMU, image or
hardware result is claimed: the canonical build, the qemu-t23 acceptance and the hardware are the
lead's.

## Primary sources and resulting decisions

| Source | Finding | CiukiOS decision |
| --- | --- | --- |
| QEMU [qapi/run-state.json](https://gitlab.com/qemu-project/qemu/-/blob/master/qapi/run-state.json) (ShutdownCause) and the [QMP reference](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html) (RESET event) | `system_reset` resets the machine; the RESET event carries `guest: false` and `reason: host-qmp-system-reset`. `run.py unexpected_resets` already records that SeaBIOS adds one `guest-reset` within the same second. | The acceptance action `power_cut` is a QMP `system_reset`: it arms its own reset, counts against the case's `resets`, and accepts that one firmware follow-up within 3 s. Used by su-13-crash. |
| Microsoft, [Audit account management](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc737542(v=ws.10)) | Account-management events (624 created, 627/628 password changed/set, 630 deleted, 642 changed, 685 renamed) are audited for success and failure. | The ACCOUNT service writes one record per completed management request, success or failure: `[accounts] manage op= caller= target= type= result=` (ids and type only, never a name, picture or secret). |
| Microsoft, [WM_SYSCOMMAND](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-syscommand) | Any application may ask SC_SCREENSAVE (0xF140); with password protection the saver starts whatever the application does. | `sectest saver` asks SC_SCREENSAVE through the screen policy, as any program of the session may (`core/display.c screen_control`); the next key dismisses the saver, which locks through LOGON. |
| [RFC 8018](https://www.rfc-editor.org/rfc/rfc8018.html) 5.2 (PBKDF2) | PBKDF2 with HMAC-SHA-256, salt, iteration count, one block for a 32-byte key. | The self-test's `security.kdf` runs the account service's exact derivation (`selftest_kdf.c` = `acct_derive`); the host suite proves it equal to `acct_derive` and to Python's `hashlib.pbkdf2_hmac` (four vectors). Target: 1 s per interactive verification (contract 3.2). |
| Microsoft, [WlxScreenSaverNotify](https://learn.microsoft.com/en-us/windows/win32/api/winwlx/nf-winwlx-wlxscreensavernotify), [WlxLoggedOnSAS](https://learn.microsoft.com/en-us/windows/win32/api/winwlx/nf-winwlx-wlxloggedonsas) (as sec-user-04/sec-k07) | The secure saver locks before the desktop returns; the secure attention while logged on opens the security options. | The cases assert LOGON's lock record before the unlock and the kernel's console records; the self-test's `security.lock_latency` measures the gate through the physical Ctrl+Alt+Del transition, entered by the input driver on a T boot only (`drivers/i8042.c input_selftest_attention`, beside `secure_attention`), so the attention census keeps the input driver as the only raiser. |

## Repository comparison: findings

1. **The desktop milestone was gone on security images.** With the trusted LOGON service
   (sec-user-03) `supervisor_bootstrap` returns after spawning LOGON: neither
   `[supervisor] bootstrap: spawning /Programs/desktop` nor `[supervisor] desktop pid=` is written.
   The f0-smoke normal-boot cases and the `boot` template of every acc-t23 case wait for those
   lines, so every hardware-image gate would have failed at the desktop on the integrated image.
   Now PID 1 logs `[supervisor] bootstrap: trusted logon pid=`, `security.c` logs each published
   session program with its credential, and the runner accepts both forms.
2. **Safe Mode stops at Welcome.** LOGON never logs on automatically in Safe Mode, so the
   acc-t23 item 6 case would have waited for a desktop forever; `boot-safe` now logs on as the
   blank CiukiUser at Welcome.
3. **acc-t23 item 2 typed a path that no longer exists.** sec-user-02 (A1-5) moved `Q3ADemo.exe`
   to the protected installer store; the desktop folder holds shortcuts only. The case now runs
   `C:\Programs\Installers\Quake III Arena Demo\Q3ADemo.exe`, the Install shortcut's target.
4. **A fw_cfg request is not a user's boot.** `CBI_F_TEST_REQUEST` sends `probes_main` past the
   supervisor (no LOGON or desktop unless admitted like `f5:q3-lease-eio`) and changes console,
   entropy, autoscan, lock-order and Setup behaviour. The security cases are menu boots selected by
   the runner (lead decision requested).
5. **CiukiUser must get a password first** (A2-7): every case that creates or enables another
   account sets CiukiUser's password in User Accounts before.
6. **A T boot of a passworded installation waits at Welcome**: `desktop.start` fails after its
   90 s bound unless the owner logs on (the owner's ready image logs on automatically).

## What changed

- Kernel: `proc/supervisor.c` (trusted logon milestone), `proc/security.c` (session program and
  console enter/leave records), `proc/grants.c` (trusted-gate latency: neutral frame, first trusted
  frame, first session frame; `grants_console_timing`), `core/syscall.c` (self-test figure hand-off
  from the desktop during a T boot), `probes/selftest.c` (figure store, `registry-backup`
  request), `core/selftest_checks.c` (`security.kdf`, `security.registry_backup`,
  `security.lock_latency`), `core/selftest_kdf.c` (new), `drivers/i8042.c` (the self-test's attention entry),
  `include/ciuki/{desktop,input,selftest}.h`.
- LOGON: screen-mode, interactive refusal, lock (desktop, security menu) and unlock records;
  LOGON_UI painted-view records; the one automatic security-menu return of a T boot.
- ACCOUNT: the management audit hook (`adapter.h audit`, `service.c audit_management`,
  `kernel_adapter.c`).
- Desktop: saver start/wake records; the self-test's Registry Checker backup answer.
- `/Programs/sectest` (`apps/systools/sectest_{model,main}.c`, `apps/desktop/build_desktop.py`,
  `apps/build_apps.py`, `scripts/build_image.py`, icon `emblem-readonly`).
- Runner: LOGON milestones (`run.py`), templates `boot`/`boot-safe`/`boot-logon`/
  `boot-safe-logon`/`logon`, the `power_cut` action (`acceptance.py`), `acceptance.sh --list` and
  `--suite`; suites `acc-t23.json` (item 2 path, item 10-18 case `acc-accounts`) and
  `sec-users.json` (new).
- Host tests: H14 (`tests/host/security/test_runner_contract.py`, `sectest_test.c`), the
  self-test fakes and counts, the acceptance replays, one more set in the attention census
  (`test_secure_gate.py`: the self-test entry, its single caller) and the self-test hand-off
  in the extracted `sys_debug_write` harness (`test_system_stalls.py`).
- Documents: test architecture amendment, self-test checks, contract 10.1 wiring table,
  inventory rows SS-1..4, SD-6, PW-8, DS-3, supersession pointers in the VFS, unified DOS, Win32,
  registry, POSIX, Setup, saver and power contracts and in users-and-logon.

## Host evidence

Recorded in `build/codex/sec-user06-report.md` (commands, counts and the comparison against an
unmodified copy of the base). H14: `python3 -B -m unittest discover -s tests/host/security -p
test_runner_contract.py`.

## Lead runtime work

1. Canonical build (`make build-full`), then `scripts/test/acceptance.sh --list` and
   `--suite sec-users --list` on the image's tree.
2. `make qemu-test-full` (f0-smoke) to confirm the LOGON milestones, then
   `scripts/test/acceptance.sh` (acc-t23 with `acc-accounts`) and
   `scripts/test/acceptance.sh --suite sec-users`.
3. Hardware: the T boot's `security.kdf`, `security.registry_backup` and `security.lock_latency`
   lines; a manual Stand by gives `[security] console concealed/restored` for resume.
