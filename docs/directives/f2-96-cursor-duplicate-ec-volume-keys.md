# f2-96 — Two cursors on screen after f2-92; the ThinkPad volume keys no longer reach the mixer after f1-100

- **Phase/model:** F2/F1, Claude agent (Opus). **Worktree:** `wt/f2-96` from `main`.
- Evidence: T23 trial of image a4d9454e… (2026-10-22 ~17:40–17:47), disk log `build/lead-scratch/t23-a4d9454e-BOOT.LOG` (5,644 lines, f0-56 protected classes) and `legacy/local/physical/a4d9454e/NETSURF.LOG`; the serial capture did not cover this boot. Owner photo: the hardware cursor and the software cursor are both drawn, a few pixels apart; the owner also reports that the laptop's volume keys no longer change the system volume.
- Records: `[accel] savage mapped mmio=… cursor=f869f000` ×3, `[accel] summary … used=1`; `[ec] thinkpad io=9 transactions=1 … poll_ms=0`, `ec_poll=off`.

## Required

1. Cursor: when the hardware cursor is loaded (f2-92 late readiness), the compositor must stop drawing the software cursor; on any engine loss the software cursor returns; host golden tests for both states and the switch.
2. Volume keys: the T23's keys only update an EC register that the kernel must mirror into the codec. Restore a default poll of one EC read every 2 s (one transaction, two 2.1 ms SMIs: 4 ms per 2 s), `ecpoll=0` keeps it off, `ecpoll=1` the 1 Hz diagnosis rate; the audio refill and the mixer must be unaffected by a 2.1 ms blackout every second (lead periods 8 = 80 ms): add the host test with injected 2.1 ms blackouts at 1 Hz showing no refill overrun above budget and no audible gap in the transport model; keep the `[ec] thinkpad io=` records.
3. Loader tokens within the budget; `test_kernel_latency`, the EC and desktop tests.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Report `build/codex/f2-96-report.md`.
