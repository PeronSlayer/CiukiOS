# sec-k08: coverage and bounded-cost audit of the security programme

Directive: `docs/directives/sec-k08.md` (2026-10-28). Base: `wt/sec-integ5` (main plus the
whole security programme through sec-sysvc2 and sec-user-04). Binding: `user-security-contract.md`
implementation plan row 8, `user-security-kernel.md` (bounded cost, inventory, and its
"Coverage audit follow-ups (sec-k08 amendment)"), `sec-lead-decisions.md` S2-4, S2-5, U4-3,
U4-5, U4-6. Host evidence only: no QEMU, image or hardware result is claimed here.

## 1. Census (H13)

`tests/host/security/policy_census.py` derives every published selector from the sources (never
from its own table) and `test_policy_coverage.py` requires exactly one policy class and one
covering test for each; a selector without a mapping, a mapping whose selector disappeared, or a
covering test that no longer names its marker fails the suite (`test_a_new_selector_without_mapping_fails`
proves the three failure modes).

| Family | Source of truth | Selectors |
| --- | --- | --- |
| Native system calls | `abi.h` `enum ciuki_syscall` and `CIUKI_SYS_W32` | 98 |
| Call-101 operations | the `security_syscall` switch | 14 |
| Call-99 services | `sysvc_syscall` cases and the `op ==` tests of `recovery_syscall_subject` | 11 |
| Call-99 sub-actions | BOOT_CONFIG, LOG actions/scopes, DRIVER_PACK actions, JOB ids, SETTING ids | 16 |
| Channel services/operations | `channel.c` `max_op` (AUTH 8, ACCOUNTS 9, OWNER 14, LOGON 10), the registry protocol's `RB_*`, W32SRV | 60 |
| Device commands (call 69) | the command constants of `abi.h`/`input_abi.h` (device prefixes and value ranges) | 79 |
| VFS access bits | `enum policy_access` | 8 |
| VFS policy kinds | `enum policy_kind` | 12 |
| **Total** | | **298** |

Unknown selectors deny. Statically (`check_defaults`): the native default routes file, desktop fd,
process and `proc_syscall` ends in `-ENOSYS`; call 101 checks 1..14 and services 1..6; unknown
call-99 ops reach `EINVAL`; SETTING, JOB and LOG reject unknown values; `channel.c` refuses
operation 0 and anything above `max_op` with `EOPNOTSUPP`, and the kernel bounds agree with the
servers' own (ACCOUNT `protocol.c` 8/9, `LGN_OP_ENROLL_SOURCE` 10, the OWNER query table 14); the
device dispatcher ends in `ENOTTY`/`EBADF`; the registry broker answers `EOPNOTSUPP` for unknown
native and Win32-stream operations; `policy_check` refuses unknown access bits and, since this
step, unknown label kinds. At run time H13 calls each of these with unknown values (below).

**Gap found and fixed:** `policy_check` allowed READ of a label whose kind is outside
`enum policy_kind` to any non-administrator (the switch fell through to the read rule). Labels are
kernel-built, so only corrupt metadata produces one; it now denies every subject, SYSTEM
included (`fs/security_policy.c`).

## 2. Adversarial harness H13 (`tests/host/security/test_policy_coverage.py`)

Each frontend runs on its existing production fixture with an H13 block compiled in by
`-DCIUKI_POLICY_COVERAGE`; every attempt is made once (warm) and three more times, and the
second snapshot must equal the first; then the fixture's own teardown requires zero of each
resource.

