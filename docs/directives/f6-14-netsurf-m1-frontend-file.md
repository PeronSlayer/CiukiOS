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

## Lead decisions on the delivery (2026-10-21)

1. The layout admission limits (75 ms monotonic deadline at stages, blocks and lines; 8192 boxes, depth 64, 8192-byte runs, 64 columns, 128 KiB layout text) are accepted provisionally; the 100 ms responsiveness bound is measured by the lead in QEMU and on the T23 with the authored pages and revised by directive if exceeded.
2. The URL entry stays at the toolkit's 1039 bytes for M1; a larger entry is a later toolkit change if a site of the acceptance list needs it. Local directory listings go through the desktop picker (accepted).
3. The 32 MiB resident target is measured by the lead at M1 on the 128 MiB profile.

## Amendment 1 (lead, 2026-10-21, from the Codex cross-review `build/lead-scratch/f6-14-review.md` in the main checkout: verdict "request changes")

Apply every finding below with a host regression. Lead decision on finding 4: the desktop protocol's DAMAGE reply is not a consumption fence and will not become one; NetSurf follows the libdeskui client model — render into a private back buffer, copy only the damaged rows into the shared surface immediately before DAMAGE, and never block on consumption; intra-frame tearing is accepted as for every other client. Finding 1 (inherited data.c bound) is fixed as a documented patch file. The review text with file:line references follows.

Ranked findings follow; P1 denotes a merge blocker, P2 a functional defect, and P3 a build-hardening issue.

1. **P1 — Short `data:` MIME types cause an out-of-bounds read.**  
   [Imported data.c:204](build/external/netsurf-target/source/netsurf/content/fetchers/data.c:204) evaluates `mimetype + strlen(mimetype) - 7` without checking the length. An address such as `data:x,hello` reaches before the allocation. This is inherited from the pinned source, but the delivery exposes it and only adds a URL-size check.  
   **Smallest fix:** calculate the MIME length once and require `length >= 7` before testing/removing `;base64`. Add sanitized short-MIME cases.

2. **P1 — Structural admission occurs after the allocations and recursion it needs to constrain.**  
   [Patch:1073](sdk/netsurf/patches/0001-offline-sdk.patch:1073) counts boxes/depth only upon entering layout. Before that, [box_construct.c:1293](build/external/netsurf-target/source/netsurf/content/handlers/html/box_construct.c:1293) recursively normalizes the completed tree. A deeply nested document below the 8 MiB file cap can exhaust heap or stack before reaching the depth-64 rejection.  
   The bitmap counter also excludes libnsgif’s separately allocated restore buffer and frame metadata; 2 MiB is therefore not a complete decoder-memory bound.  
   **Smallest fix:** enforce structural limits during parsing/construction, before normalization, and bound GIF auxiliary allocations/frame counts. Exercise these paths with allocation failures before accepting 128 MiB admission.

3. **P1 — Admitted text can monopolize the UI inside the font adapter.**  
   [font.c:77](sdk/netsurf/port/font.c:77) repeatedly measures every growing prefix; position lookup does the same. An admitted 8192-character unbroken run can require roughly **33.6 million glyph-processing iterations** in one split call. The 75 ms check outside that call cannot interrupt it. Min/max layout and plain-text reformatting also remain synchronous.  
   **Smallest fix:** replace repeated-prefix measurement with bounded searches or one shared advance walk, then check deadlines inside remaining expensive operations. Record elapsed time on rejection paths too: the current maximum updates only after completed layout.

4. **P2 — DAMAGE’s RESULT is incorrectly treated as a consumption fence.**  
   [surface.c:83](sdk/netsurf/port/surface.c:83) assumes the server consumed/copied shared pixels before replying. In reality, [client.c:1553](apps/desktop/client.c:1553) queues damage, and the desktop flushes replies before rendering. The browser can overwrite the mapping while software composition reads it.  
   **Smallest fix:** agree on an actual consumption/snapshot boundary with the lead and release presentation ownership only at that boundary. Add a deferred-compositor fixture; the present unit test assumes the desired fence.

5. **P2 — Wheel events never reach page scrolling.**  
   [chrome.c:221](sdk/netsurf/port/chrome.c:221) calls `du_input()` and returns true for wheel events over content. Consequently, [nsfb_surface.c:70](sdk/netsurf/port/nsfb_surface.c:70) never invokes the surface’s wheel mapper.  
   **Smallest fix:** return false for content-owned wheel events; retain toolkit handling over chrome/dialogs. Test both directions and multiple detents through real chrome routing.

6. **P2 — Focus/capture loss can activate a link.**  
   [surface.c:159](sdk/netsurf/port/surface.c:159) synthesizes ordinary mouse-up events when releasing state. The framebuffer click handler interprets mouse-up after a press as `BROWSER_MOUSE_CLICK_1`. Losing focus while holding a link can therefore navigate instead of cancelling.  
   **Smallest fix:** introduce a cancellation path that clears framebuffer press/drag/grab state without generating a click. Test loss after button-down and during dragging.

7. **P2 — A status change during a modal dialog causes continuous full-window repainting.**  
   [chrome.c:141](sdk/netsurf/port/chrome.c:141) renders/damages the whole window and returns without clearing `status_dirty`. If loading updates status while the picker is open, every subsequent idle turn repeats the full repaint.  
   **Smallest fix:** clear the status invalidation after rendering it in the modal path, and assert that an idle dialog produces no further DAMAGE messages.

8. **P2 — Successful navigation normally bypasses persistent history recording.**  
   [chrome.c:159](sdk/netsurf/port/chrome.c:159) records visits only when the URL callback runs with `loading == false`. Normal URL updates occur at navigation start and CONTENT_READY while loading is true; CONTENT_DONE stops the throbber without another URL callback.  
   **Smallest fix:** record the committed top-level URL on successful completion, with duplicate suppression and error handling. Test navigation, restart and History.html through the frontend.

9. **P2 — File-picker typing uses US scan-code text instead of desktop Unicode TEXT.**  
   [chrome.c:212](sdk/netsurf/port/chrome.c:212) handles TEXT only for the non-modal URL entry. Modal input falls through to libdeskui, which ignores TEXT and derives characters using its US table. Italian punctuation, accented characters and AltGr input consequently differ between URL entry and picker.  
   **Smallest fix:** apply the URL entry’s KEY/TEXT separation to modal editable fields, preserving navigation and shortcuts. Test Italian layout and non-ASCII filenames.

10. **P3 — An output-cache miss can rebuild from an unverified working source tree.**  
    [build.py:205](sdk/netsurf/build.py:205) deletes the build directory for changed inputs, but not for mismatched installed outputs. [prepare():121](sdk/netsurf/build.py:121) then reuses the existing patched tree without validating it. A modified working source plus missing/stale output can produce an artifact stamped with unchanged recorded inputs.  
    **Smallest fix:** recreate the working tree on every cache miss, or validate all working source/generated inputs before incremental reuse.

**Merge verdict: request changes; fix the concrete defects and obtain green host sanitizer runs before integration. T23 responsiveness and resident-memory qualification remain open under the accepted provisional decisions.**