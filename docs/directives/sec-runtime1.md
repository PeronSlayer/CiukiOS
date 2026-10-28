# sec-runtime1: runtime defects of the first security image on QEMU

- **Written:** 2026-10-28 13:28 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`. Base: main after the lead's session-start fixes (commit named in the queue entry).
- **Evidence:** the lead's interactive QEMU boots of main with the whole security programme. Serial log of image 7123a68a…: `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/sec-runtime1/serial-7123a68a.log`.
- **Lead fixes already on main:**
  - a concealed session may query its own display geometry and acceleration info (`src/kernel/proc/grants.c` `grant_query_check`);
  - ACCOUNTS PROFILE with generation 0 reads the current mapping (`apps/accounts/service.c`);
  - w32srv logs the failing step of a session start (`apps/w32srv/security_io.c`).

  With these fixes autologon reaches the CiukiUser desktop in 1.2 s.
- **Class:** host fakes passed while the real processes disagreed on a protocol detail. The first three defects were invisible to every host suite.

## Defects and scope

1. **StartUp never runs in a LOGON session.** The serial log shows `startup: snapshot failed: 32`. `apps/desktop/startup.c` waits up to 10 s for `desk_w32_ready()` (`W32R_INFO` on HKLM through `apps/desktop/win32_registry.c`), which never succeeds in a LOGON session whose w32srv was launched by LOGON (`LAUNCH_W32SRV`) in session-only mode.
   - Find why: route, HKLM through the SYSTEM broker, or readiness semantics. Fix it, then audit every desktop→w32srv registry and shell-launch path in a LOGON session: StartUp folder, Run/RunOnce keys, file associations, shell launch of `.exe`, Control Panel pages that read HKLM/HKCU.
2. **A blank-password account asks for a password.** Clicking CiukiUser (no password) on the Welcome screen shows "Type your password". Windows XP's Welcome screen logs an account without a password on directly when it is clicked. Cite the XP documentation and follow it.
3. **Protocol conformance tests between the real peers.** For every request the session processes send to the SYSTEM services, add host tests that run the production client encoder against the production server decoder and handler, with no fakes in between:
   - w32srv → ACCOUNTS;
   - desktop → w32srv;
   - libc/PROFILE → ACCOUNTS;
   - LOGON → ACCOUNTS, REGISTRY broker, OWNER;
   - Control Panel → ACCOUNTS.

   Cover administrator, Standard user and Guest, with generation 0 and a current generation. This closes the class of the three defects above.
4. **QEMU checks for the lead.** Write the serial records the lead should see on a good boot:
   - `[logon] session 1 running`;
   - the StartUp snapshot success line;
   - the first Win32 program launched from the desktop.

   Add them as predicates to the acceptance case for first boot to the desktop in `tests/suites/acc-t23.json`. Coordinate with sec-user-06 (in progress) only through your report.

## Acceptance

- The new conformance tests pass.
- The kernel (`bash scripts/test/host_kernel_tests.sh`), security, desktop (`python3 -B -m unittest discover -s tests/host/desktop -t .`) and Win32 suites gain no failure against the base.
- The lead verifies on QEMU.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-sec-runtime1` (branch `wt/sec-runtime1`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Report to `build/codex/sec-runtime1-report.md`. Leave the changes uncommitted.
