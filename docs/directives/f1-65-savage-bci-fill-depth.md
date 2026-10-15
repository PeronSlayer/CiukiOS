# f1-65 — Savage 2D engine: the BCI fill test writes one byte per pixel (T23, image a738204a)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-65` from `main`. Reference sources are already on disk (no network in the sandbox): `build/downloads/savage/savage_bci.h`, `savage_accel.c`, `savage_regs.h`, `savage_driver.h` (X.Org savage driver, MIT licence; read them, do not copy code).

## Evidence (disk log lines 342–372, every T23 boot since the driver exists)

- `[accel] savage probe pci=5333:8c2e cmd=0007 bar0=c0100000 bar1=e8000008 lfb=e8000000 vram=15532032`, `mode=280 bpp=32 pitch=4096`
- `[accel] savage descriptor wrote=10200401 read=10200401 mask=13ff1ffd cr40=01`
- `[accel] savage bci_test phase=fill x=1023 y=767 wrote=008641b6 before=00dce4ec cmd=4bcc8000`
- `[accel] savage bci_test result=failed error=-5 wrote=008641b6 read=b6b6b6b6 read_valid=1 restored=1 status=07e00000 status_valid=1` → `caps=0 init_result=-5`, `quarantined=1`: the desktop runs the software path on every T23 boot.

Decoding `cmd=4bcc8000` with `savage_bci.h`: `BCI_CMD_RECT | BCI_CMD_RECT_XP | BCI_CMD_RECT_YP | ROP 0xCC in bits 16–23 | BCI_CMD_SEND_COLOR | BCI_CMD_DEST_GBD | BCI_CMD_SRC_SOLID | BCI_CMD_PAT_NONE`: a solid copy-ROP fill to the global bitmap destination with the colour sent inline. The colour `008641b6` came back as `b6b6b6b6`: the engine replicated the low byte, i.e. it treated the destination as 8 bits per pixel while the descriptor announced 32 (`0x10200401`: `BCI_BD_BW_DISABLE`, bpp field 0x20, low bits 0x401).

## Task

1. Research in the reference driver which global bitmap descriptor setup applies to the SuperSavage (`SavageSetGBD` switch, chip `S3_SUPERSAVAGE`): the two descriptor registers, the meaning of the low bits (stride in pixels or bytes, the bit 0 flag), the tile bits, the 2D-engine enable sequence and any register that selects the pixel depth for the BCI outside the descriptor (compare bit by bit with the values the kernel writes; the readback mask `13ff1ffd` shows which bits the chip keeps).
2. Fix the descriptor or command setup so the marker test passes; keep the test and the quarantine (a wrong engine must never draw).
3. Host unit test with the register model (`tests/host`, grep `savage`) that reproduces the 8-bpp replication with the old setup and passes with the new one.
4. Record the links (file names and functions of the reference) and the decision in `docs/design/` beside the existing Savage notes.

## Acceptance

Kernel host suite green; the lead verifies on the T23 that `bci_test result=ok`, `caps` non-zero and the desktop frame counters (`[console] [display] … present_us`) drop. Files: `src/kernel/drivers/savage2d.c` and its header.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence files are read-only: `legacy/local/physical/a738204a/disk-BOOT.LOG` (T23 disk log of image a738204a, main `56d41c09`) and `serial.log` beside it. Plain technical prose in every document you touch, no product names of other operating systems, no filler. Report `build/codex/f1-65-report.md` with: root cause (file:line), files changed, test output verbatim, what the lead must verify on QEMU and on the T23.
