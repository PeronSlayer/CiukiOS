# f6-19 — NetSurf cannot open from the Programs menu; entropy fails on the T23; menu entry and icon

- **Phase/model:** F6, Claude agent (Opus for the entropy part). **Worktree:** `wt/f6-19` from `main`.
- **Evidence (T23, 1.0.0 alpha serial):** `[console] Unable to initialise framebuffer` right after `netsurf#39/#40` start; `sdk/netsurf/port/nsfb_surface.c:34` returns -1 when `ciuki_browser_channel<0`: launched from the menu (no `--channel-fd`), the browser has no desktop channel and dies. `[entropy] sources=0 irq_mask=0000 estimate_bits=0 initialised_ms=0 health=fail reseeds=0` after `boot rounds=2644 cycles=335678959 ms=335`: jitter-entropy initialisation fails on the Pentium III, so `getentropy` never succeeds and HTTPS would never become ready. Owner (2026-10-21 23:50): NetSurf has no icon and sits under "Development"; it is the browser application.

## Required

1. Launching `/Programs/netsurf` from the desktop (menu, icon, `ShellExecute`) must give it its window channel exactly like the GATE_BROWSER path does (`apps/desktop/desktop.c:381-395`): the program's desktop grant and channel fd are part of its program record, not of the test gate. The browser must open its window on the T23 within 3 s of the click.
2. Entropy on real Pentium III hardware: find why `entropy_jent_init()`/the health tests fail (`src/kernel/lib/jitterentropy/port.c`, `src/kernel/core/entropy.c:158-178`; jent `osr`, timer GCD, RCT/APT with a 1.1 GHz TSC) and make the pool initialise on the T23 profile; add the interrupt timing source (`fast.sources`, ATA/USB/PIT edges) as a credited second source so `sources>=1` and `health=ok` with `irq_mask!=0` on the hardware baseline. Record the expected `[entropy]` line for the T23.
3. Menu and icon: NetSurf is listed under Internet (or the Programs root with the other applications, not Development) with the Tango `web-browser` icon in every size the desktop uses; the program record carries the name "NetSurf" and the description "Web browser".
4. Host tests: the launch path without `--channel-fd` opens the surface (mock desktop); the entropy pool initialises with the recorded T23 timer characteristics (fixture from the serial numbers: 2644 rounds / 335 ms) and with the interrupt source alone; the menu record test.

Rules as f1-92. Report `build/codex/f6-19-report.md`.
