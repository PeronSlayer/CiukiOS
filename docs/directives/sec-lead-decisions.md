## Lead decisions on the sec-model open questions (2026-10-26 10:50)
- Q1 approved: a configured CiukiAdministrator (500) may authenticate in Safe Mode with its password while it stays disabled in normal boot (Windows XP behaviour for the built-in Administrator); visibility alone never authorizes.
- Q2 approved: on ready images 500 stays unset (cannot authenticate) until an administrator sets a non-empty password in User Accounts; no shared recovery password is distributed.
- Q3: reconciliation with the kernel half is done in the cross-review; both halves must agree on sender identity, owner/account transactions, service-only hive paths, trusted input/secure attention, session teardown, SYSTEM gid and privilege queries.
- Q4 approved: explicit enrollment on G32 upgrades (operator chooses the first administrator, other G32 users become Standard, restartable mapping); upgraded boots stop at enrollment until done.
- Q5 approved: 8-63 printable bytes (NIST SP 800-63B minimum for user-chosen secrets), the only blank exception is ready-image CiukiUser, fail-before-write when unattended secrets are missing; no unattended secret transport in this step.
- Q6 approved: entropy-backed per-install machine SID generated on first boot before any account publication, idempotent and protected.
- Q7 approved with one change: **the volume root follows Windows XP, not the stricter R5 policy**: Standard users may create new top-level folders and files (old installers and games install into C:\GAME), become their creator-owner (full control for the creator and administrators, read/execute for other Users, Guest read-only), and may not modify or delete entries they do not own or the protected system trees. The kernel half must support creator-owned top-level entries in its owner metadata. Guest limits, the personal/machine applet split, KDF 10,000 pending Pentium III measurement and no persistent lockout are approved.

## Lead decisions on the sec-kernel open questions (2026-10-26 11:05)
- K-Q1 approved: private data under /Windows/Profiles/u%08x, SYSTEM metadata under /System/Security; existing G32 profiles are migrated explicitly by the enrollment flow (model Q4): their numeric roots are mapped to new uids >= 1000 through the owner store, never to 500/501, ids retired permanently; the shared /Documents tree becomes explicit shared data; nothing is silently assigned to the next logon.
- K-Q2: the ABI is frozen in directive sec-k01 after this reconciliation: one SYS_SECURITY call family (query self/peer, SYSTEM-only create-session/spawn-as/end-session), the profile descriptor, flags/errors/group bounds, and a read-only Win32 token/SID facade on top. Fix the stale "call 101" prose (W32_DISPLAY is under call 96).
- K-Q3 approved with a larger bound: root Creator Owner side records as in the XP root decision (model Q7), up to 256 creator-owned root entries per volume (not 32), 32 policy rules, 32 profiles; additional fixed volumes are data volumes with the same root rule, never system aliases.
- K-Q4 approved: SYSTEM-only sealed boot/service set; raw reads and writes of the managed system disk are refused to every user (administrators included) until an exact readable-sector classification exists; ScanDisk, Defrag and Setup act through SYSTEM services after an administrator check. Win32's extra all-/System write denial is replaced by the common prefix policy (one system, no silent divergence); the F5 builtin-store protections stay.
- K-Q5 approved: one w32srv per user session plus a SYSTEM machine-registry broker; never run the desktop as SYSTEM.
- K-Q6: exact Windows XP default parity is not required; differences are documented CiukiOS choices. Zero persistent lockout with the bounded 1 s retry backoff of the model part.
- K-Q7: answered by model Q2/Q5: ready images leave 500 unset; custom accounts and Guest need 8-63 byte passwords; if every administrator verifier is lost or corrupt, recovery is the Setup CD repair path (physical access is outside the threat model); no implicit empty-password recovery.
- K-Q8: shutdown/restart allowed to every interactive session including Guest and from the logon screen (physical access is outside the threat model, XP workstation default allows it at the Welcome screen); mixer/display/input preferences are per session, machine defaults are administrator-only; audit INPUT_KEYBOARD_SET forwarding and CIUKI_SETUP_NET_QUERY routing in sec-k04 and refuse unsupported paths until fixed.

