# f4-27 — ThinkPad EC works at boot on the T23 then disables itself on one timeout; the master must fall back to software

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f4-27` from `main`. Host-testable; the lead tests on the T23.

## Evidence (T23, image 66843edc, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/66843edc/disk-log/BOOT.LOG.clean`)

```
[smbios] entry=ok address=000f7350 table=1ff7c000 bytes=1717
[smbios] version=2.31 vendor="IBM" product="26473RG" version="Not Available" type1=1
[smbios] bios vendor="IBM" version="1AET52WW (1.09 )" type0=1
[ec] thinkpad hint=1
[ec] thinkpad volume=3 mute=0
[ec] thinkpad volume=3 mute=0
[ec] thinkpad disabled error=-110
```

f4-25 works: real strings, EC handshake, level 3. Then one transaction timed out (-110, the 20 ms deadline) and the driver disabled itself for the rest of the boot: keys dead, and `thinkpad_ec_master_set` returns `-ENODEV` while `audio.master_in_hardware` stays set (`audio_dev.c:101`), so every master change from the Control Panel fails. Real ECs are busy for longer than 20 ms during their own activity (fan, battery, keyboard events); Linux `drivers/acpi/ec.c` waits up to 500 ms per transaction and retries.

## Task

1. Transaction timeout 500 ms with the IBF/OBF handshake per ACPI §12.2, bounded by the task budget (sleep between polls, no spin); on timeout the poll is skipped and retried at the next 250 ms tick; the driver disables itself only after 8 consecutive failures and re-probes every 10 s.
2. While the EC is unavailable, `audio_master_set` applies the software/codec master and saves the preference (no error to the user); when the EC returns, its level is resynchronised from the saved preference.
3. Records: `[ec] thinkpad timeout stage=<ibf|obf|read|write> ms=<n>` rate-limited, `disabled` only at the 8th failure, `resumed` on re-probe.
4. Host tests: EC model with sporadic 100 ms busy windows, one that dies, master set during both.

## Acceptance

Audio host suites green; on the T23 the keys keep working for the whole session and the Control Panel saves the volume. Files: `thinkpad_ec.c`, `audio_dev.c` master path, tests.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Record links and decisions in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f4-27-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify.
