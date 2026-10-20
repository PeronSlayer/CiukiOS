# f0-49c — pagefile extents persist across boots; the boot-time grow must not rewrite 16 MiB nor hold the C: reservation across the data write

- **Owner:** Codex, `gpt-6.1-sol`, effort `xhigh`
- **Written:** 2026-10-20 by the lead from the T23 and QEMU boot logs
- **Worktree:** `../CiukiOS-wt-f0-49c`, branch `wt/f0-49c`

## Evidence

Every boot logs `[pager] stats … extents=0` then `[pager] grow extents=1 capacity=4096 runs=2` and `[pager] attach extents=1`: the store opens with `existing=0`, so `open_store()` grows one extent (`PAGER_EXTENT_SLOTS` pages, 16 MiB) on every boot. During that grow `pagerman` is at 50–55 % CPU for the first two cpustat windows on the T23 (`[cpu] top=pagerman#6:551k551s2`) and 31–47 % in QEMU; the T23 disk writes at about 2.8 MB/s, so the grow costs seconds, and the first `[storage] vfs_wait drive=C ms=340 reason=volume` records appear right then. Under the QEMU `icount` profile the desktop never prints its first stage line while `pagerman` dominates the CPU (`normal-boot-qemu-t23`, fails on every image since at least `588609e7`).

## Required

1. **Persistence.** `src/kernel/fs/pagefile.c` (the backend) keeps the pagefile and its extents across boots: `open` reports the existing extents (size, layout) and `open_store()` maps them (the code already handles `existing > 0`: slots start FREE, pages are zeroed on first use); a grow happens only when demand exceeds the existing capacity. Find why `existing` is 0 (file removed at shutdown or at open, size not recorded, layout check failing) and fix the cause; the pagefile is a normal hidden system file (`C:\System\PAGEFILE.SYS` or the existing name) that survives a clean shutdown and an unclean one (the Windows 98 model: the swap file persists, its contents are never trusted).
2. **Grow without a data write.** Allocating an extent reserves clusters and sets the size (the FAT chain, the directory entry) without writing 16 MiB of data: use or extend the reservation primitive the boot log uses (`vfs_log_reserve`); if the FAT code needs a data write for correctness, write only the metadata and record why. The C: volume reservation (mutation) is held for the metadata update only, never across a multi-megabyte device write; the reservation hold must stay under 50 ms on a 2.8 MB/s device.
3. **Boot ordering.** The pager's boot-time work must not delay the desktop spawn or its page-in: the supervisor's `/Programs/desktop` ELF load and first frame may not wait for the pager manager; if the manager needs the store before admission, admission waits on it with a bound, the desktop does not.
4. **Tests.** `tests/host/fs` pagefile tests (open with existing extents, no data write on grow, reservation hold time on a slow fake device, persistence across a simulated reboot), `tests/host/proc`/kernel tests for the boot ordering, `scripts/test/host_kernel_tests.sh` as far as the sandbox allows. No QEMU (the lead runs `f0-smoke` and the T23).

## Constraints

Keep the f0-49/f0-49b cursor and reclaim semantics; no new `panic()`; `rg`/`sed` only; no `systemd-run`, no image builds, no commits, no diary edits; host tests one at a time with `nice -n 19`. Report `build/codex/f0-49c-report.md`.