## Lead decisions on the sec-sysvc2 and integration questions (2026-10-28 10:29)
- S2-1 approved: one per-user log location, `<profile>\Local Settings\Application Data\CiukiOS\Logs` (Windows XP keeps non-roaming per-user data and crash reports under Local Settings\Application Data); it supersedes sec-user-05's separate NetSurf and Crash folders. Profile-less sessions keep the machine folders.
- S2-2 approved: the startup ScanDisk report is a kernel SYSTEM job (call-99 JOB) and the daily registry backup a job of the SYSTEM registry broker; both run as SYSTEM, which is the requirement. No common job queue for now.
- S2-3 approved: Registry Checker restore and damaged-hive repair apply at the broker's next start, before anything loads the registry, like Windows 98 SCANREG /RESTORE (restart) and XP System Restore.
- S2-4: backups hold the machine registry, BOOT.CFG, HWPROF.CFG and the settings files; per-user hives stay out for this step. Including them (XP restore points snapshot user hives too) goes to sec-k08's audit with the 4 MiB archive limit.
- S2-5: `clock.cfg` is read at boot and must be sealed like the other boot settings; sec-k08 adds it behind call-99 SETTING.
- S2-6: the synchronous backup against the desktop's 5 s request timeout is measured on the Pentium III in the self-test boot.
- I-1 (integration): the Setup CD carries the machine registry hives that sec-k06 keeps under /System/Security/Registry/Machine (machine configuration Setup installs, like the default hives of a Windows Setup medium); the account seed and bootstrap.state stay excluded (sec-user-02).
- I-2 (integration, sec-integfix): an owned drive letter without media reports ERROR_NOT_READY (ENXIO) before the sec-k03 subject-aware walk; unknown letters remain ENOENT.

## Lead decisions on the sec-user-04 questions (2026-10-28 10:41)
- U4-1 approved: the G32 upgrade enrollment amendment before section 7 of `user-security-contract.md`, with its private LOGON/ACCOUNT operation numbers and settings request 11.
- U4-2 approved: an administrator's log-off of another user's locked session is forced, as Windows XP's "Log Off" of a disconnected user (unsaved data is lost and the dialog says so).
- U4-3 approved for this step: the desktop raises the lock before Stand by; a firmware-initiated suspend locks when the desktop sees the resume. sec-k08 checks whether a kernel resume hook is needed for the APM/ACPI paths the hardware baseline uses.
- U4-4: account pictures. Images ship only the default Ciuki portrait for now; a picture set derived from the approved portrait and Tango is a later UI item, never a substitute portrait.
- U4-5: a Standard user cannot rename their own account (Windows XP limited accounts: change password and picture only); the ACCOUNT service must refuse it, not only the page. sec-k08 fixes it.
- U4-6: machine-wide power settings (hibernation, the power button action) belong to the machine and are written through the SYSTEM service; per-user schemes stay per user. sec-k08 moves them.
- U4-7 approved: per-user Desktop and Start Menu in a LOGON session, merged with All Users (the XP union already decided for sec-user-05), superseding sec-user-03's decision 6.

## Lead decisions on the sec-k08 questions (2026-10-28 15:20)
- K8-1 (S2-4): per-user hives stay out of Registry Checker backups, as in Windows 98, where SCANREG backs up the machine registry and the system INI files.
- K8-2 (U4-6): the standby and hard-disk timers are per-user scheme settings again, editable by every user, as Windows XP power schemes are. The sealed machine `power.cfg` keeps only the power-button and lid actions.
- K8-3 (U4-3): confirmed. A firmware-initiated suspend locks every session whose owner has a password, whatever the per-user resume preference. The 2 s window after a resume, in which Ctrl+Alt+Delete may lock instead of opening the menu, is accepted.
- K8-4: `security.kdf` reports min/median/max and prints WARN above a 1,000 ms median; it never fails the self-test, because the figure depends on the CPU.
- K8-5: images ship without `power.cfg`; the defaults (button: Shut down, lid: nothing) apply.
- K8-6: every future directive that adds a syscall, call-99 action, channel operation, device command or policy kind adds its census row in `tests/host/security/policy_census.py`. The census test enforces it.
