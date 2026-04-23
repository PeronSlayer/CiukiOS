# OpenGEM Runtime Normalization (OG-P1-04)

Date: 2026-04-24  
Scope: normalize launch order, payload requirements, and troubleshooting signatures for full profile.

## Effective Launch Order (Full Profile)

1. BIOS stage0 -> stage1 (`[BOOT0-FULL]`, `[STAGE1-SERIAL]`).
2. Stage1 initializes S2 services (`[S2] init`, `[S2] ready`).
3. Stage1 autorun loads stage2 payload (`[S2] autorun`, `[S2] stage2 loaded`).
4. Stage2 OpenGEM chain:
   - `[OPENGEM] launch`
   - `CTMOUSE.EXE` attempt
   - `[OPENGEM] try GEMVDI`
   - `[OPENGEM] try GEM.EXE`
   - `[OPENGEM] try GEM.BAT` (final fallback)
   - success marker: `[OPENGEM] returned`
   - failure marker: `[OPENGEM] launch failed AX=....`

## Payload Requirements

Mandatory for full desktop path:
1. `GEMVDI.EXE`
2. `GEM.EXE` (preferred launch target)
3. Core GEM runtime payload files under `root` and/or `GEMAPPS/GEMSYS`

Optional but recommended:
1. `CTMOUSE.EXE` for mouse services
2. `GEM.BAT` as final fallback launch script

## Troubleshooting Signatures

1. Signature: `[OPENGEM] launch failed AX=0002`
   Meaning: target executable missing in current launch step.
   Action: verify payload injection in full image and file naming.

2. Signature: launch markers present, no `[OPENGEM] returned`, repeated timeout
   Meaning: probable hang inside GEM runtime chain.
   Action: run acceptance + soak reports, inspect per-run serial logs.

3. Signature: `[IERR] 4F:12`
   Meaning: DOS find-next exhausted/not found in current probe path.
   Action: verify driver/pattern payload layout and search expectations.

4. Signature: serial log empty in gate
   Meaning: infrastructure serial capture issue.
   Action: re-run with explicit `-chardev file ... -serial chardev:...` and archive diagnostics.

## Reproducible Commands

1. Trace:
   `make opengem-trace-full`
2. Acceptance:
   `make opengem-acceptance-full`
3. Soak 20 minutes:
   `make opengem-soak-full`
4. Hardware lane pack:
   `make opengem-hardware-lane-pack`
