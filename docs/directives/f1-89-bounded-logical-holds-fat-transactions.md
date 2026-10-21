# f1-89 — bounded logical holds: reserve/transfer/publish FAT transactions with a nonblocking reserved cache publication (part B of f1-87)

- **Owner:** Codex, `gpt-6-astra`, effort `xhigh` (owner rule: Astra after two Sol sessions failed on the same problem, f1-87 sessions 1 and 2; the lead states this here as required)
- **Written:** 2026-10-21 by the lead. f1-87 delivered A, C, D, E, F, G; part B stays red: on the T23 service model (111 ms per device command) the logical FAT mutation guard (`mutating`, taken by `vfs_io_begin_locked()` and kept until the whole operation returns) is held **999 ms for `mkdir`** (one preflight command, two DATA, three CHAIN, three DIRECTORY) and **111 ms for `log_write`**, against the acceptance of **20 ms**. The T23 trial measured 50–1500 ms holds by the desktop and 1.6 s by the log writer with up to 4 waiters (dev diary 2026-10-21-02).
- **Worktree:** `../CiukiOS-wt-f1-89`, branch `wt/f1-89` (branched from `wt/f1-87` at 881c5df8 so A, C, D, E, F, G are present; the lead merges `main` when needed)
- **Read first:** `docs/directives/f1-87-*.md` (the lead's reserve/transfer/publish design in Amendment 1 and the "B: exact failure and implementation obstacle" section of `build/codex/f1-87-report.md`), `docs/design/resource-management-win98.md` (C2 durable boundaries and crash-cut classification, C3 reader fairness), `src/kernel/fs/cache.c` (`cache_write` :1023, `cache_group_begin` :1264, `barrier_active`, the frontier on `cache_device`), `src/kernel/fs/cache.h`, `src/kernel/fs/fat.c` (`write_span` :825, mkdir, delete, rename), `src/kernel/fs/vfs.c` (`vfs_io_begin_locked`, holds and tags), the fs host suite (`scripts/test/host_fs_tests.sh`, 2339 crash cuts) and `CIUKI_F187_REQUIRE_BOUNDED=1` (the B acceptance gate already wired by f1-87).

## The obstacle, as f1-87's second session stated it (binding facts)

1. `cache_write()` can wait for dirty credit, fetch/evict a slot, or wait on `barrier_active`; having read a metadata block earlier neither pins it nor makes its later publication nonblocking.
2. `cache_group_begin()` waits on an active barrier; there is no API that reserves all metadata slots and credits outside the guard and then publishes them atomically without waiting.
3. `cache_block` holds one mutable image; the frontier is per device; a cached metadata version carries no ordered publication dependency of its own.
4. FAT's `write_span()` makes DATA durable, builds a detached CHAIN, barriers, publishes LINK, barriers, then DIRECTORY and FSInfo; detached EOC and final link can modify the same FAT sector; replacing these barriers with one publication of final bytes would lose the intermediate image C2's crash cuts require. `mkdir`, delete and rename have their own durable boundaries.
5. Cold planning must fetch/pin metadata outside the bounded guard and revalidate before reservation/publication; borrowed identities today rely on the retained guard.

## Required

Design and implement, with the research cited (Windows 98 IOS/VFAT and IFS Manager behaviour on volume locking versus request completion; the Linux/BSD journaling precedent for ordered metadata publication is acceptable as an engineering reference, not as a model to copy):

1. **Reserved cache publication API** in `cache.c`: `cache_reserve(set)` taken outside the guard pins the needed metadata slots (fetching them if cold) and the dirty credit for the whole transaction, returning a reservation with generations; `cache_publish(reservation, images[], order)` publishes the prepared images **without waiting** (no credit wait, no slot wait; a barrier in progress either delays the publication to after the barrier in a bounded way or the publication is split per durable boundary, see 3); `cache_release(reservation)` on abort. Per-version ordering: each published image carries its dependency on the previous durable boundary so the frontier logic stays correct.
2. **FAT transactions**: `mkdir`, `log_write`/append, file extend/write, `commit`, delete, rename become reserve (guard, ≤ 1 ms model) → transfer (no guard; DATA to the block layer under the writer budget and C3 fairness) → publish (guard, ≤ 1 ms model) **per durable boundary**: where C2 requires an intermediate durable image (detached CHAIN before LINK, LINK before DIRECTORY), the transaction publishes that image in its own short guarded step and waits for durability **outside** the guard; readers between steps see a consistent prefix (allocated-but-unlinked clusters are invisible to them and reserved against reuse through the in-flight reservation set). Borrowed path/node identities are pinned for the transaction's life (pin count) and revalidated by generation at every publish step; a concurrent unlink/rename of the target aborts the transaction cleanly.
3. **Crash cuts**: every existing crash-cut classification of the fs suite stays green (the 2339 cuts) and new cuts are added at each new step boundary; orphan clusters from an abort after transfer are reclaimed by the existing mount-time scan (test).
4. **Acceptance**: `CIUKI_F187_REQUIRE_BOUNDED=1 bash scripts/test/host_fs_tests.sh` green: no logical hold above 20 ms with the 111 ms service model for `mkdir`, `log_write`, `commit`, file writes, delete and rename; the physical mutex maximum stays 0; the replay of the trial's hold distribution in `test_kernel_latency` keeps zero `device_overrun task=audio-refill`.
5. **No weakening**: the thresholds, the crash-cut suite and the diagnostic observer stay as they are.

Report `build/codex/f1-89-report.md` with the design (one page), citations, the per-operation step table (step, guard held, device commands, durable boundary), exact commands and results. `rg`/`sed` only; no QEMU, no `systemd-run`, no image builds, no commits, no diary edits; no new `panic()` paths; English. If an obstacle makes 20 ms impossible for one operation, deliver the others and state the obstacle precisely with the measured hold.

## Amendment 1 (lead, 2026-10-21 20:44, after the cross-review)

The cross-review is in `build/codex/f1-89-review.md`. Decisions:

**Required before merge** (each with a host test that fails on the current
code):

1. Review item 1: only a real `barrier()` waits for durability; the 16-sector
   overflow of a group publishes without a flush, chunks of one group share a
   cache group so write-back merges to 32 KiB again, and file extension steps
   grow 128 KiB at a time. Evidence: the 10 MiB copy replay back near 1,278
   write commands; the C3 copy on the new path at or above the old path's
   throughput in the same run.
2. Review item 2: `W32_F_ATTR` takes the writer turn before resolving the
   path; a writer operation whose identity is unlinked but whose slot index is
   still set is refused with ENOENT; the slot index is cleared when a legacy
   unlink is reported; add the delete-versus-SetFileAttributes race test.
3. Review item 3: transaction objects are preallocated per volume (a small
   pool, size stated and measured) or lookups use a light object built from
   ordinary pages; no 24 KiB contiguous allocation per path component.
4. Review item 7 (lead decision): in-flight readers count in `guard_busy`;
   a writer-turn wait metric (max and count per drive) joins the T23 replay
   and the strict gate; the T23 model must show no writer-turn wait above
   20 ms behind the desktop copy.
5. Review item 4 (lead decision): the guarded third lookup attempt must not
   read the disk while holding the guard: warm the needed sectors before
   taking it, or drop the guard around the read and restart; lookups over
   32 sectors restart rather than fail.

**Follow-ups** (record in the report, not in this round): items 5, 6, 8, 9,
10 of the review. Item 5 (abort after a durable partial step) is the first
follow-up directive after the 1.0 Alpha.

Then rerun the strict FS gate, the C3 variants, the 10 MiB copy replay and
`test_kernel_latency`, and append the numbers to `build/codex/f1-89-report.md`.
