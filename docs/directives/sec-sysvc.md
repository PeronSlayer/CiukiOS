# sec-sysvc: SYSTEM services for administrator operations, logs, first-boot uid recovery

- **Written:** 2026-10-27 23:00 by the lead. Implementer: Claude agent. Base: branch `wt/sec-own` (wave 3 + sec-own).
- **Evidence:** sec-own report (`build/codex/sec-own-report.md` in `../CiukiOS-wt-sec-own`): with the system volume policy bound, the contract's rules (K-Q4: the system trees are SYSTEM-only; administrators act through SYSTEM services after an administrator check) now refuse, for the administrator desktop: Control Panel boot-option saves, driver-pack install/uninstall, reading `C:\System\Logs` (Log Viewer, Control Panel Logs tab, hwreport), NetSurf and crash-log writes there.

## Scope

1. **SYSTEM services** for each refused administrator operation, as the contract prescribes (administrator check on the caller's retained subject, then the service acts as SYSTEM): boot options (BOOT.CFG and the startup menu choices), driver-pack install/uninstall, and whatever else in Control Panel, Device Manager, Setup repair or ScanDisk/Defrag writes under `/System` (inventory them all, from the code, and list them).
2. **Logs:** follow Windows XP: administrators may read the system logs (Event Viewer model); writers that are not SYSTEM (NetSurf, crash reports of user programs) write per-user logs under the profile (or through a logging service), and the Log Viewer shows both. Research and cite the Windows XP defaults.
3. **First-boot uid recovery (lead decision):** a uid whose only OWNER reservations were aborted before any batch committed may be reserved again, so a power cut between OWNER's reservation and the account service's intent write never leaves a fresh install closed. Every other retired uid stays retired. Host regression with a power cut at that write.
4. Tests: per-service host tests with administrator, standard user and Guest; the Control Panel, Device Manager, log viewer, NetSurf and security suites stay green.

Rules: work only in `/home/peronslayer/Desktop/CiukiOS-wt-sec-sysvc` (branch `wt/sec-sysvc`); never commit, merge or push; never run QEMU or image builds; run tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`. Report to `build/codex/sec-sysvc-report.md`. Leave changes uncommitted.