| Frontend (fixture) | Attempts by a Standard user and Guest | Refusals | Leak counters compared |
| --- | --- | --- | --- |
| Native: syscalls, call 99, call 101 on FAT/VFS/policy/OWNER (H16) | SECRET files/folders (accounts, logs, pagefile, hives, SysBckup, Registry); every LOGON-only call-101 operation; SYSTEM and Bob's process (PROC_TERMINATE) and token (QUERY_PROCESS); every administrator call-99 service, SETTING CLOCK/POWER, MACHINE log; Bob's profile file/hive/registry folder/crash log/home (open, write, stat, FILE_INFO get/set, create, mkdir, unlink, rename, chdir); the sealed set (VMM.ELF, BOOT.CFG, CIUKILDR, Services, Drivers, Programs/setup, SFC.LST, HWPROF.CFG, clock.cfg, power.cfg, performance.cfg: write, truncate, unlink, rename, metadata, plant, replace a folder); unknown syscall numbers (6-15, 92-95, 102+, 0xffffffff), call-101 operations, call-99 services, SETTING/JOB/LOG values, device commands; all twelve label kinds against five subjects (expected from the rule table), the root's creator-owned ADD (Q7); public device queries and their administrative neighbours | 444 | fds, open descriptions, pins, namespace nodes, pages, heap blocks, subject and session references |
| Call-101 channels and the registry broker (H07 adapter) | CONNECT to AUTH, OWNER, LOGON (EPERM), services 0/7/0xffffffff (EINVAL), W32SRV of another session (ESRCH); ACCOUNTS operations 0, 10, 11, 0x7fffffff, 0xffffffff (kernel EOPNOTSUPP); a cross-service frame; RB_NATIVE_BEGIN/PART/COMMIT on System, Programs and Users (EACCES before staging); nine unknown broker operations and operation 0 | 64 | fds, channels, descriptions, messages, pages, heap, subject references; no write reaches storage |
| DOS VM (H06) | Alice's profile file, hive and crash log (open, attributes, delete), her profile (chdir, rmdir, create, mkdir), accounts, system log, machine hive, Security folder, the sealed set on D: (delete, create, plant, rmdir) | 46 | VM holds, IFS opens, descriptions, pins, pages, heap; teardown returns holds and the subject |
| Win32 personality (H07 kernel) | Bob's (another session) and a SYSTEM process with six access masks; post/send to Bob's thread; APC and suspension of Bob's and a SYSTEM thread; Bob's named event (own session namespace instead) | 36 | Win32 objects, handles, sections, views, messages, APCs, endpoints, pages, heap |
| ACCOUNT service (H03) | create, delete, enable, type, rename, password reset and picture of other accounts; own type, name and status; another PROFILE; AUTH, UNLOCK, AUTOLOGON, ENROLL_VERIFY from an ordinary endpoint | 98 | database generation, allocations, peer references (no store write) |

The LOGON service's operation routes (role per operation, same-session DESKTOP) are covered by
H10/H12 (`logon_service_test.c`, `account_controls_test.inc`) and mapped in the census.

## 3. Bounded cost

Printed by H13 (`H13 cost` lines); host figures (clang -O1 with ASan/UBSan), so only their ratios
and the zero counts are evidence; the Pentium III figures are the lead's runtime gate.

| Measure | Result |
| --- | --- |
| Warm descriptor checks | 2,000 `pread`/`pwrite` on an open file each by a Standard user and an administrator: **0 disk reads, 0 OWNER store reads** (the label check is O(1) on the retained description) |
| OWNER store at the bounds | 54,864 bytes: 32 profiles + 256 creator-owned roots + 32 pending records of 144 bytes; receipts at most 1,024 x 32 bytes, compacted at 768 |
| Policy table | 2,224 bytes per volume: 32 rules (30 installed by `policy_init`, `policy_rule_add` refuses the 33rd with `ENOSPC`) |
| Per namespace node | one 40-byte `policy_label` inside the 1,040-byte `px_node` |
| Cold resolution at the bounds | `policy_child` scans at most 32 rules and 288 owner records: creator root at record 288 about 1.2 us, profile 32 about 0.7 us; warm `policy_check` about 12-18 ns (host) |
| Identity | credential 64 bytes (8 groups), subject 128 bytes, session record 584 bytes x 4; process limit 64, thread limit 256 |
| Group/session/process limits | 9 groups and malformed group sets `EINVAL`, a second interactive session `EBUSY`, exhausted session ids `EAGAIN` (H01 `test_security_templates`); the 65th process `EAGAIN` with no page/heap growth (`proc_test.c` `test_limits`) |

**Every final-reference failure** and its containment test:

