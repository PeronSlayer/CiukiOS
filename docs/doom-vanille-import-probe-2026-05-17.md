# doom-vanille Import Probe - 2026-05-17

> Historical import/build report. Supersession note (2026-09-01): the original `000B`, `I_AllocLow(256000)`, wall-rendering, and silent-SB16 results described below are superseded. Current `full` gates pass the low-DOS request, healthy gameplay/HUD/walls, simultaneous OPL2 music plus SB16 SFX, combined DMX return 10, objective mixed output, and real-time timedemo performance. Audio-enabled clean exit and full-CD remain unproven. The dated results below are retained as historical evidence rather than rewritten.

## Decision

Candidate: `doom-vanille`.

Reason:

- GPL-2.0 DOS source port.
- OpenWatcom/DOS oriented.
- IWAD vanilla compatible.
- Based on PCDoom/PCDoom2.
- Contains public DMX-like/audio support files, avoiding more reverse work on
  the proprietary original DOS `DOOM.EXE`/DMX path.

Reference:

- https://github.com/AXDOOMER/doom-vanille

## Import Policy

Do not commit the upstream tree yet.

Use an external checkout under:

```text
build/external/doom-vanille
```

This is safer for this slice because:

- `build/` is already ignored.
- Upstream contains many source files plus binary/library artifacts such as
  `AUDIO_WF.LIB`, `PCFX.OBJ`, and `AudioLib.zip`.
- Keeping the checkout external avoids repo-size and binary-policy churn until
  the build is proven.
- Licensing remains clear: upstream is GPL-2.0, but any future committed import
  must preserve license files and source attribution.

Future tracked import path, if approved:

```text
third_party/source/doom-vanille/
```

## Observed Build Requirements

From upstream README:

- Open Watcom C 1.9.
- Open `pcdoom.wpj` in Watcom.
- Build `pcdoom.exe`.
- Copy `pcdoom.exe` to the Doom folder.

Observed project files:

- `pcdoom.wpj`
- `pcdoom.tgt`
- output target: `pcdoom.exe`
- audio/backend-related files include `DMX.C`, `DMX.H`, `I_SOUND.C`,
  `AUDIO_WF.LIB`, `PCFX.OBJ`, `DPMIAPI.H`, `TASK_MAN.H`, `FX_MAN.H`,
  `PCFX.H`, `SNDCARDS.H`, and `USRHOOKS.C`.

Expected assets:

- A compatible Doom IWAD such as `DOOM.WAD`, provided separately by the user.
- No IWAD should be committed.

DOS extender/runtime:

- The upstream project is DOS/OpenWatcom-oriented. The exact extender/runtime
  output behavior must be confirmed by the build probe before packaging.

## Probe Script

Added:

```text
scripts/build_doom_vanille_probe.sh
make build-doom-vanille-probe
```

Markers:

```text
[DOOMVANILLE] SOURCE MISSING
[DOOMVANILLE] TOOLCHAIN MISSING
[DOOMVANILLE] BUILD START
[DOOMVANILLE] BUILD PASS
[DOOMVANILLE] BUILD FAIL
```

The probe is fail-open for missing source/toolchain and does not alter the full
image.

With an external checkout present, the probe builds manually with `wcl386`
rather than using `pcdoom.tgt` directly. The `.tgt` file is a Watcom IDE project
file, not a modern `wmake` makefile.

Linux/OpenWatcom 2 compatibility notes handled by the probe:

- Creates lowercase symlinks inside the ignored external checkout, because the
  upstream tree has uppercase headers/libraries while source includes lowercase
  names.
- Uses lowercase `pcfx.obj` and `audio_wf.lib` symlinks so `wcl386` treats them
  as link inputs.
- Applies an ignored local compatibility edit in `mus2mid.c`, changing the
  `mus2mid` function return from `boolean` to `int` to match `mus2mid.h`.

Observed build result:

```text
[DOOMVANILLE] BUILD START source=build/external/doom-vanille
[DOOMVANILLE] BUILD PASS
```

