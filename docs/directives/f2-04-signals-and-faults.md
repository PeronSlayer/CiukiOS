# Directive f2-04: signals, fault delivery and `sigreturn`

- **Step:** F2. **Contracts:** `execution-abi.md` F2 extension ("Signals,
  faults and signal return": supported set, dispositions, masks, delivery
  rules, one active handler per thread, `ciuki_ucontext` 256 bytes with the
  FNSAVE image, the 320-byte frame with token, `sigreturn` validation,
  exception mapping #DE/#MF→SIGFPE, #UD→SIGILL, #PF/#GP→SIGSEGV, #AC→SIGBUS
  with CR0.AM; syscalls 52–55 and 68; the interruption rules: eligibility,
  handler blocking, close/dup2 exemption, issued-read drain, result stored
  before handler entry), `posix-subset.md` (signal library surface,
  async-signal-safe subset), `f2-acceptance.md` (`signals-fault` incl.
  `handler-blocking`; the clock case's EINTR at ~10 ms).
- **Implementer:** Codex, `gpt-6-astra`, effort `xhigh`.
- **Prerequisite on `main`:** f2-02 (processes, threads, syscall layer,
  `wait_word` EINTR plumbing point).
- **Worktree:** `wt/f2-signals`. Files: new `src/kernel/proc/signal.c`,
  `src/kernel/proc/sigframe.c`, `src/kernel/proc/syscalls_signal.c`,
  `src/kernel/include/ciuki/signal.h`; `src/kernel/arch/trap.c` (user
  exception routing to the signal layer; F0 payload termination path kept
  for F0 probes), `src/kernel/arch/isr.asm` only for the return-to-user
  delivery hook, `src/kernel/core/fpu.c` (save/translate the lazy owner's
  x87 image into the FNSAVE format and restore it on `sigreturn`),
  `src/kernel/proc/waitword.c` and the blocking points of f2-02/f2-03
  (EINTR decision helper), `src/kernel/probes/f2_probes_signals.c`,
  `tests/host/proc/signal_*`, `scripts/test/host_kernel_tests.sh`.

## What to build

1. **State**: process-wide dispositions (24-byte `ciuki_sigaction`
   records), per-thread masks (u64), pending sets per process and per
   thread, coalescing, SIGKILL rules, SIGCHLD default ignore, lowest number
   first, SIGKILL precedence; `kill` authority (own group only, PID 1
   excluded), `thread_kill` within the process; SIGPIPE generation by
   channels (hook for f2-05).
2. **Delivery** on return to user mode (or immediately for a faulting
   thread) with no kernel lock held: eligibility test, one active handler
   per thread (async signals stay pending while it runs; a synchronous
   fault inside a handler terminates with the new signal), frame
   construction below the interrupted ESP (320 bytes, four-byte aligned,
   token, siginfo at +32, ucontext at +64), argument registers per
   `SA_SIGINFO`, DF/TF/AC cleared on entry with the interrupted values in
   the context, mask update (signal + action mask), a clean reset x87
   environment for the handler after saving the interrupted image;
   unwritable or exhausted stack → SIGSEGV termination.
3. **`sigreturn`**: whole-frame copy and validation (active frame address,
   token, size, version, selectors unchanged, EIP in an executable mapping,
   ESP in a writable stack range, EFLAGS rules, FPU reserved fields
   sanitized), restore through the lazy FPU machinery, replace the saved
   syscall frame entirely; a bad frame terminates with SIGSEGV.
4. **Exception mapping** in `trap.c` for user-mode faults with the siginfo
   fields (`fault_addr` = CR2 for #PF, EIP for #UD/#DE/#MF/#GP, 0 for #AC;
   `code` values per contract), CR0.AM set at boot with user EFLAGS.AC clear
   by default, blocked/ignored synchronous faults terminate immediately.
   F0 probe payloads keep the `0x100 + vector` termination path (they
   install no handlers): the signal path applies only to F2 processes.
5. **Interruption helper** used by every blocking point: "a caught signal
   eligible for delivery to this thread is pending" → EINTR before
   progress; issued-read drain rule; `close`/`dup2` exempt; completed
   results stored in the interrupted context before handler entry.
6. **Probe** `signals-fault` exactly as the acceptance row (real #PF
   unmapped and read-only, #UD, divide, unmasked x87, #AC with AC set under
   test; handler repairs EIP/x87 and resumes; mask/unmask defers and
   coalesces SIGUSR1; group kill rules; `thread_kill`; SIGKILL, default
   fault, blocked fault, bad stack, handler fault and forged `sigreturn`
   terminate only the offender; `handler-blocking` subcase), with interim
   NASM payloads until the SDK programs replace them.

## Host tests (mandatory)

`tests/host/proc/signal_*`: the pending/mask/eligibility state machine
(coalescing, ordering, SIGKILL precedence, handler-active blocking), frame
builder layout against `abi.h` (offsets, token, alignment, stack
exhaustion), the `sigreturn` validator against every forbidden edit
(selectors, IOPL/VM/NT/IF, kernel EIP/ESP, token/size/version, FPU reserved
bytes), the interruption helper decision table, and the exception → signal
mapping table.

## Acceptance by the lead

Diff review; host tests; kernel build with audit; F0 suites unchanged
(localfault and syslife still terminate with vector codes); on QEMU
`signals-fault` passes on the three profiles. Reply with: files,
interfaces, test output, and any contract problem found.
