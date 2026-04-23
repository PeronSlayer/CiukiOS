# macOS 26 Build Fix - 2026-04-23

## Status
✅ **COMPLETE** - All builds and QEMU tests passing on macOS 26 Intel

## Problem Fixed
The original macOS wrapper scripts had an incompleteness in handling GNU `stat` compatibility:
- Linux build scripts use `stat -c%s` (GNU stat syntax)
- macOS native `stat` uses `stat -f%z` (BSD stat syntax)
- The previous wrapper attempted to use function-based aliasing which doesn't persist through subshell execution

## Solution Implemented

### 1. Enhanced [scripts/macos/common.sh](scripts/macos/common.sh)
Created a robust wrapper system that:
- Generates a temporary bin directory (`$TMPDIR/ciukios-bin-$$`) at front of PATH
- Creates real executable scripts (not bash functions) for:
  - **`stat`** - Translates GNU format flags (`-c%s`, `-c%Y`, etc.) to macOS format (`-f%z`, `-f%m`)
  - **`timeout`** - Delegates to `gtimeout` from Homebrew coreutils
- Properly exports all GNU tool paths from Homebrew installations

### 2. `stat` Format Translation
Implemented these mappings:
```
-c%s  → -f%z   (file size)
-c%Y  → -f%m   (modification time in seconds)
-c%X  → -f%a   (access time)
-c%Z  → -f%c   (change time)
-c%U  → -f%u   (user id)
-c%G  → -f%g   (group id)
-c%a  → -f%A   (permissions symbolic)
-c%n  → -f%N   (filename)
```

## Build Results

### Setup
```bash
bash scripts/macos/setup_macos26.sh --install
```

Installed packages:
- ✅ nasm (3.01)
- ✅ qemu (11.0.0)
- ✅ mtools (4.0.49)
- ✅ coreutils (9.11)
- ✅ gnu-sed (4.10)
- ✅ findutils (4.10.0)
- ✅ gawk (5.4.0)

### Build Artifacts
```
build/floppy/ciukios-floppy.img    1.4 MB  ✅ PASS smoke test
build/full/ciukios-full.img        128 MB  ✅ PASS smoke test (with OpenGEM)
```

### Smoke Tests
```bash
bash scripts/macos/build_full_macos.sh          # ✅ PASS
bash scripts/macos/build_floppy_macos.sh        # ✅ PASS
bash scripts/macos/qemu_run_full_macos.sh       # ✅ PASS (stage0/stage1 detected)
bash scripts/macos/qemu_run_floppy_macos.sh     # ✅ PASS (stage0/stage1 detected)
bash scripts/macos/qemu_test_all_macos.sh       # ✅ PASS (all tests combined)
```

## Daily Workflow on macOS 26

### Build only
```bash
bash scripts/macos/build_full_macos.sh
bash scripts/macos/build_floppy_macos.sh
```

### Build + test
```bash
bash scripts/macos/qemu_run_full_macos.sh --test
bash scripts/macos/qemu_run_floppy_macos.sh --test
```

### Full combined workflow
```bash
bash scripts/macos/qemu_test_all_macos.sh
```

## Technical Details

### Wrapper Script Architecture
1. Each wrapper is a real executable file in a temporary directory
2. Temporary directory is prepended to PATH to shadow native commands
3. Wrappers parse arguments and translate to macOS-compatible equivalents
4. This approach persists through subshell invocations via `exec bash ...`

### Why This Works
- Unlike bash functions (which don't export to subshells), real files in PATH are always found
- The Linux scripts remain unchanged; macOS-specific compatibility is injected by wrappers
- Clean separation: wrappers handle syntax translation, Linux scripts remain source-of-truth

## Notes
- OpenGEM payload injection is fully functional
- All existing environment variables are preserved (e.g., `CIUKIOS_OPENGEM_TRY_EXEC`)
- Wrapper cleanup is automatic when the shell session ends (temp directory is in `$TMPDIR`)
