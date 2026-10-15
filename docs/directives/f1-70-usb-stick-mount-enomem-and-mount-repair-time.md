# f1-70 — USB stick mounts read-only with -12 (ENOMEM); mount repair of the system volume takes 6.4 s (T23, image a395ffa8)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-70` from `main`.

## Evidence (`disk-BOOT.LOG`, `disk-BOOT2.LOG`)

- Normal boot: `[usb-storage] stage=max_lun … elapsed_ms=517`, `inquiry 585 ms`, `ready`, `capacity`, `flush`, `mode`, then `[usb-storage] volume drive=E part=1 fat=32 mode=ro dirty=0 error=-12`: the stick (first time) enumerates in about 1.2 s and reaches the volume stage, but the FAT32 mount ends read-only with `-ENOMEM`; the owner cannot use it.
- H page: `PASS Mount repair: FAT32 no_publish repaired=1 writable=1 … (6365 ms)` and the normal boot's `[storage] repaired logs=1 lost=334 … dirty=cleared`: the writable upgrade with the bounded ownership scan (f0-25) costs about 6 s on the system volume at every unclean boot, more on a large stick.

## Task

1. Find the allocation that fails for the stick's FAT32 (the ownership bitmap or cluster table sized by the cluster count, the metadata windows, the cache) and size the structures for volumes up to 32 GiB within a fixed budget (a bitmap of one bit per cluster, allocated from the normal pool with a bounded fallback that scans in passes); the stick must mount writable when its FAT is consistent; otherwise the reason is recorded with the needed and available bytes.
2. Make the ownership scan and the writable upgrade fast: read the FAT in 64 KiB runs (f1-66's windows), avoid per-cluster lookups, skip the full scan when the dirty flag is clear and the copies agree, bound the dirty-volume scan to the used extent; target below 1 s on the T23's 2 GB system volume and below 5 s on a 4 GiB stick; the page's `Mount repair` item and the normal boot record report the time.
3. The page's USB storage item must wait for enumeration up to its budget (it reported `SKIP … (8033 ms)` while the stick is detected within 1.2 s in the normal boot: check the page's start time versus the worker's first scan) and report mount, capacity and a 1 MiB read with elapsed ms.

## Acceptance

USB and filesystem host suites green (fixtures for a 4 GiB and a 32 GiB FAT32 layout); QEMU `f1-usb` and the `f4-media` stick cases green; on the T23 the stick mounts writable within budget and the normal boot's repair takes under 1 s.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Build the kernel and the DOS image before the report. Evidence (read-only): `legacy/local/physical/a395ffa8/`. Owner's standing rule: ground every hardware decision in the supplied driver package (`third_party/Ultimate Windows Driver Pack (for vista,xp,98,95,me,2000,2003)/`) and in upstream open sources found on the internet; cite them; never program registers from reasoning alone. Report `build/codex/f1-70-report.md`: root causes (file:line), fixes, verbatim test output, what the lead must verify on the H page and on the T23.
