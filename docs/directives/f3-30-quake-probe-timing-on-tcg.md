# f3-30 — Quake probe timing under TCG (proposed)

- **Model/effort:** `gpt-6-luna`, `low`. Not launched.

## Evidence

`quake-qemu-t23` on image `51c80660` (run of 2026-10-15 19:35): `case=video width=320 height=200 text=0 changes=32 span_ms=209916`, `case=memory … dpmi_active=1 pool_used=25116672`, `case=exit … released=0 terminations=0 error=-110`: Quake starts, enters DPMI and renders (32 screen changes in 210 s) but the software renderer under TCG is far too slow for the probe's exit deadline.

## Task

Measure the demo's progress rate under TCG from the records, set the probe's exit deadline and the case timeout from that rate (or accept a bounded number of screen changes as the pass criterion on TCG only, like the f3-22 wait), and keep the hardware expectation unchanged.
