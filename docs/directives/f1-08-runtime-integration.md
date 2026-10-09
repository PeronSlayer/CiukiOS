# Directive f1-08: bring the F1 drivers up at boot, register the probes, `registry` and `safe` probes

- **Step:** F1. **Contracts:** `f1-acceptance.md` (activation order and the
  probes `registry`, `safe`, `input`, `input-fault`, `framebuffer`),
  `device-firmware-ownership.md` (firmware-first lease, quarantine,
  latency), `execution-abi.md` (stack protection with a boot-time guard is
  required from F1), `boot-memory.md` (safe mode: flag bit 0 set before any
  activation; optional devices stay off).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Prerequisites on `main`:** f1-03 (dispatch, `CIUKI_F1_PROBE`), f1-04
  (i8042), f1-05 (fbdev), f1-07 (BIOS VM and `fwinput`).
- **Worktree:** `wt/f1-runtime`. Files: new `src/kernel/core/init.c` and
  `src/kernel/include/ciuki/init.h`, `src/kernel/probes/probes.c` (call
  `drivers_init()` from the probe task before any probe; nothing else),
  `src/kernel/core/registry.c` (`registry_init`: publish `boot-framebuffer`
  and the `input` reservations as firmware reservations transferable by
  `registry_claim_reserved`), `src/kernel/drivers/i8042.c` (reply deadline
  200 ms per the contract; firmware-first `input` outcome, see below),
  `src/kernel/drivers/i8042_probe.c`, `src/kernel/drivers/fbdev_probe.c`,
  new `src/kernel/drivers/fwinput_adapter.c`, new
  `src/kernel/probes/registry_probe.c`, `src/kernel/probes/safe_probe.c`,
  `src/kernel/lib/stackprot.c`, `scripts/build_kernel.py` (CFLAGS),
  `tests/host/*` additions, `scripts/test/host_kernel_tests.sh`.

## What to do

1. **Boot sequence** (`init.c`, `drivers_init()`): after `timing_calibrate`
   and `kwork_init`, in this order with one record each
   (`probe=boot event=DATA group=activation device=… result=…`):
   `fbdev_init` (optional device; skipped entirely in safe mode without LFB
   — safe mode still shows the console), input: `i8042_init` when the boot
   info allows native input, else the firmware backend of f1-07
   (`biosvm` setup + `fwinput`), then `ata_init` if present on `main`
   (otherwise leave the hook). Safe mode (`CBI_F_SAFE_MODE`): only the
   required devices (console, input backend) are activated; optional
   activations are counted and must be 0. Every failure is recorded and
   never stops the boot; a quarantined device stays quarantined.
2. **Probe registration**: register `probe_framebuffer`, `probe_input`,
   `probe_input_fault` and the new probes with `CIUKI_F1_PROBE` in their
   own files; `f1:core` runs them in contract order.
3. **Firmware-first input path**: `fwinput_adapter.c` converts
   `fwinput_poll` events into the native `input_event` queue (same codes,
   `source=INPUT_FIRMWARE`) from a worker/thread so that `probe_input`
   passes with identical counts on the firmware-first profile. The `input`
   probe must never emit `END status=not_run` (not in the grammar): it
   reports `backend=` and passes or fails.
4. **Stack protector**: build the kernel with `-fstack-protector-strong`;
   `__stack_chk_guard` is set once at boot from the TSC and `g_ticks` (and
   RDRAND-free; document the entropy), `__stack_chk_fail` panics with the
   task name; a host test checks the audit still passes and a kernel
   probe-level self-check is not required (the panic path is destructive).
5. **`registry` probe** (`registry_probe.c`): 100 activation/quiesce/
   release cycles on a scratch port range restore `live`, claims and
   releases exactly; conflicting, unknown-size and stale-generation
   requests cause zero device writes (counter) and name both owners; a
   failed idle proof retains quarantine and a second claim fails; the
   shared-IRQ scenario runs without hardware: two owners on an unused PCI
   line that stays masked at the physical PIC, with the dispatcher entry
   invoked through a synthesized frame (document how), both serviced, one
   removed safely, a stuck source quarantined after 1,000 passes. Evidence
   fields as in the contract table.
6. **`safe` probe** (`safe_probe.c`): reports the option provenance
   (`fw_cfg`, `BOOT.CFG`, menu — from the boot info), that the flag was set
   before any activation (activation records carry a sequence after the
   flag), the disabled devices and reasons, the active-owner list, that the
   console and the input backend work, `optional_activations=0`, and
   `READY`. On a boot without LFB and on one without writable storage it
   must still pass without hanging.

## Host tests (mandatory)

Fake-boot tests of `drivers_init` ordering and safe-mode gating (which
init functions are called, in which order, with which boot flags), the
fwinput adapter mapping, the registry probe's scenario logic with the fake
PIC from `kernel_sync_test.c`, and the stack-protector build flag check.

## Acceptance by the lead

Diff review; host tests; kernel build with audit; on QEMU: F0 suites
unchanged; `f1-input` passes on `qemu-t23` (native) and `qemu-e500`
(firmware-first); `f1-safe` passes including the no-LFB and no-storage
cases; `registry` and `framebuffer` pass on all profiles. Reply with:
files, test output, and any contract problem found.