Output:

```text
build/external/doom-vanille/pcdoom.exe
```

The file is an MS-DOS LE executable. It is packaged only when the ignored
external build output exists.

## Current Guardrails

- Do not touch original `third_party/Doom/DOOM.EXE`.
- Do not change the working `DOOMSFX` lane.
- Keep doom-vanille isolated under `\APPS\DOOMVAN`.
- Do not test gameplay/audio until startup is proven stable.

## Next Slice

1. Clone upstream into `build/external/doom-vanille`.
2. Run `make build-doom-vanille-probe`.
3. Keep the built executable isolated under `\APPS\DOOMVAN`.
4. Extend the separate taxonomy lane from startup to gameplay.
5. Only after gameplay is stable, validate SB16 SFX.

## Isolated Package Probe

Added isolated packaging:

```text
\APPS\DOOMVAN\PCDOOM.EXE
\APPS\DOOMVAN\DOOM.WAD
\APPS\DOOMVAN\DEFAULT.CFG
\APPS\DOOMVAN\DOOMVAN.COM
```

Package result: PASS. The files are copied into the full image when the ignored
external `pcdoom.exe` build output is present.

Added taxonomy target:

```text
make qemu-test-full-doomvan-taxonomy
```

Initial direct-launch result:

- `PCDOOM.EXE` is found in `\APPS\DOOMVAN`.
- The shell sends `cd \APPS\DOOMVAN; run PCDOOM.EXE -nosound -nomusic`.
- CiukiOS reboots immediately after the exec attempt.
- No MZ transfer marker is observed.
- No extender/video/runtime/audio evidence is reached yet.

Resolution:

- `PCDOOM.EXE` is an LE payload-style OpenWatcom/DOS4G executable, not the same
  bound shape as the original packaged `DOOM.EXE`.
- Added `DOOMVAN.COM`, a tiny isolated launcher that executes
  `\SYSTEM\DRIVERS\DOS4GW.EXE \APPS\DOOMVAN\PCDOOM.EXE ...`.
- The taxonomy target now runs `run DOOMVAN.COM -nosound -nomusic`.

Superseded startup result:

```text
make qemu-test-full-doomvan-taxonomy
```

The first generic taxonomy target reported `runtime_stable`, but that was too
weak: it accepted launcher text as an app marker and did not prove the extender
or video path.

After tightening the lane to `dosapp`/visual evidence, the real blocker is:

- Direct `run PCDOOM.EXE -nosound -nomusic` from `\APPS\DOOMVAN` reboots
  CiukiOS before DOS/4GW or DOOM startup markers.
- Copying `DOS4GW.EXE` beside `PCDOOM.EXE` does not change that.
- A `DOOMVAN.COM` launcher that calls `INT 21h AX=4B00` for `DOS4GW.EXE`
  fails with DOS error `000B` (invalid executable format), even when
  `DOS4GW.EXE` is local to `\APPS\DOOMVAN`.
- A CauseWay-linked variant also reboots before visual evidence.

Current validated result:

```text
make qemu-test-full-doomvan-taxonomy
```

Fails before extender/video:

```text
binary_found=PASS
wad_found=PASS
dosapp_exec_attempted=PASS
visual_gameplay=DEFERRED
result=FAIL
```

This is not a doom-vanille gameplay failure and not an SB16 audio failure yet.
The executable has not reached visual/menu/gameplay under CiukiOS.

This remains isolated from:

- original `\APPS\DOOM\DOOM.EXE`
- controlled `\APPS\DOOMAUD\DOOMSFX.COM`
- Stage1 behavior outside the existing MZ execution path

Next narrow target:

Fix the CiukiOS DOS/4G launch path for OpenWatcom-generated protected-mode
executables, or produce a doom-vanille executable shape that the existing
Stage1 MZ path can run without rebooting. Only after this reaches visual/menu
or gameplay should SB16 SFX be enabled.
