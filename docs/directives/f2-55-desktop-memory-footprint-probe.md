# f2-55 — The desktop grows its heap by one extent during the crash-isolation cycle (probe `processes_equal=0`)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-55` from `main`. Host-testable; the lead re-runs the QEMU case.

## Evidence (QEMU `qemu-t23`, main 9d6bb01e with the instrumented probe, 2026-10-14 01:38)

```
case=restored server=desktop cycle=1 equal=1 processes_equal=0 procs=4/4 threads=2/2 zombies=0/0 handles=18/18 extents=22/23 backing=1910/1918 tables=6/6
… END status=FAIL reason=desktop_contract
```

The kernel's crash-isolation probe (`src/kernel/probes/f2_probes_desktop.c`, `native_crash_isolation`) snapshots the process ledgers after the desktop and its survivor client are up (baseline), kills a victim client, waits for the desktop to recover, and requires the ledgers to be equal. Since the merges of 2026-10-13 evening the desktop process gains **one memory extent and 8 pages (32 KiB)** between the two snapshots: a heap growth that is never returned. Until yesterday the case passed (`f2-desktop` suite, `desktop-shell-qemu-t23`). Candidates: the per-slot chrome cache of f2-50 (`apps/desktop/compositor.c`, "compact chrome cache for each of the 16 client slots", allocated when a window first appears), the caption/edge caches of f2-45, the system-sound decode buffers of f4-12 (window open/close events fire during the cycle), AutoPlay or media polling buffers (f2-51, f1-43), the wallpaper worker's buffers (f2-44, the thread itself is now created at init). The libc malloc does not shrink the heap on free, so any allocation first made after the first frame shows up as an extent.

## Task

1. Find the allocation(s) with a host harness: run the production desktop through the existing fakes (`tests/host/desktop/` harness, `shell_launch_test.c`/`desktop_test.c` style) over the same scenario (desktop up, client window created, destroyed, recovered) with a malloc hook or an allocation counter, and list every allocation that first happens after the first frame with size and call site.
2. Make the desktop's footprint deterministic after its first frame: preallocate the caches (chrome, caption, sound decode buffers, media/AutoPlay scratch) at initialisation with their steady-state bounds, or allocate them from a pool created at init; document the bound in `docs/design/desktop-rendering-architecture.md` (memory section). Do not change the probe's rule.
3. Host test: the harness asserts no new allocation after the first frame across window create/destroy/recover, menu open/close, a volume tick and a media arrival.

## Acceptance

Desktop host suites green; the lead re-runs `desktop-shell-qemu-t23` and `f1-usb` on QEMU.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-55-report.md` with the allocation list.
