# f0-37 — a Normal boot came up with firmware input and no USB; the BIOS VM refused an EBDA size byte of 0; nothing fell back, so the machine had no input

- **Phase:** F0 (boot/platform). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f0-37` from main `85299550`.
- **Authority:** `src/kernel/core/init.c` (input backend decision and activation table), `src/kernel/vm/biosvm.c` (EBDA parsing), `src/kernel/usb/usb_core.c` (`blocked()`), `src/boot/ciukldr/` (boot-info initialization and `input_policy`), `src/kernel/include/ciuki/boot_info.h`, host tests for init/biosvm/loader (find them under `tests/host`), `docs/design/boot-memory.md`, `docs/design/hardware-check.md`.

## Evidence (T23, image d598bf4c, `legacy/local/physical/d598bf4c/BOOT.LOG` from line 472 — the newest boot, read-only)

```
[init] flag safe_mode=0 boot_flags=0000040a activation_seq=1
[biosvm] step=ebda_size error=-14 start=0009fc00 end=0009fc00
[init] input result=failed error=-14 reason=firmware activation_seq=3 tick=102 safe_flag_before=0
[usb] native activation skipped reason=firmware_first_or_safe_mode
[supervisor] desktop pid=2 pgid=2
```

- `0x040a` = `CBI_F_SERIAL_LOG | CBI_F_VBE_CTRL_VALID | A20=CBI_A20_PORT92`; **no** `CBI_F_INPUT_FORCED`, `CBI_F_TEST_REQUEST`, `CBI_F_SAFE_MODE`, `CBI_F_VERBOSE`. Yet `init.c:87` chose the firmware backend, which requires `g_boot.input_policy == CBI_INPUT_FIRMWARE`. The loader sets `input_policy` to 1 only in `menu.inc:295` (`selector_accept`, which also sets `CBI_F_INPUT_FORCED` and `CBI_F_TEST_REQUEST`); `memory.inc:39` skips the KBC A20 method when `input_policy==1`, which matches the recorded `A20=PORT92`. So `input_policy` was already 1 when the loader ran its A20 sequence, without any menu or config request: find the writer (boot-info block not zeroed at every boot? field overlap? stale RAM after the owner's forced reset? cfg parsing?).
- The same machine, in the two previous boots (menu L and H, `boot_flags=00001a0a`, `A20=INT15`): `[init] input result=ready error=0 reason=native` and later `[biosvm] step=ready leases=0 qemu_pmtimer=0` — the EBDA check passed there.
- Result: the desktop started with no keyboard, no mouse and no USB. The owner reported the system as frozen.

## Required

1. **Root cause of `input_policy==1`.** Read the loader's boot-info layout against `boot_info.h` (offset 0x00E), every writer of the block, and whether the block is zeroed before the menu on every boot (cold and warm). Fix so that a plain Normal boot always yields `CBI_INPUT_NATIVE` unless a config source or the selector asked for firmware. In the kernel, `input_policy==CBI_INPUT_FIRMWARE` without `CBI_F_INPUT_FORCED` and without a cfg/fw_cfg source is logged `[init] input_policy=firmware source=unknown action=native` and treated as native.
2. **Fallback.** When the firmware backend fails (`biosvm_init` or `fwinput_adapter_init`), fall back to native `i8042_init()` and let `usb_core` activate natively (`blocked()` must observe the effective backend, not the requested policy); record the fallback in the activation table (`result=ready reason=firmware_failed_native_fallback`). Safe mode keeps the firmware-first policy but must end with a working input backend too; define the order and document it in `docs/design/hardware-check.md` (or the boot design doc that owns input policy).
3. **EBDA extent.** Per the BIOS Data Area convention (segment at 0x40E, size in KiB at EBDA[0]) and the Linux reservation (`arch/x86/kernel/ebda.c`: reserve from the lower of EBDA start and the 0x413 low-memory size up to 0xA0000, regardless of the size byte), when the EBDA size byte is 0 or inconsistent, use `(conventional_kib*1024 … 0xA0000)` as the extent, log `[biosvm] step=ebda_size fallback=lowmem start=… end=…`, and continue; fail only when 0x40E and 0x413 are themselves inconsistent. Cite the sources (Linux source, Ralf Brown's Interrupt List for INT 15h/C1 and the BDA) in the design doc.
4. **Host coverage.** Decision table test for `init.c` (policy × forced × safe × backend success/failure → backend, USB blocked/active, activation rows); `biosvm` EBDA parsing with size byte 0, 1, 4 and inconsistent values; loader: the boot-info block is zeroed on every boot (static check on the assembled loader or a NASM-level unit test if one exists; otherwise document the manual proof). Run `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 -B -m unittest discover -s tests/host -p 'test_version.py'`, `python3 scripts/build_kernel.py`, and the loader build target.
5. Report in `build/codex/f0-37-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. No new register programming; the native i8042 path stays as f0-33 left it (no runtime PIC writes). Distinguish evidence from hypothesis in the report.
