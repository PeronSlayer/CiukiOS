# Review fixes for wt/fx-launch

- **Written:** 2026-10-25 by the lead. Implementer: Codex gpt-6.1-sol, effort high. Apply every finding of the review below with a regression test for each (the test must fail before the fix), rerun the suites that cover the changed files, and report.

**Verdict: merge after fixes** for HEAD `55cd81be0`. Two defects need correction.

1. **[P2] Digest reuse assumes immutability that the loader API does not enforce.**  
   At [sections.c:922](/home/peronslayer/Desktop/CiukiOS-wt-fx-launch/src/kernel/proc/win32/sections.c:922), the earlier digest is reused after potentially waiting for `image_section_building`. However, [loader.c:92](/home/peronslayer/Desktop/CiukiOS-wt-fx-launch/src/kernel/proc/win32/loader.c:92) accepts any nonwritable source range, including a read-only shared mapping with a writable alias. `ua_pin()` preserves that mapping and its frames; it does not freeze writes through the alias.

   Concrete sequence: hash bytes A → wait behind another image build → another thread changes the shared backing to B → seed B using digest A. The section cache can subsequently reuse the wrong image. The removed second hash would have observed B in this sequence. Mutation during seeding was already a separate weakness.

   **Minimal fix:** after acquiring the pin, validate that every source extent is private, nonshared and nonwritable; reject other sources. Alternatively, copy accepted input into an immutable private snapshot before hashing and seeding. Add a regression that changes a writable alias during the serialization wait.

   The normal `W32_LDR_OPEN` → `W32_LDR_IMAGE` path produces a private read-only snapshot. Changing the original disk file between these operations does **not** invalidate that snapshot’s digest.

2. **[P2] Quiet-mode launch timing starts too late.**  
   [syscalls_proc.c:29](/home/peronslayer/Desktop/CiukiOS-wt-fx-launch/src/kernel/proc/syscalls_proc.c:29) substitutes the current time when `spawn_us` is zero. [spawn.c:284](/home/peronslayer/Desktop/CiukiOS-wt-fx-launch/src/kernel/proc/spawn.c:284) supplies zero in quiet mode, and calls `proc_launch_start()` only after argument copying and child preparation. Any paging, allocation or scheduling delay there disappears from the quiet record, while verbose mode includes it.

   **Minimal fix:** capture `proc_launch_now()` unconditionally at `proc_spawn()` entry. Extend the test to advance the clock before `proc_launch_start()` and verify that quiet mode preserves the entry timestamp.

The remaining requested paths look correct:

- **Hash allocation and resize:** pages are zeroed; partial allocation failure frees only fresh pages and preserves the old table. The 32 MiB ceiling receives 32,768 buckets—32 pages on i686.
- **Concurrency:** rehash retains `c->lock` across service points. Readers, writers, `cache_publish()` and `cache_peek()` serialize on it. Writeback retains stable block descriptors across unlocked transport, rather than bucket-head pointers. I found no new corruption or FIFO lock-order defect.
- **Deferred start log:** shutdown and startup-aware defrag still flush directly. Twelve queued records bypass settling; overflow at sixteen still drops the oldest and increments `startup_dropped`. The 60-second limit bounds settling, not subsequent scheduling or disk I/O.

Fresh syntax checks passed for the changed filesystem code and launch/Win32 host harnesses. Runtime tests were not rebuilt or run in this read-only workspace.