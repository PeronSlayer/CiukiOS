# f1-93 — Native volume holds must not span cold device reads (T23 window lag)

- **Phase/model:** F1, Claude agent (Opus). **Worktree:** `wt/f1-93` from `main`.
- **Evidence (T23, 1.0.0 alpha serial):** `[storage] vfs_hold drive=C ms=2294 task=desktop#26 op=lookup site=native waiters=2 tagged=3`, `ms=1601/1588/1338/1291 task=desktop#28 op=stat site=native waiters=1..4`, `ms=1910 task=dosvm#53 op=mkdir site=dos`, with `[storage] queue name=ata0 … svc_max_ms=106` (device commands of up to 106 ms) and the desktop's `[desktop] stall stage=launch no_frame_ms=2149`. Windows lag because the desktop waits 1–2 s for its own stat/lookup on C: while another holder reads cold sectors; f1-87 tagged these holds, f1-89 bounded only the guarded lookups and the writer turn.

## Required

1. A native hold (`vfs.c` holds[]) must never be held across a device read: either release and re-acquire around every cold sector read (with the value-validated restart of f1-89) or convert these operations to the guarded path. The gate: on the T23 replay model with 100 ms device service time and a cold cache, no `vfs_hold` above 20 ms for op=stat/lookup/mkdir/open from any task; the desktop's stat during another task's cold directory walk completes within 30 ms.
2. The T23 replay must model cold reads with the measured service time (`svc=…/svc_max_ms=106` of the trial) instead of a warm cache; the existing 0 ms result was a model artefact and must be explained in the validation document.
3. DOS site (`site=dos` mkdir 1.9 s): same rule through the DOS file API path.
4. Host tests that fail on `main` for each of the three operations, then the strict FS gate, `test_kernel_latency`, `test_t23_holds` with the new cold-read actor.

Rules as f1-92. Report `build/codex/f1-93-report.md`, validation `docs/validation/f1-93-holds-cold-reads.md`.
