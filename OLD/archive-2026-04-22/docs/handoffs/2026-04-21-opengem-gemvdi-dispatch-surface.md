# Handoff — OpenGEM GEMVDI DOS API Surface Mapped

**Date:** 2026-04-21
**Branch:** `wip/opengem-044b-real-v86-first-int`
**Latest commits:**
- `1337a4c` — shell+scripts: `gem vdi` optional arg + probe scripts
- `bb342f4` — v86_dispatch: add INT21h stubs AH=0E/19/1A/2F/3B/47/4E/4F
- HEAD — v86_dispatch: trace DS/ES + dump ASCIZ path for AH=3B/4E

## Session highlight

GEMVDI.EXE now executes its full probe under the v86 dispatcher in CiukiOS
stage2. We captured the **complete INT 21h sequence** it needs and the
**exact file it's searching for** before bailing with `No screen driver found`.

## Observed GEMVDI dispatch sequence (from `build/serial-gem.log`)

```
INT 21h AH=19                              ; get current drive
INT 21h AH=47 AL=10 DL=3                   ; get cwd of drive 3
INT 21h AH=47 AL=10 DL=3                   ; (repeat)
INT 21h AH=47 AL=10 DL=3                   ; (repeat)
INT 21h AH=1A                              ; set DTA
INT 21h AH=4E findfirst pattern="GEM.EXE"  ; current dir
INT 21h AH=3B chdir "..\GEMBOOT"           ; CDUP to GEMBOOT
INT 21h AH=4E findfirst pattern="GEM.EXE"  ; there
INT 21h AH=3B chdir "\"                    ; root
INT 21h AH=3B chdir "\"
INT 21h AH=19                              ; get drive
INT 21h AH=47 AL=10 DL=3                   ; get cwd
INT 21h AH=0E select drive DL=3            ;
INT 21h AH=3B chdir "\"
INT 21h AH=1A set DTA
INT 21h AH=4E findfirst pattern="SD*.*"    ; screen-driver probe
INT 21h AH=09 "No screen driver found\r\n"
INT 21h AH=09 "\r\nExecution terminated."
INT 21h AH=3B chdir "\"
INT 21h AH=4C exit
```

## Key insight: OpenGEM expected layout

OpenGEM/GEMVDI expects this tree **at the root of its boot drive** (C:\):

```
C:\
├── GEMBOOT\           <-- not present in our FreeDOS tree
│   └── GEM.EXE
├── SDxxx.xxx          <-- screen drivers at root, pattern "SD*.*"
└── (other files)
```

Our FreeDOS OpenGEM package instead has:

```
/FREEDOS/OPENGEM/
├── GEMAPPS/
│   └── GEMSYS/
│       ├── GEM.EXE
│       ├── GEMVDI.EXE
│       ├── MDGEM9.SYS
│       ├── SDPSC9.VGA      <-- this is the VGA driver GEMVDI wants
│       └── SETUP/VIDEO/SD*.*
```

The real OpenGEM installer normally copies one `SD*.*` file from
`SETUP/VIDEO/` to the root during configuration. In our bundle the chosen
driver is already elevated to `GEMAPPS/GEMSYS/SDPSC9.VGA` but **not to
the root**, and there is **no `GEMBOOT/` directory** — both mean GEMVDI's
searches fail.

## INT 21h handlers now wired in `stage2/src/v86_dispatch.c`

Stubs flow without `CF=1` for: AH=02, 09, 0E, 19, 1A, 25, 2F, 30, 35, 3B,
47, 48, 49, 4A, 4C, 4E, 4F. Each one traces inputs to serial for
debugging (DS/ES, eax..edx, and ASCIZ path for 3B/4E).

However AH=4E/4F still return "no more files" (AX=0x12, CF=1). That is
the sole reason GEMVDI bails. All other calls succeed.

## Diagnostics infrastructure

- `scripts/run-gemvdi-probe.sh` — 18s QEMU run with `gem vdi` autoexec,
  captures `build/serial-gem.log` + `build/debugcon.log`.
- `scripts/rebuild-and-probe-gemvdi.sh` — rebuild stage2 + image + probe.
- Rich serial tracing: every INT 21h logs vec/eax/ebx/ecx/edx/ds/es;
  AH=09/3B/4E also dump the ASCIZ string at DS:DX.

## Next session's plan (multi-step)

1. **Wire AH=4E/4F to real FAT search**. Stage2 already has
   `fat_findfirst`/`fat_findnext` (see `[compat] INT21h file search
   ready` self-test). Hook them into `v86_dispatch_int` so GEMVDI's
   DTA gets a real 43-byte DOS findfirst record.
2. **Track per-guest cwd**. AH=3B should update a stage2-side cwd, and
   AH=4E must resolve the pattern relative to that cwd against the
   virtual FAT tree (which is the FreeDOS image mount).
3. **Reconcile OpenGEM layout**. Either:
   - at image build time, copy `SDPSC9.VGA` and create `GEMBOOT/GEM.EXE`
     at the root of the FreeDOS partition, OR
   - implement a synthetic mapping in stage2 so `C:\SD*.*` resolves to
     `C:\FREEDOS\OPENGEM\GEMAPPS\GEMSYS\SDPSC9.VGA`.
4. **Wire AH=3D/3F/40/3E** (open/read/write/close) — GEMVDI will need to
   read the driver file after findfirst succeeds.
5. **Set video mode** — GEMVDI (and the driver it loads) will issue INT
   10h to switch to graphics mode. Our v86 dispatcher only handles
   INT 21h today; INT 10h needs to route through stage2's BIOS int10
   compat layer.
6. **TSR chain to GEM.EXE** — after GEMVDI loads the driver and goes
   resident, the DOS shell should continue with GEM.EXE via the BAT.

## Stable invariants

- 32-bit IA-32 + legacy BIOS/MBR (permanent).
- Version: `Alpha v0.8.9` (no bump yet).
- Main worktree: `/home/peronslayer/Desktop/CiukiOS`.
- Merge worktree: `/home/peronslayer/Desktop/CiukiOS-merge-main`.
- `docs/handoffs/` is gitignored — always `git add -f`.
