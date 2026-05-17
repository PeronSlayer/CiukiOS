# Controlled DOOM Audio Lane - 2026-05-17

## Decision

Stop reverse-stepping the original packaged `DOOM.EXE` for now.

Best full-port candidate: `doom-vanille`.

Why:

- GPL-2.0 DOS source port.
- Based on PCDoom/PCDoom2.
- Build target is Open Watcom/DOS.
- Keeps vanilla IWAD compatibility.
- Has public DMX-like support files, unlike the proprietary original DMX path.

First implementation slice:

Use a smaller controlled WAD/SB16 harness before importing a full port.

`DOOMSFX.COM` launches `DOS4GW.EXE DOOMSFX.LE`; `DOOMSFX.LE` loads
`\APPS\DOOM\DOOM.WAD`, extracts a supported sound lump, and plays it through
SB16 8-bit DMA with protected-mode IRQ markers.

Default lump:

- `DSPISTOL`

Supported explicit lump arguments:

- `DSPISTOL`
- `DSDOROPN`
- `DSITEMUP`

## Files

- `src/probes/doomsfx/doomsfx.c`
- `src/com/doomsfx_launch.asm`
- `scripts/build_doomsfx_dos4gw.sh`
- `scripts/build_full.sh`
- `Makefile`

Packaged path:

- `\APPS\DOOMAUD\DOOMSFX.COM`
- `\APPS\DOOMAUD\DOOMSFX.LE`

## Validation

Controlled lane:

```text
make qemu-test-full-doomsfx
```

Door-open validation:

```text
DOOMSFX_LUMP=DSDOROPN make qemu-test-full-doomsfx
make qemu-test-full-doomsfx-dsdoropn
```

Manual examples:

```text
DOOMSFX
DOOMSFX DSDOROPN
DOOMSFX DSITEMUP
```

Required markers:

```text
[DOOMSFX] DSP OK
[DOOMSFX] WAD OK
[DOOMSFX] LUMP DSPISTOL
[DOOMSFX] DMA START
[DOOMSFX] TIMER HIT
[DOOMSFX] IRQ HIT
[DOOMSFX] PASS
```

Original lane guard:

```text
make qemu-test-full-doom-taxonomy
```

This must keep the original `DOOM.EXE` visual/gameplay lane unchanged.
