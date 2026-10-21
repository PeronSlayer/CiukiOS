# f6-13 — NetSurf 3.11 port plan: dependency audit, fetcher on native sockets with BearSSL, framebuffer frontend on the CiukiOS surface, build recipe (document only; contract gate "f6-04: browser port", first half)

- **Owner:** Codex, `gpt-6.1-sol`, effort `high`, `--search` allowed (NetSurf documentation and the pinned sources, BearSSL documentation, the F6 contract)
- **Written:** 2026-10-21 by the lead (owner decision 2026-10-20: the browser is NetSurf; dev diary 2026-10-20-08)
- **Worktree:** `../CiukiOS-wt-f6-13`, branch `wt/f6-13`
- **Pinned sources:** `config/netsurf.json` → `build/downloads/netsurf/netsurf-all-3.11.tar.gz` (SHA-256 `4dea880f…44b7b`); BearSSL already in `third_party/bearssl`; the CA bundle in `third_party/cacert`. Read-only in this directive: no extraction into the tree, no code.
- **Contract:** `docs/design/network-contract.md`, gate "f6-04: browser port" ("lead-selected browser/version/license/backend and exact API dependency list before code"); the native socket ABI of f6-10 (calls 78–90, SDK headers) as the network boundary; the F2 POSIX subset and the ported newlib as the libc; `apps/libdeskui` and the desktop v3 protocol as the window/input/surface boundary.

## Required (one document, `docs/design/netsurf-port-plan.md`, English)

1. **Dependency audit** of `netsurf-all-3.11`: every component (netsurf core, libcss, libhubbub, libdom, libparserutils, libwapcaplet, libnsutils, libutf8proc, libnsgif, libnsbmp, libsvgtiny, librosprite, libnsfb, buildsystem, nsgenbin) with licence, size, build requirements (perl/python generators, gperf, pkg-config), and the exact libc/POSIX surface it uses (`nm`/`grep` evidence against the F2 subset and newlib: threads, `dlopen`, locales, `iconv`, `select`/`poll`, files, time, signals); a table "available in CiukiOS / missing / to stub / to avoid".
2. **Images and fonts:** what NetSurf needs for GIF/BMP/PNG/JPEG (libpng/zlib/libjpeg are not in the pinned archive: list what to pin, their licences and sizes, or which formats to disable for the first milestone) and fonts for the framebuffer frontend (libnsfb's internal bitmap fonts versus FreeType through the existing SDK font bridge).
3. **Network:** NetSurf's fetcher layer (`content/fetchers/`): the curl fetcher versus a **CiukiOS fetcher** on the native socket ABI with HTTP/1.1 (keep-alive, chunked, redirects, cookies through NetSurf's own cookie code), gzip via zlib, and HTTPS through BearSSL with the pinned CA bundle and the system clock; decision with reasons (the lead's preference: the CiukiOS fetcher, no libcurl/OpenSSL port); the exact fetcher API surface to implement; DNS through `resolve4`; `file:` and `data:` fetchers kept.
4. **Frontend:** `frontends/framebuffer` on libnsfb with a new libnsfb surface backend over the CiukiOS v3 window (surface mapping, damage, keyboard and pointer events from the desktop protocol, clipboard), window chrome from libdeskui where reasonable; the URL bar, back/forward/reload/stop, bookmarks file, history; JavaScript disabled (Duktape off) for the first milestone with the switch documented.
5. **Build recipe:** how the pinned archive is built by the SDK toolchain (`ciuki-cc`, newlib) into `/Programs/netsurf` with its resources, deterministic, cached like FreeType/TinyGL; the make/buildsystem adaptations needed (cross environment variables, generators run on the host); memory footprint target for the T23 (process budget, cache sizes).
6. **Milestones and acceptance** aligned with the private site list the lead will supply at code time: M1 `file:` and `http:` with HTML4/CSS2 rendering and keyboard navigation; M2 `https:` with BearSSL; M3 images and cookies; M4 bookmarks/history and the desktop integration; for each the host tests and the QEMU/T23 evidence the lead will run.
7. **Open decisions for the lead**, listed explicitly.

## Constraints

Document only (plus the directive index row is the lead's): no extraction into tracked paths, no code, no builds; `rg`/`sed` only; no commits, no diary edits; English. Report `build/codex/f6-13-report.md`.