| Final reference | Failure at the last reference | Test |
| --- | --- | --- |
| Subject (process exit, peer close) | session already DEAD: the last subject reference frees it | H01 teardown (zero subjects/channels/heap) |
| Session END/COLLECT | fence cannot complete (DOS full screen, present lease) | H09 `test_fences`; CLOSING blocks the next logon (`secure_gate_test.c`) |
| Service channel | peer revoked with an admitted or queued exchange | H01 cancellation, H13 channels (zero channels/messages) |
| File description | the last close's flush or FSINFO write fails | `files_test.c` (fail_flush/fail_write), H16/H13 ledger (zero descriptions/pins) |
| DOS VM | logoff while handles are held | H06 revocation, H13 dos (holds back to zero) |
| Win32 session objects/tokens | session revoked while bound objects exist | H07/H09 `w32_session_revoke` |
| Registry broker link | peer dies or stalls mid-stream | H07 adapter (`ESRCH`, stage expiry), H13 channels |
| OWNER receipts | power cut at every checkpoint/compaction marker | H02/H03 power-cut sweeps |
| ACCOUNT peers | cancellation or failure after retain | H03 `fake_destroy` (no peer reference), H13 accounts |
| SYSTEM service tables | any `sysvc_*` error path | H16 teardown ledger |

**KDF.** The account verifier's PBKDF2-HMAC-SHA-256 at 10,000 iterations stays the lead's Pentium
III runtime gate. The self-test boot now has `security.kdf` (`core/selftest_checks.c`, 15 s bound):
five derivations of a fixed non-secret input while the desktop runs, figures
`iterations runs min_ms median_ms max_ms target_ms=1000`, FAIL over the 1,000 ms target of the
kernel contract. The derivation (`src/kernel/lib/pbkdf2.c`) equals the ACCOUNT service's
`acct_derive` byte for byte and the published PBKDF2-HMAC-SHA-256 vectors (H13
`test_registry_checker_and_kdf_figures`).

## 4. Lead follow-ups

### S2-5: `clock.cfg` sealed behind SETTING

`clock.cfg` is SEALED by name in `C:\SYSTEM\SETTINGS` (`policy_child` boot inputs): every direct
writer, administrators and the interim session included, is refused; everyone reads it. The time
zone service (call 97, administrators) keeps writing it as SYSTEM; SETTING CLOCK
(`CIUKI_SYSTEM_SETTING_CLOCK` 4) accepts exactly what `tz_load` accepts (a complete
`tzr_config_parse` record selecting a rule, at most 511 bytes) for administrators, the interim
session and SYSTEM, and restores the old text after a failed write. Setup keeps writing the
destination's `clock.cfg` on its unmanaged volume (`apps/control/configuration.c`). Evidence: H16
`test_k08_settings`, H02 boot-input rows.

### S2-4: per-user hives in Registry Checker backups

Measured with the production w32srv registry (`policy_cost_test.c`): machine hive 11,248 bytes,
default user template 4,569 bytes, a new profile's hive 6,777 bytes; one hive may hold up to
`W32R_TEXT` = 4,194,304 bytes of text plus its header; the archive bound is
`RC_MAX_BYTES` = 4,194,304 bytes and `RC_MAX_FILES` = 32 entries.

- Bytes: 32 typical profile hives (about 217 KB) fit beside the machine set; a single hive at its
  bound does not fit the archive at all.
- Entries: 32 profiles take all 32 entries, leaving none for the machine roots, hive slot,
  BOOT.CFG, HWPROF.CFG and the settings files.

So the per-user hives cannot be included in the machine archive for every allowed state, and they
are not included in this step. Windows 98 Registry Checker backs up `User.dat` and `System.dat`,
`Win.ini` and `System.ini` (Windows 98 Resource Kit, "Using Registry Checker", local extract
`build/rskt/wrkc27_2.htm`), not the profiles' `USER.DAT`; the lead's S2-4 note states that XP
restore points also snapshot user hives. Options for the lead are in the report.

### U4-5: no rename of one's own account by a Standard user or Guest

The ACCOUNT service (`apps/accounts/service.c` `mutate`) now treats only the password and the
picture as personal: a non-administrator's RENAME and every UPDATE carrying the name selector are
`EPERM` before any validation or store write, as Windows XP limited accounts change only their
password and picture. Administrators rename other accounts and their own. H03 `service` and H13
accounts; the H12 Guest case now expects `EPERM` instead of the built-in name's `EINVAL`.

### U4-6: machine power settings

