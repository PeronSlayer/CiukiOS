# Directive f2-17: the `app-gate` probe double-faults (kernel stack overflow) before launching Lua

- **Step:** F2. **Contracts:** `f2-acceptance.md` (`app-gate` row), directive
  f2-15, `execution-abi.md` F2 extension, the kernel stack rules of the F0
  foundations (`docs/design/` kernel-core contract: one 8 KiB kernel stack
  per task with a guard page below it).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-app-gate-fix`, from `main` at or after the f2-15
  merge (`b09a614`). Files: `src/kernel/probes/f2_probes_app.c`,
  `src/kernel/proc/supervisor.c` only if the capture/report helpers it
  provides are the stack consumers, `scripts/build_kernel.py` only to add
  a frame-size guard (`-Wframe-larger-than=` or an equivalent post-link
  check), `tests/host/proc/app_gate_test.c`, `tests/host/test_app_gate.py`,
  `scripts/test/host_kernel_tests.sh`.

## Observed on image `dae098ef…` (commit `b09a614`, runner case `app-gate-qemu-t23`, 2026-10-11)

```
probe=app-gate event=BEGIN
probe=app-gate event=DATA group=app-gate application=Lua version=5.4.8 abi_version=1 extra_skips=0
probe=app-gate event=DATA group=exclusions …
probe=app-gate event=DATA case=write-gate expected=0 observed=0
probe=app-gate event=DATA case=desktop server=desktop pid=3 pgid=3 expected=1 observed=1
probe=app-gate event=DATA group=metadata name=fd_setup part=3 parts=5 encoding=json hex=…
probe=app-gate event=DATA group=metadata name=fd_setup part=4 parts=5 encoding=json hex=…
probe=app-gate event=DATA group=metadata name=fd_setup part=5 parts=5 encoding=json hex=…
probe=panic event=PANIC vector=8 error=00000000 eip=c0110cea esp=f0007000 cr2=f0006ffc why=double_fault
```

The guest double-faults with `ESP` at the bottom of the probe task's
kernel stack and `CR2` four bytes below it: a kernel stack overflow in the
probe task while it builds the metadata records (JSON provenance read from
`/system/tests/app-gate.meta`, hex-chunked into ≤240-byte records) before
the first Lua launch. The runner records "version, run or probe mismatch"
because the next record comes from the panic path. The host harness did not
catch it: the host stack is not bounded.

## What to do

1. Find the frames: measure the probe's stack usage on the path from
   `probe_f2_app_gate` to the metadata records (large automatic buffers:
   provenance text, JSON parsing, hex chunk buffers, capture tails, the
   record formatter) and the helpers it calls in `supervisor.c`. Move every
   buffer larger than a few hundred bytes to static storage owned by the
   probe (the probe runs once per boot) or to the existing bounded capture
   structures; keep the records and their names unchanged.
2. Add a guard that fails the kernel build or a host test when any kernel
   function's frame exceeds a bound compatible with the task stack (clang
   `-Wframe-larger-than=<N>` with `-Werror` for the probes directory, or a
   post-link check from the `.stack_sizes` section / the assembler listing),
   and record the bound chosen in the directive report with the largest
   frames found.
3. Host test: the probe's metadata path runs in a thread with a stack of
   the kernel task size (`pthread_attr_setstacksize`) plus a guard page,
   so a regression overflows on the host too.

## Acceptance by the lead

Host tests; kernel build with the new guard; `make build-full`; on QEMU
the runner case `app-gate-qemu-t23` proceeds past the metadata records to
both Lua runs (their own result is a separate question answered by the
same run). Reply with: files, the frame sizes before and after, the guard
bound, test output.
