# f0-55 — Boot splash: modern progress bar under the Ciuki image

- **Phase/model:** F0, Claude agent (Opus: loader/kernel VESA splash). **Worktree:** `wt/f0-55` from `main`.
- **Owner request (2026-10-21 23:58):** keep the existing Ciuki photograph splash (`misc/CiukiOS_SplashScreen.png`, project identity) and add a nice, modern loading bar so the system can get ready underneath; the owner wants a screenshot (the lead takes it on QEMU).

## Required

1. A progress bar under the Ciuki image on the VESA splash: thin rounded bar, Ciuki palette (derive from the splash colours), indeterminate until the kernel reports phases, then determinate over the boot phases already recorded (`[init]` activation sequence: storage, drivers, supervisor, desktop); it advances from the kernel's init phases, never from a timer alone; no text other than the tagline `Classic computing, one woof at a time.`
2. The splash stays until the desktop's first frame (handover without a black frame); on `safe=1` and on the serial-only path the bar is drawn the same way.
3. Cost: the bar's drawing must not take the framebuffer lock for more than 1 ms per update and must not add any disk read; the splash image itself stays as today.
4. Host tests: the bar renderer (golden frames at 0 %, 40 %, 100 %, indeterminate), the phase mapping, the handover rule. The lead takes the QEMU screenshot after the merge.

Rules as f2-89 (the loader extent has 887 free bytes; prefer the kernel splash path). Report `build/codex/f0-55-report.md`.

## Amendment 1 (lead, 2026-10-22 02:25, decisions on the delivery)

Safe mode (`safe=1`) keeps the text log instead of the splash, as
`docs/boot.md` defines; the bar is drawn on every splash boot (normal and
serial-log boots). The 100 % hold while the desktop starts and the mode
switch blank are accepted for the 1.0.0 alpha; a desktop-reported stage is a
follow-up.