- Windows XP keeps the power-button and lid actions in `GLOBAL_USER_POWER_POLICY` ("Each user has
  a separate global power scheme", [WriteGlobalPwrPolicy](https://learn.microsoft.com/en-us/windows/win32/api/powrprof/nf-powrprof-writeglobalpwrpolicy),
  [GLOBAL_USER_POWER_POLICY](https://learn.microsoft.com/en-us/windows/win32/api/powrprof/ns-powrprof-global_user_power_policy));
  only lid-open wake and capacity resolution are machine-wide
  ([GLOBAL_MACHINE_POWER_POLICY](https://learn.microsoft.com/en-us/windows/desktop/api/powrprof/ns-powrprof-global_machine_power_policy)).
  The lead's U4-6 makes the buttons machine settings: a documented CiukiOS choice (K-Q6), matching
  the contract's Power row ("button/lid defaults" are machine policy).
- CiukiOS has no hibernation (S4), so there is no hibernation setting to move.
- Implementation: `C:\SYSTEM\SETTINGS\power.cfg` (sealed), lines `power_button=` and `power_lid=`;
  SETTING POWER (`CIUKI_SYSTEM_SETTING_POWER` 5) for administrators and SYSTEM. In a LOGON session
  the desktop reads it when its settings are (re)loaded and applies it over the user's own lines
  (`power_policy.c` `pp_machine_*`, `desktop.c`, `shell.c`); Control Panel's Power page shows the
  machine's values and saves them through the service (Standard users see them disabled and the
  service refuses a forced change). Schemes, alarms, the meter and the resume preference stay in
  the user's own `desktop.cfg`; the interim G32 desktop keeps its `desktop.cfg` lines. Evidence:
  H16, `control_test.c`, `power_policy_test.c`, `desktop_test.c`.

### U4-3: a firmware-initiated resume and the LOGON lock

Paths on the hardware baseline:

1. **ACPI.** OSPM alone enters a sleeping state, by programming `SLP_TYPx` and setting `SLP_EN`
   ([ACPI 6.4, 16.1 Sleeping States](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/16_Waking_and_Sleeping/sleeping-states.html);
   the same OSPM-directed mechanism as ACPI 1.0b/2.0); the sleep button is an event to OSPM.
   CiukiOS enters no ACPI sleeping state (`power-policy.md` item 21: no wake trampoline). No hook.
2. **APM 1.2 standby/suspend requests** (0001h/0002h/0009h/000Ah): the kernel holds them with
   "processing" answers until the desktop's call-73 Stand by or cancel (`power-policy.md` G11B),
   and the desktop arms password-on-resume first. No hook.
3. **APM 1.2 Critical System Suspend** (0008h): the OS must answer Set Power State Suspend within
   the BIOS's 5 s (APM 1.2 4.3.9, cited in `acpi-events-power.md`); CiukiOS's APM worker flushes
   and acknowledges it itself, so the desktop never runs its lock. A Critical Resume (0004h) and a
   resume notification without a preceding call-73 Stand by mean the same. **A kernel hook is
   needed.**

Implemented: `core/power_source.c` raises the trusted gate (PID 1, `security_console_attention`,
the same deferred path as a physical Ctrl+Alt+Delete) at 0008h, before the acknowledgment, and at
0004h or an unsolicited 0003h/000Bh; the echo of a call-73 Stand by is consumed without a lock.
LOGON's watch (`apps/logon/coordinator.c`) sees the moved gate and the call-73 resume/critical
counts within 2 s and locks an owner with a password (`SECURE_ENTER` RESUME, as Windows XP prompts
on resume by default) or restores a passwordless owner (no false lock, contract 4.3). The
per-user resume preference is the desktop's and cannot be honoured for a suspend the desktop did
not see: such a suspend locks every owner with a password. Evidence: `apm_platform_test.c` (no
lock for the Stand by echo, lock for 0003h/0004h and at 0008h before the acknowledgment), H12
`saver` and `saver-blank` scenarios.

## 5. Changed shared tables, headers and test scripts (one sec-k08 block each)

`abi.h` (SETTING CLOCK/POWER), `security.h` (`security_attention_resume`), `sysvc.h`,
`security_policy.c` (sealed names, unknown kind), `power_policy.h`, `shell.h`, `control.h`,
`logon.h`, `selftest_checks.c` (check table), `abi_layout_test.c`, `system_services_test.c`,
`registry_adapter_test.c`, `dos_identity_test.c`, `win32_isolation_test.inc`, `accounts_test.c`,
`account_controls_test.inc`, `logon_service_test.c`, `owner_policy_test.c`, `apm_platform_test.c`,
`control_test.c`, `desktop_test.c`, `power_policy_test.c`, `selftest_checks_test.c`,
`test_selftest.py`; `docs/developer-api.md` regenerated.
