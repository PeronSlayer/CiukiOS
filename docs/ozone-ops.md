# oZone GUI — Operations Notes

## Overview
oZone is an optional DOS GUI application from the FreeDOS ecosystem.
It is integrated as an optional runtime payload — CiukiOS does NOT depend on it.

This integration does NOT replace the CiukiOS native GUI roadmap.

## Import Flow

### 1. Obtain the oZone package
Download `ozonegui.zip` from the FreeDOS 1.3 repository:
```
https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/gui/ozonegui.zip
```

### 2. Extract the archive
```bash
mkdir -p /tmp/ozone-extract
unzip ozonegui.zip -d /tmp/ozone-extract
```

### 3. Run the import script
```bash
./scripts/import_ozonegui.sh --source /tmp/ozone-extract
```

This copies oZone files to `third_party/freedos/runtime/OZONE/` and updates `manifest.csv`.

### 4. Run CiukiOS with oZone
```bash
./run_ciukios.sh
# or explicitly:
CIUKIOS_INCLUDE_OZONE=1 ./run_ciukios.sh
```

oZone files are placed at `A:\FREEDOS\OZONE\` on the disk image.

### 5. Launch oZone from shell
At the CiukiOS shell prompt:
```
C:\> ozone
```

## Expected Runtime Paths
| Path | Description |
|------|-------------|
| `third_party/freedos/runtime/OZONE/` | Local import directory |
| `A:\FREEDOS\OZONE\OZONE.EXE` | Disk image location |
| `/FREEDOS/OZONE/OZONE.EXE` | FAT path (internal) |

## Environment Variables
| Variable | Default | Description |
|----------|---------|-------------|
| `CIUKIOS_INCLUDE_OZONE` | `auto` | `auto`: include if files exist; `1`: force include; `0`: skip |
| `CIUKIOS_REQUIRE_OZONE` | `0` | `1`: pipeline validation fails if oZone missing |

## Troubleshooting Markers (Serial Log)

### Preflight markers
| Marker | Meaning |
|--------|---------|
| `[ app ] ozone launch requested` | ozone command was invoked |
| `[ app ] ozone preflight started` | Preflight checks beginning |
| `[ app ] ozone preflight exe: ok` | OZONE.EXE found on FAT |
| `[ app ] ozone preflight exe: missing` | OZONE.EXE not found |
| `[ app ] ozone preflight fat: ok` | FAT layer ready |
| `[ app ] ozone preflight fat: fail` | FAT not initialized |
| `[ app ] ozone preflight passed` | All checks passed, launching |
| `[ app ] ozone preflight failed` | Launch aborted |
| `[ app ] ozone launch completed` | Execution returned from oZone |

### Recovery steps
1. **"OZONE.EXE: NOT FOUND"** — Run the import script, then rebuild the image.
2. **"FAT layer: NOT READY"** — Disk initialization failed; check QEMU image and boot log.
3. **Preflight passes but oZone crashes** — oZone requires 16-bit real-mode DOS APIs that may not yet be fully implemented in CiukiOS. Check INT 21h compatibility baseline.

## Scripts Reference
| Script | Purpose |
|--------|---------|
| `scripts/import_ozonegui.sh` | Import oZone files into runtime bundle |
| `scripts/test_ozone_integration.sh` | Smoke test for oZone integration |
| `scripts/validate_freedos_pipeline.sh` | Pipeline validation (includes oZone check) |

## Related Documentation
- `docs/ozone-integration-notes.md` — Provenance and package identity
- `docs/freedos-integration-policy.md` — Licensing and redistribution policy
- `docs/legal/freedos-licenses/ozonegui-license.txt` — License notice
- `third_party/freedos/manifest.csv` — Import manifest with checksums
