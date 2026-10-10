# Directive f1-29: the T23 keyboard controller answers the interface test `0xAB` with `0xFA`

- **Step:** F1. **Contracts:** `f1-acceptance.md` (`input` row),
  `device-firmware-ownership.md`, directive f1-27 (per-step setup
  diagnostics, now merged), the 8042 command set, Linux
  `drivers/input/serio/i8042.c` (no `0xAB`/`0xA9` interface tests in the
  controller check; AUX qualified by loopback), SeaBIOS `ps2port.c`
  (self-test and configuration only).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-i8042-iface`, from `main` at or after `502ad0e`.
  Files: `src/kernel/drivers/i8042.c`, its header, `tests/host/i8042_test.c`,
  `tests/host/test_firmware_records.py` and the `input` case predicates in
  `tests/suites/f1-input.json` only if a record changes.

## Observed on the ThinkPad T23 (image `cb33aec3…`, commit `f7286ca`, sweep capture `44444444`, 2026-10-11)

The f1-27 diagnostics name the failing step on the first boot of the
unattended sweep:

```
[i8042] step=flush        index=1 command=a7 reply=none status_before=1c status_after=1c result=0
[i8042] step=config_read  index=2 command=20 reply=77 status_before=1c status_after=1c result=0
[i8042] step=config_write index=3 command=60 reply=none status_before=1c status_after=14 result=0
[i8042] step=self_test    index=4 command=aa reply=55 status_before=14 status_after=18 result=0
[i8042] step=config_write index=5 command=60 reply=34 status_before=18 status_after=1c result=0
[i8042] step=iface_kbd    index=6 command=ab reply=fa status_before=1c status_after=1c elapsed_ms=1 result=-5
[init] input result=failed error=-5 reason=native activation_seq=3
```

The controller passes the self-test (`0x55`) and accepts the
configuration (`0x77` → `0x34`: translation and both interrupts cleared,
both ports inhibited), then answers the keyboard interface test `0xAB`
with `0xFA` (the device-style acknowledge) instead of the `0x00` result
byte; the driver treats any reply other than `0x00` as `EIO` and aborts.
The IBM embedded controller behind the T23's 8042 interface therefore
does not implement the interface tests the way the driver requires;
Linux and SeaBIOS do not depend on those tests and both drive this
keyboard.

## What to do

1. Stop treating the `0xAB`/`0xA9` interface tests as gating: keep issuing
   them for the record (step records with the raw reply), but accept any
   bounded reply (including `0xFA`, or no reply within the deadline) and
   decide the keyboard and AUX availability from the device-level steps
   that follow (reset/identify/enable with their ACKs), exactly as the
   Linux controller check does; a keyboard whose `0xF5`/`0xF0 0x02`/`0xF4`
   sequence is acknowledged is available. Keep the AUX loopback
   qualification from f1-27.
2. If the EC also acknowledges other controller commands with `0xFA`
   (for example after `0x60`), drain and ignore a single stray `0xFA`
   after controller commands and record it (`stray_ack=1`), never
   treating it as a device byte.
3. Host tests: replay the T23 sequence above through the i8042 harness
   (self-test ok, config ok, `0xAB` → `0xFA`, then device ACKs) and
   require `input result=ready`; the QEMU behaviour (`0xAB` → `0x00`)
   unchanged; a controller whose device reset is never acknowledged still
   fails with the step named.

## Acceptance by the lead

Host tests; kernel build; QEMU `f1-input` cases PASS; on the T23 the next
sweep shows `[init] input result=ready` and the `input` probe's native
cases pass. Reply with: files, the new decision rule, the step table for
the T23 replay, test output.
