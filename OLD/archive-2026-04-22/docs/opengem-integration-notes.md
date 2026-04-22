# OpenGEM Integration Notes

## Package Identity
- **Name**: OpenGEM (FreeGEM distribution)
- **Version**: Release 7 RC3 (9th July 2017)
- **Author**: Copyright (C) 2001-2017 Shane Coughlan
- **License**: GPL-2.0-or-later
- **FreeDOS package name**: `opengem`
- **Source URL**: `https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/gui/opengem.zip`

## Description
OpenGEM is a DOS GUI application based on FreeGEM/GEM Desktop. It provides a graphical desktop environment for DOS systems with 3D windows, file management, and application support.

## Archive Layout
```
opengem.zip
├── APPINFO/OPENGEM.LSM    (package metadata)
├── GUI/OPENGEM/
│   ├── GEM.BAT            (launch entry)
│   ├── LICENSE.TXT         (GPL-2.0)
│   ├── README.TXT
│   ├── SETUP.BAT
│   └── GEMAPPS/
│       ├── GEMSYS/         (core: GEM.EXE, GEMVDI.EXE, DESKTOP.APP)
│       ├── FONTS/          (screen fonts)
│       ├── HELPZONE/       (help docs)
│       └── ...
└── SOURCE/OPENGEM/SOURCES.ZIP (18MB source archive)
```

## Launch Entry
Primary: `GEM.BAT`
This batch file sets up the environment and invokes `GEM.EXE` through the GEMSYS subsystem.

## Runtime Paths in CiukiOS
| Path | Description |
|------|-------------|
| `third_party/freedos/runtime/OPENGEM/` | Local import directory |
| `A:\FREEDOS\OPENGEM\` | Disk image location |
| `/FREEDOS/OPENGEM/GEM.BAT` | FAT path (launch entry) |
| `/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/GEM.EXE` | FAT path (main executable) |

## Trust Assumptions
1. Downloaded from official FreeDOS 1.3 repository at ibiblio.org.
2. Package is GPL-2.0 licensed; LICENSE.TXT included in archive.
3. Source code available in SOURCE/OPENGEM/SOURCES.ZIP within the archive.
4. Not a CiukiOS dependency — optional GUI payload only.

## Redistribution Notes
1. OpenGEM is GPL-2.0-or-later — redistribution is permitted with source availability.
2. Source archive is included in the package itself (`SOURCE/OPENGEM/SOURCES.ZIP`).
3. CiukiOS does NOT redistribute OpenGEM by default — user must supply the archive.
4. If redistributing in a disk image, include license notice and source access path.
