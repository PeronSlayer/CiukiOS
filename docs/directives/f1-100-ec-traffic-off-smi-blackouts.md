# f1-100 — ThinkPad EC traffic causes the 2.1 ms SMI blackouts: no periodic EC I/O by default

- **Phase/model:** F1, Claude agent (Opus). **Worktree:** `wt/f1-100` from `main`.
- Evidence: T23 trial of image 5a10eef4… (2026-10-22 ~13:05–13:12), serial copy `build/lead-scratch/t23-5a10eef4-serial.txt` (5,904 lines).
- Records: `[kernel] smi_config chip=ICH3-M … mon_smi=1000 …` (one I/O-trap monitor armed by the firmware), `smi count=0 … unproven=1 gaps=8 rate=3.98/s`, `smi_coincide gaps=8 hid_near=0 scan_near=0 ec_near=8 … ec_io=36 ec_poll=1000`, and at the end `gaps=4 … ec_near=4 … ec_io=18 … ec_lead_us=15/15`: every blackout coincides with an EC port access (lead 15 µs), even with the poll at 1 Hz (`[ec] thinkpad poll_ms=1000 source=default`); SMI_STS shows no bit (the trap monitor's status is elsewhere). Conclusion: the firmware traps the EC ports (0x62/0x66) into SMM, about 2.1 ms per transaction, and our EC traffic (poll transactions of several port accesses, plus volume reads) is the source. `irqoff_long` 83, `violations=373`.

## Required

1. No periodic EC I/O by default: the volume poll is off (`ecpoll=0` becomes the default; `ecpoll=1` re-enables the 1 Hz poll for diagnosis, both tokens accepted by the loader within the budget). The boot probe keeps one transaction; a volume change from the desktop keeps its write; the tray's hardware-button mirror is updated only by an explicit user action (opening the volume popup) — document the limitation in `docs/design/f4-18-audio-levels.md`.
2. Every remaining EC transaction is counted and bounded (`[ec] thinkpad io=<n> transactions=<n>` per window) so the T23 serial shows the residual rate; the `smi_coincide` record keeps `ec_near`.
3. Research and document (ICH3-M datasheet: DEVTRAP_EN/MON_SMI I/O trap monitors; ThinkPad T23 BIOS behaviour) why the status bit is not visible in SMI_STS and whether reading MON_SMI's status would attribute it; do not disable the firmware's monitor (R1).
4. Host tests: default off, token parsing, transaction counting, the coincidence record; `test_kernel_latency`, `host_kernel_tests.sh`, loader tests with the budget.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Report `build/codex/f1-100-report.md`.

## Amendment 1 (lead, 2026-10-22 14:30, decisions on the delivery)

Accepted. The delivery corrects the directive's reading: no I/O trap is
armed; the EC's event line GPI[12] is routed to SMI# (`gpe1_en=1000`,
`gpi_rout=01000000`, SCI_EN=0) and the EC raises it for each byte, two SMIs
per read, with the firmware clearing GPE1_STS inside SMM. Decisions: the
new devctl `MIXER_REFRESH_MASTER` (4204h, delegated handle) is reserved; a
level-only change makes no EC access and a hardware mute persists until the
popup is opened or Mute is clicked (documented in f4-18). Follow-up: the
pre-existing `desktop.test_memory_footprint` failures (wallpaper_boot_test.h:145,
sound_runtime_test.c:130) are directive f2-93.
