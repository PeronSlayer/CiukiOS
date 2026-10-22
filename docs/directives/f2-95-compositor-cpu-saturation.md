# f2-95 — The desktop composites full frames at ~40 fps and saturates the Pentium III

- **Phase/model:** F2, Claude agent (Opus). **Worktree:** `wt/f2-95` from `main`.
- Evidence: T23 trial of image a4d9454e… (2026-10-22 ~17:40–17:47), disk log `build/lead-scratch/t23-a4d9454e-BOOT.LOG` (5,644 lines, f0-56 protected classes) and `legacy/local/physical/a4d9454e/NETSURF.LOG`; the serial capture did not cover this boot.
- Records: `[cpu] window_ms=1999 idle_pm=82–196 … interactive_pm=150–484 normal_pm=185–569` (the CPU is 80–92 % busy at the desktop with a music player and nothing else running); `[cpu] top=desktop#30:324k…379k, desktop#28:234k…484k` per 2 s window (the two desktop threads take 40–50 % of the CPU); `[desktop] accel … frames=78 … lfb_bytes=2271024`, `frames=88 … lfb_bytes=2200056` (about 40 composed frames per second, 1.1 MB/s to the LFB, copies=88 = one engine copy per frame); `[user 45] [music] … starvations=9 … starve ms=26 … close starvations=119` (the player's reservoir empties while the desktop runs); `[desktop] stall stage=idle … no_frame_ms=524–676 input_pending=1 wait=idle` ×19 (no frame for over half a second with input pending); NetSurf `layout_max_us=3252000` for the local welcome page (22 ms on the model). The SMI blackouts are gone (`irqoff_long` 2 in the whole boot, `smi … rate=0.49–0.99/s` only with EC transactions), so the remaining lag is CPU starvation by the compositor itself.

## Required

1. Find what drives ~40 composed frames per second with only the music player open (the player's level meter and `repaint_mps`, timer ticks, the cursor, the accel cache comparison per row, the f2-92 offscreen comparison) and make the compositor compose only the damaged region at the frame rate the damage requires: an animated 20-pixel meter costs a 20-pixel composite, not a full 1024×768×32 frame; a full-frame composite happens only on a full-frame damage; cap the frame rate of continuous small damage at 30 fps.
2. The desktop's idle wait must wake on input: `stall … input_pending=1 wait=idle` must never last more than one frame; find why the loop sleeps with input queued (the wait set, the f2-91 queues, the poll timeout) and add the host regression (input arriving during the idle wait wakes the loop within 10 ms).
3. Budget: on the T23 model, the desktop with the music player open and no window moving must use under 10 % of the CPU (today 40–50 %); a window drag must stay under 40 % with the engine copies; report the model numbers and the `[desktop] accel`/`[cpu] top` lines expected on the T23.
4. Host tests: damage-region compositing golden frames unchanged, the frame-rate cap, the input wake, the CPU model.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Report `build/codex/f2-95-report.md`.

## Amendment 1 (lead, 2026-10-22 18:38, owner decision)

The owner approved the lead's recommendation (2026-10-22 ~18:00): no port of
a window system (X.Org, Wayland, DirectFB, Microwindows); the compositor is
rebuilt on the Windows 98 / X11 model — clipped, region-driven updates and
never a full-frame composition — reusing under MIT licence the X11 region
algorithms (xorg `miregion.c` / pixman `pixman-region32.c`) and pixman's
software blit/fill fast paths (MMX on the Pentium III) where they drop in,
pinned under `third_party/` with their notices and recorded in the desktop
design document. The budget criteria of this directive stay the gate for the
next image.
