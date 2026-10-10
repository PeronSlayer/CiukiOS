# Directive f1-14: `input-fault` fails in the guest; payload host test must skip

- **Step:** F1. **Contracts:** `f1-acceptance.md` (`input-fault` row),
  `test-architecture.md` (host tests never depend on build artifacts being
  present; they skip explicitly).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-input-fault`. Files: `src/kernel/drivers/i8042.c`,
  `src/kernel/drivers/i8042_probe.c`, `src/kernel/include/ciuki/i8042.h`,
  `tests/host/i8042_test.c`, `tests/host/test_image_payloads.py`,
  `scripts/test/host_kernel_tests.sh`. Nothing else.

## Observed on image `a23b7462…` (commit `6ed5ec4`)

Selector `f1:input-fault run=00000002` (also under the runner with
`run=0bd02930`): serial shows `[init] input result=ready reason=native`,
`[selector] probe=input-fault platform=native`, then
`event=BEGIN` immediately followed by `event=END status=FAIL
reason=fault_or_survivor` — no ARM record, no klog. Reading
`probe_input_fault`, this means `i8042_fault_begin` returned an error for
the first case (survivor spawn succeeded). The host orchestration test
passes because it never runs the sequence the guest runs: `drivers_init`
with the native backend active, then the probe.

## What to do

1. Add `klog` diagnostics to every refusal path of `i8042_fault_begin`
   (`fault_selected` false with the selector text and length, IF clear,
   fixture already present, allocation or generation failure) so a guest
   failure is explained in the serial log; emit the errno in the probe's
   END reason (`reason=fault_begin:<errno>`).
2. Reproduce on the host: extend `tests/host/i8042_test.c` with the guest
   sequence (fake boot info with the exact selector string and length as the
   loader stores it — check `menu.inc` for how `test_request_len` is set,
   whether a terminator is counted — native backend initialised and active
   through `i8042_init`, then `probe_input_fault`) and make it fail the way
   the guest does; then fix the cause (the fixture must work while the
   native backend is active: it has private state and no physical claims;
   if the native backend holds something the fixture needs, decouple it).
3. `tests/host/test_image_payloads.py`: when the desktop or Lua payloads are
   not built, the test skips with a clear message (like the Lua SDK skip),
   never errors; the runner's host prerequisite must pass on a tree without
   `build/apps`.

## Acceptance by the lead

Host tests; kernel build; on QEMU `f1:input-fault run=…` passes on
`qemu-t23` and the `f1-input` suite reaches `input` and `framebuffer`.
Reply with: the root cause, files, test output, and any contract problem.
