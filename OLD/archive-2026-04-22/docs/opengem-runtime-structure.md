# OpenGEM Runtime Structure (CiukiOS view)

## Source
Local copy of the OpenGEM Release 7 RC3 distribution (GPL-2.0) lives at:

```
third_party/freedos/runtime/OPENGEM/
```

The payload is imported via `scripts/import_opengem.sh` and mirrored into
the FAT image (`::FREEDOS/OPENGEM`) by `run_ciukios.sh` whenever
`CIUKIOS_INCLUDE_OPENGEM` is `auto` (default when the directory exists)
or `1`.

## Top-level layout
```
OPENGEM/
├── GEM.BAT          # launcher script (main entry point)
├── SETUP.BAT        # setup helper (not used by CiukiOS)
├── SETUP.OLD        # legacy setup (unused)
├── README.TXT       # upstream README
├── SOURCE.TXT       # upstream source note
├── LICENSE.TXT      # GPL-2.0 license text (must ship with the payload)
└── GEMAPPS/
    ├── GEMSYS/      # GEMVDI.EXE + VDI drivers (core runtime)
    ├── FONTS/       # bitmap fonts
    ├── HELPZONE/    # built-in help content
    ├── 2048.APP     # demo accessory
    ├── 2048.RSC
    ├── EDICON.APP   # icon editor accessory
    ├── EDICON.RSC
    ├── SYSFONT.APP  # system font viewer
    └── SYSFONT.RSC
```

## Entry points (preflight order)
`shell_run_opengem_interactive()` probes the FAT image in this order and
launches the first hit via `shell_run()`:

1. `/FREEDOS/OPENGEM/GEM.BAT`              — canonical entry
2. `/FREEDOS/OPENGEM/GEM.EXE`              — direct-EXE fallback
3. `/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/DESKTOP.APP`
4. `/FREEDOS/OPENGEM/OPENGEM.BAT`          — alternate naming
5. `/FREEDOS/OPENGEM/OPENGEM.EXE`

`GEM.BAT` itself expects `\GEMAPPS\GEMSYS\GEMVDI.EXE` to be reachable
and invokes `CD \GEMAPPS\GEMSYS` + `GEMVDI %1 %2 %3`. Under CiukiOS the
BAT interpreter layer is the limiting factor: Phase 1 provides the
launcher glue and preflight; actual BAT/MZ dispatch accuracy is tracked
by later phases of the roadmap (`docs/roadmap-opengem-ux.md`).

## CiukiOS entry surfaces
OpenGEM can be reached from three places:

1. **Shell command** — `opengem` at the `CIUKSH>` prompt.
2. **Desktop launcher** — `OPENGEM` item (seventh entry) selected with
   `ENTER` in the left dock.
3. **Desktop shortcut** — `ALT+O` while the desktop session is active.

All three converge on `shell_run_opengem_interactive()` in
`stage2/src/shell.c`, which emits the following serial markers:

| Marker                                                    | Phase                      |
|-----------------------------------------------------------|----------------------------|
| `[ app ] opengem launch requested`                        | entry                      |
| `OpenGEM: boot sequence starting`                         | entry                      |
| `[ app ] opengem preflight started`                       | preflight                  |
| `[ app ] opengem preflight entry: {ok,missing}`           | preflight — entry probe    |
| `[ app ] opengem preflight fat: {ok,fail}`                | preflight — FAT probe      |
| `[ app ] opengem preflight complete`                      | preflight end              |
| `[ app ] opengem preflight {passed,failed}`               | preflight verdict          |
| `OpenGEM: launcher window initialized`                    | runtime handoff            |
| `OpenGEM: runtime not found in FAT, fallback to shell`    | fallback (missing payload) |
| `OpenGEM: exit detected, returning to shell`              | runtime return             |
| `[ app ] opengem launch completed`                        | exit                       |
| `[ ui ] opengem dock state saved: sel=<n>`                | Phase 3 — entry snapshot   |
| `[ ui ] opengem overlay active`                           | Phase 3 — runtime handoff  |
| `[ ui ] opengem overlay dismissed, state restored`        | Phase 3 — exit / fallback  |

## Desktop state save/restore contract (OPENGEM-003)
On every invocation of `shell_run_opengem_interactive()` the helper
captures a stack-allocated snapshot of the desktop state before any
preflight probe:

```
struct {
    int  launcher_focus;   /* ui_get_launcher_focus() at entry */
    char status0[64];      /* reserved for future status restore */
    u8   valid;            /* 1 when the snapshot is populated    */
} desktop_snapshot;
```

The snapshot is restored via `ui_set_launcher_focus()` on every
return path — both the graceful fallback (preflight failure, missing
payload, FAT not ready) and the normal return after `shell_run()`.
`ui_set_launcher_focus()` clamps out-of-range indices defensively so
a hypothetical dock-item-count change cannot wedge the launcher.

The `OPENGEM` entry is rendered with a `[G]` text-mode facsimile
glyph via `ui_launcher_display_for()`. The canonical action key in
`g_launcher_items[]` remains `"OPENGEM"` — dispatch, help strings,
and existing smoke gates keep matching the canonical form.

## Fallback behavior
When any preflight check fails the helper returns 0 without invoking
`shell_run`. The desktop launcher reports `OPENGEM: n/a` in the system
window status and pushes `(opengem unavailable)` into the console ring,
while the shell prints the preflight failure and a remediation hint
(`Install: scripts/import_opengem.sh`). No panic, no state corruption.

## Out of scope for Phase 1
- Accurate BAT interpretation (Phase 2+).
- GEMVDI video driver integration with the stage2 graphics stack
  (Phase 3+).
- Mouse capture bridging between INT 33h and GEMVDI (Phase 4+).
- Customizing OpenGEM configuration files.

## Validation hooks
- Host-side smoke: `bash scripts/test_opengem_smoke.sh`
  (`make test-opengem-smoke`).
- Image-level probe: `bash scripts/check_opengem_in_image.sh`.
- Boot-log integration smoke: `bash scripts/test_opengem_integration.sh`
  (inspects a captured `stage2-boot.log`).
