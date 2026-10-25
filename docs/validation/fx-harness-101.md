# fx-harness-101 host harness repair

The directive names no external primary-source URLs. Relevant sources checked
before implementation:

- [POSIX wait](https://pubs.opengroup.org/onlinepubs/9699919799/functions/wait.html)
  specifies host status macros. `dosvm_perf_irq_test.c` does not consume host
  wait statuses: use the existing DOSVM harness pattern of undefining these
  names before the guest ABI is included, without changing that ABI.
- [Microsoft PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#the-rsrc-section)
  defines resource metadata by type, name and language. Rebuild the authored
  resource fixture with `scripts/win32/fixtures.py`, regenerate its census,
  and reconcile the source oracle with templates 140/141 in `resources.rc`.
- [Clang SanitizerCoverage](https://clang.llvm.org/docs/SanitizerCoverage.html)
  documents the basic-block and scalar load/store callbacks used by the IRQ
  replay. Its costs are a deterministic software model, not physical latency.
  Preserve existing instrumentation and budgets while investigating vm-clock.

The Savage family fixture and guest CPU runner have no physical mode provider.
Their missing LFB hooks must refuse with `-EOPNOTSUPP`; the guest runner's
cache-alias hook must reject mappings. Physical write draining has no work in
this fixture. Production direct-LFB behavior remains covered by the separate
`dosvm_test.c` fullscreen seams.

The baseline IRQ replay fails only `DOS-audio-clock-per-VM`: 256 us against
250 us. The top half is 3/100 us and the clock turn 264/1000 us. The 40 ms
catch-up reports 80 audio calls (19958 us total) and 83 clock turns (20578 us).
`transfer()` checks the direct-LFB mapping twice even on resident DMA reads.
Only `guest_frame()` between those checks can yield; `as_lookup()` is a plain
page-table read. Move the second check into the pool-resolution branch,
retaining the recheck after a possible yield and the first check on every
pool copy. This is a production latency regression; the existing IRQ budget test
is its failing regression oracle. The native LFB suite also checks binding
which starts during a pool write's yield. Extend that suite with resident
low-memory DMA copies while the LFB is active.

Moving the recheck alone reduces the audio maximum to 252 us (still failing
250 us), clock turn to 260 us and audio catch-up total to 19641 us. Also gate
the first direct-copy block on `linear >= VM_POOL_BASE`: the VBE buffer is
allocated by `dosvm_pool_alloc`, so conventional-memory DMA cannot name it.
All pool addresses retain the original direct-copy check and post-resolution
recheck. No budgets, instrumented targets or workload cases are removed.

[VESA VBE 3.0, accessing linear framebuffer memory, page 6](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf)
requires mapping the reported physical framebuffer into the process address
space. Keep the renderer's validated LFB mapping and pinned-copy path intact;
this optimization only removes a repeated mapping query when no yield occurred.

Final replay of the unchanged workload and cost model:

| Target | Samples (before and after) | Before maximum / budget (us) | After maximum / budget (us) |
| --- | ---: | ---: | ---: |
| DOS clock top half | 47402 | 3 / 100 | 3 / 100 |
| DOS clock turn | 222924 | 264 / 1000 | 257 / 1000 |
| DOS audio clock per VM | 84389 | 256 / 250 (FAIL) | 249 / 250 (PASS) |

The 40 ms catch-up still executes 80 audio turns and 83 clock turns. Audio
model total falls from 19958 to 19396 us; clock total from 20578 to 20016 us.
These are host instrumentation costs, not a hardware qualification.

The full Win32 run also exposed an ASLR-dependent harness failure in
`offline_test.c`: a stack pointer's low 32 bits fell inside the executable
range, so its heuristic reconstructed a static address and ASan reported a
SEGV. [WG14 N3096, 6.3.2.3](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3096.pdf)
does not provide a reversible mapping through an integer narrower than a
pointer. Replace that heuristic with a bounded host pointer table in the
generated Winsock fixture; only pointer packing is adapted, while the real
32-bit records, facade logic and assertions remain intact. Include a
deterministic collision regression. No SDK source or public header changes
are needed.

Host verification completed sequentially at `nice -n 19`:

- IRQ replay passes all targets; latency/Savage passes 16 tests; the census
  passes all six tests; DOS integration passes seven tests with one skip.
- The kernel runner's guest build and synthetic DPMI16/32 plus pinned Quake
  EXEC/CRT checks pass. The native DOSVM suite prints PASS for 60533141
  checks, including direct-LFB copy/revoke races and the new resident-RAM
  case. The ABI oracle prints PASS for 2938 target/native checks.
- Fully green sanitized evidence is blocked: LeakSanitizer cannot perform
  its ptrace-based scan inside the sandbox and exits fatally at teardown.
  Existing sanitizer settings were preserved, including leak detection.
- The final full Win32 run executes 372 tests: 29 failures, 22 errors,
  67 skips. The 29 failures and printing error are LeakSanitizer teardown
  failures; 21 installer-parser errors are empty-stdout JSON decode errors
  from the same sanitizer-blocked helper (its built-in test reports the
  LeakSanitizer fatal). The repaired Winsock fixture reaches PASS before
  teardown; no ASan SEGV remains. Exposing the existing qualified Wine PE
  cache read-only also removes the initial missing-cache error.
- H57/DOSMEM/DOSX stop before CPU boots because the pinned Open Watcom
  `wmake` is killed with `Bad system call` (SIGSYS). The ABI i386 runtime
  probe likewise returns SIGSYS (-31). No emulator or sanitizer bypass was
  used to work around these restrictions.

Exact commands, logs, limitations and integration handoff are in
`build/codex/fx-harness-report.md`. No QEMU, full OS image build or diary
edit was performed; public ABI and SDK headers are unchanged.
