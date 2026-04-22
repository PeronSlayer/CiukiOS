# OpenGEM payload for full image

The full build can inject OpenGEM files into the FAT16 root automatically.

Recommended files in this folder:

- `GEM.EXE` (launcher target used by `opengem` command)
- `GEM.BAT`
- `GEMVDI.EXE`
- `DESKTOP.APP`
- `OUTPUT.APP`
- `SETTINGS.APP`
- `CTMOUSE.EXE`

The build also looks in:

- `assets/full/opengem/upstream/OPENGEM7-RC3`

and copies known files from there if they are not already present at top level.

Build command:

```bash
bash scripts/build_full.sh
```

Run command inside CiukiOS full shell:

```text
opengem
```
