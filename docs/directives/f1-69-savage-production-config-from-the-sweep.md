# f1-69 — Savage: production engine configuration from the hardware sweep; page aftermath (T23, image a395ffa8)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-69` from `main`.

## Evidence (`disk-BOOT2.LOG`, the H page)

- `Savage bpp=32 config=0 bit3=1 BW_DISABLE=1 stride=pixels CR50=3 tile=0 wrote=008641b6 read=b6b6b6b6 FAIL` (the production default since f1-67).
- `config=1 bit3=0 BW_DISABLE=1 stride=pixels CR50=3 tile=0 read=008641b6 PASS`, `config=2 bit3=1 BW_DISABLE=0 … PASS`, `config=3 bit3=0 BW_DISABLE=0 … PASS`, `config=8 bit3=1 BW_DISABLE=1 stride=pixels CR50=0 … PASS`; every `stride=qwords` candidate reads `5a5a5a5a` (nothing drawn); 23 PASS / 49 FAIL at 32 bpp; no 16 bpp candidates were recorded; the log ends with kernel records after the sweep (the page did not reach the audio item: the 1024×768×16 mode switch or the restoration after the sweep hung the display; the owner saw a frozen page).
- `Savage registers: MM8136 wrote=0007 immediate=0000 post_command=0000 CR45=c0 defined_mask=15`.

## Task

1. Make configuration 1 (bit 3 clear, BW_DISABLE set, stride in pixels, CR50 depth bits 11, linear) the production default for the SuperSavage: that is what the hardware accepted; keep the registry override. Cross-check with the vendor driver INF in the pack: `Driver/D/A0400/SSAV1014.INF` and `Driver/D/A0500/ssav1179.inf` (registry values for tiling, block write, memory type; strings in the `.sys`/`.dll` beside them) and with the X.Org/Linux references in `build/downloads/savage/`; record in `docs/design/savage-2d.md` why bit 3 with BW_DISABLE fails on this chip if the sources say it, otherwise record it as the measured fact.
2. Production path on the T23: the marker test with configuration 1 must pass at boot and `caps` must become non-zero; the desktop then uses the engine for fills and copies (verify the hardware cursor and the present paths in the register model with the same descriptor). Record the engine's per-second use in the existing `[accel]` line.
3. Page aftermath: find why the page stopped after the 32 bpp sweep (the 16 bpp mode switch through the display service, or the restoration) from the code; bound it with the display watchdog and restore the desktop mode; run the 16 bpp candidates only after the 32 bpp winner is recorded and only if the mode switch is confirmed restorable on the register model.
4. The USB mouse item reported `device absent (0 ms)` while the receiver was attached later in the same boot (`hid mouse attached` appears in the normal boots): the page must wait for enumeration up to its budget, like the stick item.

## Acceptance

Kernel host suite green (register model: configuration 1 at 32 bpp passes the marker, configuration 0 reproduces `b6b6b6b6`); on the next T23 boot `bci_test result=ok` with the engine active and the H page completing all items.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Build the kernel (`python3 scripts/build_kernel.py`) and the DOS image (`python3 scripts/build_dos_image.py`) before the report. Evidence (read-only): `legacy/local/physical/a395ffa8/` (`disk-BOOT.LOG` latest normal boot with the `[blackbox]` head of the previous boot, `disk-BOOT1.LOG` first boot, `disk-BOOT2.LOG` the Hardware check boot, `serial.log` corrupted). Owner's standing rule: every hardware decision is grounded in the supplied driver package (`third_party/Ultimate Windows Driver Pack (for vista,xp,98,95,me,2000,2003)/` with the INF/registry tables of the vendor drivers, `third_party/drivers/` DOS drivers) and in upstream open sources found on the internet (X.Org, Linux, FreeBSD, SeaBIOS, vendor datasheets); cite them in the report; never program registers from reasoning alone. Report `build/codex/f1-69-report.md`: root causes (file:line), fixes, verbatim test output, what the lead must verify on the H page and on the T23.
