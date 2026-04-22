# OpenGEM GUI — Operations Notes

## Overview
OpenGEM is an optional DOS GUI application from the FreeDOS ecosystem (FreeGEM distribution).
It is integrated as an optional runtime payload — CiukiOS does NOT depend on it.

This integration does NOT replace the CiukiOS native GUI roadmap.

## Import Flow

### 1. Obtain the OpenGEM package
The default location is:
```
third_party/freedos/sources/opengem/opengem.zip
```
Or download `opengem.zip` from the FreeDOS 1.3 repository.

### 2. Run the import script
```bash
# Using default zip location:
./scripts/import_opengem.sh

# Or specify a zip explicitly:
./scripts/import_opengem.sh --zip /path/to/opengem.zip

# Or from an extracted directory:
./scripts/import_opengem.sh --source /path/to/extracted/dir
```

This copies OpenGEM files to `third_party/freedos/runtime/OPENGEM/` and updates `manifest.csv`.

### 3. Run CiukiOS with OpenGEM
```bash
./run_ciukios.sh
# or explicitly:
CIUKIOS_INCLUDE_OPENGEM=1 ./run_ciukios.sh
```

OpenGEM files are placed at `A:\FREEDOS\OPENGEM\` on the disk image.

### 4. Launch OpenGEM from shell
At the CiukiOS shell prompt:
```
C:\> opengem
```

## Expected Runtime Paths
| Path | Description |
|------|-------------|
| `third_party/freedos/runtime/OPENGEM/` | Local import directory |
| `A:\FREEDOS\OPENGEM\GEM.BAT` | Disk image launch entry |
| `/FREEDOS/OPENGEM/GEM.BAT` | FAT path (internal) |

## Environment Variables
| Variable | Default | Description |
|----------|---------|-------------|
| `CIUKIOS_INCLUDE_OPENGEM` | `auto` | `auto`: include if files exist; `1`: force include; `0`: skip |
| `CIUKIOS_REQUIRE_OPENGEM` | `0` | `1`: pipeline validation fails if OpenGEM missing |

## Troubleshooting Markers (Serial Log)

### Preflight markers
| Marker | Meaning |
|--------|---------|
| `[ app ] opengem launch requested` | opengem command was invoked |
| `[ app ] opengem preflight started` | Preflight checks beginning |
| `[ app ] opengem preflight entry: ok` | Launch entry found on FAT |
| `[ app ] opengem preflight entry: missing` | Launch entry not found |
| `[ app ] opengem preflight fat: ok` | FAT layer ready |
| `[ app ] opengem preflight fat: fail` | FAT not initialized |
| `[ app ] opengem preflight passed` | All checks passed, launching |
| `[ app ] opengem preflight failed` | Launch aborted |
| `[ app ] opengem launch completed` | Execution returned from OpenGEM |

### Recovery steps
1. **"OpenGEM entry: NOT FOUND"** — Run the import script, then rebuild the image.
2. **"FAT layer: NOT READY"** — Disk initialization failed; check QEMU image and boot log.
3. **Preflight passes but OpenGEM crashes** — OpenGEM requires 16-bit real-mode DOS APIs that may not yet be fully implemented in CiukiOS. Check INT 21h compatibility baseline and GEM.BAT batch processing support.

## Scripts Reference
| Script | Purpose |
|--------|---------|
| `scripts/import_opengem.sh` | Import OpenGEM files into runtime bundle |
| `scripts/test_opengem_integration.sh` | Smoke test for OpenGEM integration |
| `scripts/check_opengem_in_image.sh` | Verify OpenGEM files in disk image |
| `scripts/validate_freedos_pipeline.sh` | Pipeline validation (includes OpenGEM check) |

## Related Documentation
- `docs/opengem-integration-notes.md` — Provenance and package identity
- `docs/freedos-integration-policy.md` — Licensing and redistribution policy
- `docs/legal/freedos-licenses/opengem-license.txt` — License notice
- `third_party/freedos/manifest.csv` — Import manifest with checksums
