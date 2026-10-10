# Directive f1-15: the firmware-first input backend fails at init on QEMU (`error=-5`)

- **Step:** F1. **Contracts:** `f1-acceptance.md` (firmware-first input,
  `safe` on the E500 profile), `device-firmware-ownership.md` (firmware-call
  boundary), the f1-07/f1-07b reports.
- **Implementer:** Codex, `gpt-6-astra`, effort `xhigh`.
- **Worktree:** `wt/f1-fwinput-qemu`. Files: `src/kernel/vm/*.c`,
  `src/kernel/vm/*.asm`, `src/kernel/include/ciuki/{v86,biosvm,fwinput}.h`,
  `src/kernel/drivers/fwinput_adapter.c`, `src/kernel/core/init.c` (the
  input activation call site only), `tests/host/v86_test.c`,
  `tests/host/runtime_init_test.c`, `scripts/test/host_kernel_tests.sh`.

## Observed on image `b0cb346a…` (`qemu-e500` settings: 256 MiB, `platform=e500 safe=1`, SeaBIOS)

`[init] input result=failed error=-5 reason=firmware activation_seq=3`, no
other line from the BIOS VM or the adapter; the `safe` probe then reports
`input_works=0 backend=firmware errors=1` and fails (`safe_contract`). The
same happens on `f1-safe`'s `safe-fw-cfg-qemu-e500` case under the runner.
The native profile passes `safe`.

## What to do

1. Instrument every failure path of `fwinput_init`/`biosvm_init`/setup
   with `klog` (step name, INT/function, registers in/out, VM fault vector
   and CS:IP, registry claim result, deadline elapsed) so the serial log
   explains an init failure; keep the lines under 240 bytes.
2. Reproduce on the host as far as the fakes allow (the init sequence after
   `drivers_init` with the f1-08 firmware leases already published; the
   BIOS VM must transfer the `input` port and IRQ reservations, not claim
   them anew — check `registry_claim_reserved` use and the f1-08 report's
   note "BIOS VM rejects pre-existing IRQ claims").
3. Fix the cause. If it is a SeaBIOS behaviour (for example INT 15h/C2h
   returning an error, or an `IN`/`OUT` to a port the policy refuses during
   setup), handle it per the contract (mouse half disabled, keyboard half
   kept; policy allowlist extended only with the port and reason recorded
   in the source comment).
4. The `safe` probe on the firmware-first profile must then report
   `input_works=1 backend=firmware`.

## Acceptance by the lead

Host tests; kernel build; on QEMU (`qemu-e500`) `f1:safe … platform=e500
safe=1` passes and `f1:input … platform=e500` reaches READY with
`backend=firmware`; native profiles unchanged. Reply with: root cause,
files, test output, and any contract problem found.
