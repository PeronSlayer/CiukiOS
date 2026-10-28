# sec-wave4: OWNER binding, trusted gate, application identity; then logon, User Accounts, audit and evidence

- **Written:** 2026-10-27 19:00 by the lead. Base: branch `wt/sec-wave3` (main 225710f20+ with sec-k03, sec-k04, sec-k05, sec-k06 and sec-user-02 merged).
- **Owner decisions 2026-10-27:** the whole user-security programme is completed now; the next hardware image waits for it.
- **Binding specification:** `docs/design/user-security-contract.md`, `docs/design/user-security-kernel.md` ("Implementation plan" rows 5, 6A, 6B, 7, 8, 9 and the sections they cite), `docs/directives/sec-lead-decisions.md`, and the wave-3 reports in the worktrees `../CiukiOS-wt-sec-k03`, `-sec-k04`, `-sec-k05`, `-sec-k06`, `-sec-user02` (`build/codex/<step>-report.md`), plus `../CiukiOS-wt-sec-wire/build/codex/sec-wire-report.md`.

## First round (parallel)

| Step | Scope |
| --- | --- |
| **sec-own: OWNER binding and system-volume publication** | Bind the system volume's policy and OWNER lease at boot (sec-k03's integration step); one volume ID shared by the image builder, Setup, the account service and the owner service (partition start sector << 32 \| FAT serial, as sec-user-02 reports); the kernel opens and binds its owner store at boot; the six missing owner operations sec-wire listed; the user-space owner adapter that replaces the interim one; first boot turns sec-user-02's seed into the account database and the service publishes accounts (no more "every request answered with EIO"). Also: k05's `vfs_table_subject` now exists (k03) — bind DOS VM tables to the VM subject; old `C:\SYSTEM\DOS\SAVES` content migrates to the first administrator's profile (user-02 migration step). |
| **sec-k07: trusted gate and kernel teardown integration** (row 5) | Kernel secure attention, input/display/grants and session drain; real-backend integration of k01's launch and secure-console operations; `dosvm_session_revoke` and Win32 session teardown at logoff. |
| **sec-user-05: application-facing identity** (row 6B) | advapi32/shell token, SID, user name, CSIDL and privilege facades, native account lookup, authorized PROFILE/libc adapters, DOS launcher adapters; **per-user joystick and mixer preferences** (sec-k04 left interactive CALIBRATE/ROUTE/CONFIGURE/SAVE_GAME and session mixer saves at EOPNOTSUPP); printing for Standard users through the spooler (sec-k05 open item). |

## Second round (sequential, after the first round merges)

sec-user-03 (row 6A: authenticated logon and session UX, Welcome/typed names, Safe Mode 500 exception, Log Off), then sec-user-04 (row 7: User Accounts in Control Panel, saver and resume), then sec-k08 (row 8: coverage and bounded-cost audit) and sec-user-06 (row 9: runtime evidence and acceptance cases in `tests/suites/acc-t23.json` for logon, Guest refusal, CiukiAdministrator in Safe Mode, standard-user games/saves).

## Common acceptance

The contract rows' criteria and host evidence; the administrator desktop, games (DOS and Win32), installers (Quake III to its exit, InstallShield 5) and Setup keep working; the kernel, security, desktop, Win32, DOS and Setup suites stay green; new `abi.h` records get `abi_layout` rows; constant kmalloc sizes above 2040 bytes do not compile. Shared tables/headers/test scripts: one commented block per step, listed in the report.

Report to `build/codex/<step>-report.md` in your worktree. Leave changes uncommitted.
