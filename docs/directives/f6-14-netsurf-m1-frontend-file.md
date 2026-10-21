# f6-14 — NetSurf M1 without network: reproducible SDK build of NetSurf 3.11 and its libraries, libnsfb backend on the v3 window, framebuffer frontend with libdeskui chrome, `file:`/`data:` rendering (HTML4/CSS2, GIF/BMP, fonts through the SDK adapter)

- **Owner:** Codex, `gpt-6.1-sol`, effort `xhigh`
- **Written:** 2026-10-21 by the lead from the approved plan `docs/design/netsurf-port-plan.md` (f6-13) and its lead decisions in `docs/directives/f6-13-netsurf-port-plan.md`
- **Worktree:** `../CiukiOS-wt-f6-14`, branch `wt/f6-14`
- **Pins:** `config/netsurf.json` (netsurf-all 3.11, zlib 1.3.1, libpng 1.6.59); archives under `build/downloads/`; hash-checked extraction into `build/external/`; no downloads.
- **Out of scope here:** the HTTP/HTTPS fetcher and DNS (f6-15, after the f6-10 merge), JPEG/PNG content, JavaScript.

## Required

1. **Build recipe** (`sdk/netsurf/build.py` like `sdk/freetype/build.py`): the netsurf-all buildsystem driven with the SDK toolchain (`ciuki-cc`, newlib), host-run generators, `NETSURF_USE_*` options fixed by the plan (no curl, no OpenSSL, no Duktape, libnsfb frontend, `FONTLIB=ciuki`), zlib built for the target, deterministic output, cached by input hashes; installed into `/Programs/netsurf` (binary) and `/system/netsurf` (resources: `Messages`, `Resources/`, icons from the Tango conversions); license inventory under `/system/licenses` (NetSurf GPL-2.0, MIT libraries, zlib, MPL PSL list as the plan records).
2. **libnsfb surface backend** `ciuki`: a v3 desktop window (libdeskui-managed chrome: title, URL bar, back/forward/reload/stop/bookmark buttons, status line) whose content area is the libnsfb framebuffer; damage from libnsfb rectangles only; keyboard and pointer from the desktop protocol mapped to libnsfb events; clipboard through the desktop; resizing recomputes the libnsfb surface; the plan's responsiveness gate (layout slices ≤ 100 ms, input served between slices).
3. **Frontend**: the framebuffer frontend's `gui_*` tables adapted (window, download refusal with message, file dialogs through the existing picker, URL entry, history navigation, bookmarks file `C:\Documents\NetSurf\Bookmarks.html`, history retention 2000, settings file); `file:` and `data:` fetchers; GIF and BMP through libnsgif/libnsbmp; fonts through the SDK adapter with the four charsets of the decisions; the `about:` pages.
4. **Tests.** Host: the build recipe's reproducibility (two builds, identical hashes), unit tests of the surface backend and event mapping against the desktop v3 protocol fixtures (`tests/host/desktop` patterns), rendering goldens of three authored `file:` pages (text + CSS2 layout, a GIF, a BMP, a table and a form) through the host build of the frontend (libnsfb's existing SDL-less surfaces for the host, or the `ram` surface), charset tests; `make CAP= apps` once (the recipe joins the apps build). No QEMU (the lead opens the authored pages in QEMU and on the T23).

## Constraints

Upstream files unmodified (patch files with reasons); no network code; `rg`/`sed` only; no `systemd-run`, no image builds, no commits, no diary edits; host tests one at a time with `nice -n 19`; English. Report `build/codex/f6-14-report.md`.
