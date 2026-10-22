# f6-20 — NetSurf: the welcome page is rejected by the layout budget; the browser then exits on the T23

- **Phase/model:** F6, Claude agent (Opus). **Worktree:** `wt/f6-20` from `main`.
- **Evidence (lead, QEMU qemu-t23 profile, image state ec82e48b, 2026-10-22 09:15):** NetSurf launched from Programs → NetSurf opens its window and shows "Error occurred fetching page — Local page exceeds the layout admission/time limit." for `about:welcome`; serial `[console] NetSurf layout rejected: local page exceeds admission/time limit.` Screenshot `build/lead-scratch/qemu-netsurf-welcome-rejected.png`, serial `build/lead-scratch/qemu-netsurf-menu-serial.txt`. On the T23 (slower, no 2D acceleration) the owner sees NetSurf start and then disappear; the disk log holds only the lwc leak iterator WARN lines of a normal exit (`desktop/netsurf.c:103 netsurf_lwc_iterator`). The budget lives in the port (`sdk/netsurf/patches/0001-offline-sdk.patch`, `sdk/netsurf/port/budget.c`): local text/pages are subject to a layout admission and time limit meant for hostile remote content.

## Required

1. Local content (`about:` pages, the welcome and error pages, `file:` pages shipped in the image) is never rejected by the time limit: the admission budget applies to remote content only; the welcome page must lay out on the T23 model (1.1 GHz P3, software rendering) with margin — measure its layout time on the host under the port's budget clock and state it.
2. A budget rejection never terminates the browser: the window stays with the error page and "Try Again" works; find the exit path on the T23 (what calls `netsurf_exit` after a rejection: the frontend's main loop on an empty browser list, a `die()` in the gate, the chrome) and remove it; the exit reason must be logged on stderr (`[netsurf] exit reason=…`) before the lwc leak report.
3. Host tests: the welcome page lays out under the budget with the T23 clock scale; a rejected remote page leaves the browser running; the exit-reason line.

Rules: no QEMU (the lead reruns `build/lead-scratch/menu-probe.py`), no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Report `build/codex/f6-20-report.md`.
