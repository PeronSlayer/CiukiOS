# f6-22 — NetSurf: no network address on the T23, exit reason recorded empty

- **Phase/model:** F6, Claude agent (Opus). **Worktree:** `wt/f6-22` from `main`.
- Evidence: T23 trial of image 5a10eef4… (2026-10-22 ~13:05–13:12), serial copy `build/lead-scratch/t23-5a10eef4-serial.txt` (5,904 lines).
- Records: `[net] driver=e100 mac=00:d0:59:b6:43:8f link=up irq=11 mode=irq state=running reason=admission` and nothing else from the network stack (no DHCP, no address, no resolver record) in the whole session; `[console] [netsurf] layout slow elapsed_us=` and `[console] [netsurf] exit reason=` with empty values (the stderr line was cut at the value). Owner: NetSurf does not work well and seems to have no internet.

## Required

1. The network stack logs its state as protected records: DHCP start/offer/bound/timeout with the address, gateway and DNS (`[net] dhcp state=… ip=… gw=… dns=…`), the resolver's first answer, and a periodic `[net] stack ifaces=… ip=… rx=… tx=… errors=…` line; find why no DHCP happened on the T23 (the stack not started after admission, the e100 receive path, DHCP never sent) using the records and the host NIC model, and fix it.
2. NetSurf's stderr lines reach the log complete: the port's exit-reason and layout-slow lines must be single writes with the value (today the value is lost); the readiness page shows the network state (no address / address / DNS ok) and the exit reason is also written to `C:\SYSTEM\LOGS\NETSURF.LOG`.
3. Host tests: DHCP records on the host NIC model, the e100 receive path test if that is the cause, the stderr line atomicity, the readiness page states.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Report `build/codex/f6-22-report.md`.
