# f4-21 — ThinkPad EC volume driver silent on the T23: log the SMBIOS identity and open the gate

- **Model/effort:** `gpt-6.1-sol`, `high`, `--search`. **Worktree:** `../CiukiOS-wt-f4-21` from `main`. Host-testable; the lead tests on the T23.

## Evidence (T23, image 445e91c7, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/445e91c7/disk-log/BOOT.LOG.clean`)

No `[ec]` record at all. `src/kernel/drivers/thinkpad_ec.c` (f4-18) activates only when the SMBIOS type-1 strings satisfy `vendor == "IBM" && (product starts with "ThinkPad" || version starts with "ThinkPad")` and logs nothing when the gate fails. On IBM ThinkPads of 2001 the type-1 Product Name is the machine type-model (for the T23 `2647-xxx`, e.g. `26475FU`), the Version string is often `ThinkPad T23` but may be `Not Available`; the gate cannot be trusted blind. Audio otherwise fine: `[audio] ... master=0909 ... reason=ready`, `boot_tone=1khz frames=48000 result=0`.

## Task

1. Always log the SMBIOS entry point verdict and the type-1 strings at boot: `[smbios] version=.. vendor="..." product="..." version="..."` (bounded, printable), plus the type-0 BIOS vendor/version; log `[ec] thinkpad gate=<reason>` when the driver stays off (not_ibm, no_smbios, qemu, safe_mode, ec_handshake_failed) and `[ec] thinkpad volume=.. mute=..` when it runs.
2. Gate: vendor `IBM` (or `LENOVO` with product `ThinkPad`/type-model) **and** the EC handshake on 0x62/0x66 succeeding with a register-0x30 readback that stays stable across two reads; the product/version string check becomes a logged hint, not a condition. Keep QEMU and safe mode excluded.
3. Research with `--search` the T23 SMBIOS strings (Linux `dmidecode` outputs for ThinkPad T23 posted online, thinkpad_acpi's DMI matching in `drivers/platform/x86/thinkpad_acpi.c` which matches `IBM` + model numbers), cite them in `docs/design/hardware-baseline.md`.
4. Host tests: EC model with the T23-like strings (`IBM`, `26475FU`, `Not Available`), a QEMU identity, a non-IBM identity; records asserted.

## Acceptance

Audio host suites green; on the T23 the log shows the SMBIOS strings and either `[ec] thinkpad volume=` records with the keys moving the master, or a gate reason the lead can act on. Files: `thinkpad_ec.c`, `bootinfo.c`/SMBIOS reader, tests. Do not touch the USB stack (f1-59) or `savage2d.c` (f4-22).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research the references first (`--search` is enabled), record links and decisions in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f4-21-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify on QEMU and on the T23.
