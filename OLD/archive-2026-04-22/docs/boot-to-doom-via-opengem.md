# Boot-to-DOOM via OpenGEM

Reference flow for staging a user-supplied, shareware DOOM binary so
that CiukiOS Alpha discovers it through the app catalog
(OPENGEM-004), launches it via OpenGEM (OPENGEM-001/-003), and
routes input through the hardened OpenGEM session bridge
(OPENGEM-005).

> **Licensing:** DOOM shareware binaries/WADs are **user-supplied**.
> The CiukiOS project does **not** redistribute `DOOM.EXE`,
> `DOOM1.WAD`, or any derivative. Fixtures must live outside the
> repository; `scripts/test_doom_via_opengem.sh` discovers them via
> `CIUKIOS_DOOM_FIXTURES_DIR`.

## Flow diagram

```
  [ UEFI loader ]
        |
        v
  [ stage2 ] --- FAT mount ---> /GAMES/DOOM/DOOM.EXE (user-supplied)
        |                       /GAMES/DOOM/DOOM1.WAD
        |
        |   app_catalog_init(handoff)      (OPENGEM-004)
        |   --> scans /, /FREEDOS, /FREEDOS/OPENGEM, /EFI/CiukiOS
        |   --> merges handoff->com_entries[]
        |
        |   DOOM readiness probe          (OPENGEM-006)
        |   --> [ doom ] catalog discovered DOOM.EXE at <path>
        |   --> [ doom ] catalog discovered DOOM1.WAD at <path>
        |
        v
  [ desktop ] --- ALT+G+Q ------+
        |                       |
  user picks OPENGEM (dock)     |
  or  types `opengem`           |
        |                       |
        v                       |
  [ shell_run_opengem_interactive ]
        |
        |   save launcher snapshot          (OPENGEM-003)
        |   stage2_mouse_opengem_session_enter()    (OPENGEM-005)
        |   --> [ mouse ] opengem session: cursor disabled
        |
        |   shell_run() dispatches GEM.BAT
        |   --> BAT interpreter             (OPENGEM-002)
        |   --> GEMVDI / launcher window
        |
        v
  user selects DOOM.EXE from OpenGEM
        |
        |   shell_run_from_fat("DOOM.EXE")
        |   --> [ doom ] opengem launch DOOM.EXE
        |
        v
  [ DOOM session ]                        (fixture-dependent)
        |
        |   DOS/4GW extender bootstrap (M6 DPMI smokes)
        |   mode 13h VGA                  (VIDEO-*)
        |   sound blaster / AdLib (optional, gap)
        |
        v
  [ doom ] stage reached: menu            (expected runtime marker)
        |
  user exits DOOM (Q/Y)
        |
        v
  [ shell ] -> stage2_mouse_opengem_session_exit()   (OPENGEM-005)
           -> [ mouse ] opengem session: cursor restored
           -> restore launcher snapshot             (OPENGEM-003)
           -> desktop
```

## Prerequisites

1. User obtains shareware DOOM (`DOOM.EXE` + `DOOM1.WAD`, and the
   companion `DOS4GW.EXE` if the shareware variant requires it).
2. User stages them under `FREEDOS_IMAGE_ROOT/GAMES/DOOM/` **before**
   building the image (so they land on the FAT volume).
3. For the test harness: `export CIUKIOS_DOOM_FIXTURES_DIR=<dir>`
   pointing at the staging directory; optionally set
   `CIUKIOS_DOOM_BOOT_LOG` to the boot log captured from a QEMU
   run.

## Expected boot-log markers (runtime)

```
[ catalog ] scan begin root=/
[ catalog ] scan entry DOOM.EXE kind=exe path=/GAMES/DOOM/DOOM.EXE
[ catalog ] scan entry DOOM1.WAD kind=... path=/GAMES/DOOM/DOOM1.WAD
[ catalog ] scan done entries=<n> roots=<m>
[ doom ] catalog discovered DOOM.EXE at /GAMES/DOOM/DOOM.EXE
[ doom ] catalog discovered DOOM1.WAD at /GAMES/DOOM/DOOM1.WAD
[ ui ] opengem dock state saved: sel=<n>
[ ui ] opengem overlay active
[ mouse ] opengem session: cursor disabled
[ app ] opengem preflight passed
[ bat ] enter gem.bat
[ bat ] line 1 of gem.bat: <content>
...
[ doom ] opengem launch DOOM.EXE
[ doom ] stage reached: menu
[ mouse ] opengem session: cursor restored
[ ui ] opengem overlay dismissed, state restored
```

## Compatibility gap list (as of Phase 6 landing)

| Gap | Impact on DOOM | Tracking |
|-----|----------------|----------|
| DOS/4GW / DPMI extender bootstrap incomplete | DOOM client cannot attach to extender without the full M6 DPMI path | M6 DPMI smokes (`com/m6_dpmi_*/`) |
| Sound Blaster / AdLib emulation absent | DOOM falls back to silent | Out-of-scope (future audio milestone) |
| VGA mode 13h scrolling / double-buffer corners | DOOM blits OK; scrollers may tear | `docs/copilot-task-codex-sr-video-003.md` |
| `INT 33h` event-driven mode (sub-function 0x0C) | Mouse polling works; event callbacks may miss | INT 33h hook path (OPENGEM-005) provides structure; needs real consumer |
| FAT write-through limits | DOOM savegames land on cache; persistence requires explicit flush | FAT subsystem roadmap |

## Running the harness

```
# Static-only (no fixtures):
make test-doom-via-opengem

# Full run (user-supplied fixtures + captured boot log):
export CIUKIOS_DOOM_FIXTURES_DIR=/absolute/path/to/doom/fixtures
export CIUKIOS_DOOM_BOOT_LOG=/absolute/path/to/doom-boot.log
make test-doom-via-opengem
```

The harness PASSes when either:
- `CIUKIOS_DOOM_FIXTURES_DIR` is unset and all static invariants hold, or
- fixtures + boot log are provided and all runtime markers are found.
