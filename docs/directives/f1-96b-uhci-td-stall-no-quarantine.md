# f1-96b — UHCI: a stalled TD mid-transfer ends in reset recovery and quarantine (stick disappears)

- **Phase/model:** F1, Claude agent (Opus). **Worktree:** `wt/f1-96b` from `main`.
- Evidence: T23 trial of image 5a10eef4… (2026-10-22 ~13:05–13:12), serial copy `build/lead-scratch/t23-5a10eef4-serial.txt` (5,904 lines).
- Records: during a 64 KiB bulk-in read (f1-96) `[usb-storage] td addr=2 tag=155 … expected=64 actual=-1 td=388007ff token=07e88269 hc_status=00000000 result=-110`, `timing … frames=45 elapsed_ms=44 retired=0`, `bulk … packets=273 index=272 offset=17408 stop=packet` (the last packet of the run never retired: the controller stopped advancing with no error status), then `csw available=0`, `quarantine addr=2 reason=reset_recovery stage=data`, port disabled, `[storage] completion drive=E … result=-5`: the stick is gone from the desktop. f1-96's report already named the early Intel UHCI queue-advance erratum that Linux works around.

## Required

1. Implement the Linux uhci-hcd workaround for Intel controllers that fail to advance a queue (uhci-q.c: the "depth/breadth" TD linking and the QH element re-write / FSBR handling; cite the exact code and commit), so a long TD chain across frames completes on the ICH3-M.
2. Recovery policy: a timed-out TD in the data stage is retried once with the same chain, then the transfer is retried with the fallback size (128 → 8 → 1 sectors) with a record; reset recovery and quarantine only after the fallback at 1 sector fails or the device no longer answers; a quarantined stick is re-enumerated when re-plugged (today the port stays `reason=quarantined`).
3. Host tests on the UHCI model: a controller that stops advancing at packet n completes with the workaround; the fallback ladder; the quarantine only at the end; re-plug after quarantine.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Report `build/codex/f1-96b-report.md`.

## Amendment 1 (lead, 2026-10-22 14:28, decision on the delivery)

Accepted. The cause was our f1-96 ring walk keeping a stale TD status after
the controller had drained the ring between two driver wake-ups (reproduced
on the host at a 9-frame wake), not the Intel erratum; the erratum
workaround stays as a defensive measure. Follow-ups: USB port reset before
quarantine when the BOT reset fails, re-qualification up to 128 sectors
after successes, cancelled mid-transfer requests, drive-letter reuse on
re-plug.
