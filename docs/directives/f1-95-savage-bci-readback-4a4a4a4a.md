# f1-95 — Savage BCI self-test still fails on the T23 (no 2D acceleration)

- **Phase/model:** F1, Claude agent (Opus). **Worktree:** `wt/f1-95` from `main`.
- Evidence: T23 trial of image b453bc92… (2026-10-22 ~08:30, serial copy `build/lead-scratch/t23-b453bc92-serial.txt`, disk log `legacy/local/physical/b453bc92/BOOT.LOG` copied to `build/lead-scratch/t23-b453bc92-BOOT.LOG`).
- Records: `[accel] savage bci_test phase=fill x=1023 y=767 wrote=0042b94a before=00181c10 cmd=4bcc8000`, `bci_test result=failed error=-5 wrote=0042b94a read=4a4a4a4a read_valid=1 restored=1 status=07e00000`, `depth firmware_cr50=30 bpp=32 descriptor=10200401 order=cr50_before_gbd`, `completion reg=48c64 before=00000001 requested=0002 observed=00000002 pending=0 status=07e00000 idle_bits=7`, then `quarantined reason=bci_test_readback_failed`; mode=280 bpp=32 pitch=4096 vram=15532032, pci 5333:8c2e (SuperSavage IX/C). The full init register trace is in the serial (`[accel] savage init reg=…`). f1-88's CR50-before-GBD order did not fix it: the fill completed (status idle, completion observed) but the pixel reads back as 4a4a4a4a — a byte-replicated 0x4a, which looks like an 8-bpp fill of the pattern (0x42b94a low byte 0x4a) or a wrong pixel format/pitch in the BCI global bitmap descriptor, not a missing command.

## Required

1. Research: SuperSavage/IX BCI 2D programming in the X.org savage driver (savage_accel.c: `SavageInitialize2DEngine`, GBD/PBD setup, `BCI_CMD_RECT` with `BCI_CMD_DEST_GBD`, pattern/solid fill colour register, the SuperSavage-specific `GlobalBitmapDescriptor` bits for 32 bpp, `BCI_BD_BPP` encoding, tile/stride fields, the "CR50 depth" bits) and the Savage programming documents in `third_party/drivers`; cite file and lines.
2. Fix the descriptor/pixel-format for 32 bpp so the fill writes the full 32-bit colour; verify with the readback of several pixels and both a solid fill and a screen-to-screen copy; keep the quarantine on a real failure.
3. Host tests on the Savage register model with the recorded init trace: the descriptor built for mode 280/32 bpp/pitch 4096 matches the X.org encoding; the self-test passes on a model that implements the GBD rules; the old descriptor fails the model the same way the hardware did (read=4a4a4a4a).
4. Report the expected `[accel]` lines on the T23 (caps non-zero, no quarantine) in `docs/validation/f1-95-savage-bci-32bpp.md`.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Report `build/codex/f1-95-report.md`.

## Amendment 1 (lead, 2026-10-22 09:31, decision on the delivery)

The delivery refutes the directive's premise: the descriptor `10200401` and
the fill command match X.org and savagefb; the measured behaviour is that the
first colour fill after the engine reset repeats the colour's low byte and
every later fill is exact (five T23 boots, the a395ffa8 sweep). Accepted: the
self-test primes with a colour-0 fill, then a marker fill and a
screen-to-screen copy on a saved 8×2 block, re-run after every engine reset;
the quarantine stays the safety net. The f1-69/f1-88 conclusions are
superseded. T23 acceptance: `result=ok … phase=done pixels=16`, `caps=3`.
