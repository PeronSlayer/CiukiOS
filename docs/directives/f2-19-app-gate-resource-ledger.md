# Directive f2-19: `app-gate` verdict after both Lua runs pass — the resource ledger rule

- **Step:** F2. **Contracts:** `f2-acceptance.md` (`app-gate` row: evidence
  includes heap/stack/resident high-water and resource ledgers; the
  desktop/survivor remains alive), `posix-subset.md` (named identities
  survive close until mount teardown; storage cache), `vfs-storage-contract.md`
  (block cache growth and bounds), directives f2-15, f2-17, f2-18.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f2-app-gate-ledger`, from `main` at or after `62bd77f`.
  Files: `src/kernel/probes/f2_probes_app.c`, `tests/host/proc/app_gate_test.c`,
  `tests/host/test_app_gate.py`, `tests/suites/f2-app.json` (the ledger
  predicates only), one paragraph in `docs/design/f2-acceptance.md` if the
  rule needs stating.

## Observed on image `33a68c43…` (commit `62bd77f`, runner case `app-gate-qemu-t23`, 2026-10-11)

Both Lua programs now pass in the guest:

```
case=lua-basic pid=4 exit=0 final_ok=1 assertion_failures=0        (72.9 s)
case=lua-supplement pid=5 exit=0 final_ok=1 assertion_failures=0   (127.8 s)
group=survivor run_case=lua-supplement pid=3 alive=1 pid_unchanged=1 … progress=129
group=resources processes_delta=0 zombies_delta=0 threads_delta=0 fds_delta=0
group=resources mappings_delta=0 backing_delta=0 restored=0
group=resources descriptions_delta=0 surfaces_delta=0 messages_delta=0
group=resources pages_delta=32 kernel_bytes_delta=1024 file_descriptions_delta=0
group=resources live_threads_delta=0 retained_threads_delta=0 waiters_delta=0
group=resources cache_nodes_before=59 cache_nodes_after=60 cache_accounted=0
event=END status=FAIL reason=application_contract
```

Every process-level ledger is balanced; the verdict fails on
`pages_delta=32`, `kernel_bytes_delta=1024` and one more cache node with
`cache_accounted=0`: the storage block cache and the namespace's retained
named identities grew during the file tests of `files.lua` and the
supplement (files created, renamed and removed under `/tmp` and the test
directory), which the contracts allow (the cache is bounded and reused; a
named identity survives close until mount teardown), and the probe's
accounting could not attribute the growth.

## What to do

1. Define the ledger rule for the gate in the probe and, if needed, in one
   contract paragraph: the deltas that MUST be zero are those of the
   process model (processes, zombies, threads, fds, descriptions,
   mappings, backing, surfaces, messages, waiters) and of any kernel
   allocation that is not the storage cache or namespace identities; the
   storage cache growth MUST stay within its configured bound and be
   reported (blocks/nodes before/after, bound); retained named identities
   MUST be counted and reported and MUST be released by the mount
   teardown path (prove it with a host test that detaches the volume after
   the gate and shows the identities freed).
2. Make `cache_accounted` meaningful: attribute `pages_delta` and
   `kernel_bytes_delta` to the cache and identity ledgers; an unattributed
   remainder is a leak and fails the verdict.
3. Suite predicates follow the records; host tests for the attribution and
   the teardown proof.

## Acceptance by the lead

Host tests; kernel build; on QEMU `app-gate-qemu-t23` ends `END
status=PASS` with the ledger records attributing the growth. Reply with:
files, the rule, the records, test output.
