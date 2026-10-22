# f6-23 — NetSurf: the address bar and the toolbar do not respond; navigation does not start

- **Phase/model:** F6, Codex gpt-6.1-sol, reasoning high. **Worktree:** `wt/f6-23` from `main`.
- Evidence: T23 trial of image 3423912f… (2026-10-22 ~22:40–22:50): serial `build/lead-scratch/t23-3423912f-serial.txt` (14,803 lines), disk log `build/lead-scratch/t23-3423912f-BOOT.LOG`, `legacy/local/physical/3423912f/NETSURF.LOG`. Music and window moves are now smooth (owner). Network works: `[net] dhcp state=bound ip=192.168.1.123`, `[net] resolver first_answer … elapsed_ms=155`, NETSURF.LOG `network=dns-ok`. Owner: NetSurf opens and typing goes into the page, but the address bar cannot be edited (no delete, cannot type a URL) and no toolbar button can be clicked; it seems to lack functionality. `layout slow elapsed_us=2717000 class=local`.

## Required

1. Find why pointer clicks and keyboard focus never reach the chrome (address bar, back/forward/reload/stop/home, status) in the CiukiOS port (`sdk/netsurf/port/chrome.c`, `nsfb_surface.c`, the input translation: coordinates relative to the window vs the page area with `CIUKI_BROWSER_TOP`, focus routing between the libnsfb toolbar widgets and the page, key translation for Backspace/Delete/arrows/Home/End/Enter/Ctrl+A/Ctrl+L); compare with the upstream framebuffer frontend's widget handling (`frontends/framebuffer/gui.c`, `fbtk`), cite file:line.
2. Fix it: clicking the address bar focuses it and selects its text; typing replaces it; Backspace/Delete/arrows edit; Enter navigates; Ctrl+L focuses; every toolbar button works; the page keeps its own focus and input when clicked.
3. Local page layout cost: the welcome page took 2.7 s on the T23; find the cost (fonts, image decode, layout passes) and bring it under 300 ms on the model.
4. A QEMU probe script for the lead (`scripts/test/netsurf_probe.py`, modelled on `scripts/test/control_display_probe.py`): open NetSurf from the Programs menu (Logo, Right, click at the NetSurf entry), click the address bar, type `http://example.com` and Enter, screendump every second for 20 s; the port logs `[netsurf] input target=chrome|page widget=…` for each click and key (bounded).
5. Host tests: chrome input routing, address-bar editing, button actions, navigation from the bar.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Write `build/codex/<id>-report.md`.
