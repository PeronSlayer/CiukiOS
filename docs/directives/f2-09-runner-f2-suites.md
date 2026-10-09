# Directive f2-09: the `f2:` selector phase, F2 suites and application evidence

- **Step:** F2. **Contracts:** `f2-acceptance.md` (selection and evidence,
  probe matrix, Lua application evidence, suites and deadlines, result.json
  additions), `f1-acceptance.md` and `f0-acceptance.md` (grammar, runner
  rules), `test-architecture.md`.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Prerequisite on `main`:** f1-03 (phase-scoped dispatch, `CIUKI_F1_PROBE`
  section registration, runner-side digests).
- **Worktree:** `wt/f2-runner`. Files: `scripts/test/run.py`,
  `scripts/test/evidence.py`, `scripts/test/loader_model.py`,
  `scripts/test/qmp.py`, `tests/suites/f2-*.json`, `tests/host/test_runner.py`,
  `tests/host/fake_qemu.py`, `src/boot/ciukldr/menu.inc` (F2 probe names
  and the `f2:` prefix only), `src/kernel/probes/probes.c` (parser phase 2
  and dispatch to a `.f2probes` section; add `CIUKI_F2_PROBE` to `probe.h`
  and the section to `linker.ld` in the same pattern as F1). Nothing else.

## What to build

1. **Selector**: grammar `f[012]:<probe-id> run=<8hex> [platform=e500]
   [safe=1]`; F2 probe ids `elf-load`, `spawn-wait`, `fd-table`, `mmap`,
   `signals-fault`, `threads-wait`, `crash-isolation`, `libc-smoke`,
   `app-gate`; no `all`/`core` for F2 in the kernel (contract); the loader
   list, the kernel parser, the model and the evidence `PROBES` set agree.
   A missing F2 probe yields `BEGIN`, `READY table=f2 installed=<n>`,
   `ERROR status=not_run reason=missing_probe` like F1.
2. **Application evidence**: the runner frames application output
   separately from controller records (contract: untrusted stdout never
   becomes a record even if it contains `CIUKI_TEST`); the kernel controller
   wraps application lines into `DATA group=app` records with a bounded
   streaming digest and head/tail retention when output exceeds the 4 MiB
   log limit; result.json gains `abi_version`, `sdk_manifest_sha256`,
   newlib and application hashes, ELF hashes, argv/env/cwd/fd setup, wait
   status, exclusions and resident/committed high-water fields (values are
   supplied by the probes; the runner records what it receives and marks
   missing fields).
3. **Suites**: `f2-process.json` (elf-load, spawn-wait, fd-table; 180/180/
   300 s), `f2-runtime.json` (mmap, signals-fault, threads-wait, libc-smoke;
   180/180/300/180 s), `f2-desktop.json` (crash-isolation normal, no-LFB
   and safe fallback; 300 s), `f2-app.json` (app-gate; 900 s) on the icount
   profiles `qemu-t23`, `qemu-e500`, `qemu-min128`; an `f2-all` runner alias
   in the contract order; F0 and F1 regressions first; QMP input stimuli
   for crash-isolation reuse the F1 actions; predicates from the acceptance
   table (counts, `leaked=0`, `desktop_restarts=0`, `assertion_failures=0`,
   `final_ok=1`).

## Host tests (mandatory)

Grammar tests for `f2:` (phase separation, no alias), the application
output framing (a stdout line containing `CIUKI_TEST` must not parse as a
record), the digest/head-tail retention at the log cap, suite loading and
predicate evaluation against fabricated record sets for every F2 probe,
and the kernel map check for `.f2probes`. Keep every existing test green.

## Acceptance by the lead

Diff review; host tests; loader and kernel build; on QEMU the F0 and F1
suites unchanged and a dry `f2-process` run reaches `READY` then
`not_run` for the missing probes. Reply with: files, the final grammar
regex, suite case ids, test output, and any contract problem found.
