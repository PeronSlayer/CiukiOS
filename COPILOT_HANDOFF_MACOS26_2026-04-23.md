# CiukiOS - VS Code macOS 26 Handoff

Date: 2026-04-23  
Target host: macOS 26 + VS Code

## 1) Scope
This handoff introduces a macOS-first wrapper layer for build and QEMU workflows without rewriting existing Linux-oriented scripts.

The wrappers solve the main compatibility gap in this repository:
- GNU-flavored tool expectations (`stat -c`, `timeout`, etc.)
- Homebrew-based toolchain discovery

## 2) New macOS scripts
All scripts are under `scripts/macos`:

1. `scripts/macos/setup_macos26.sh`
- checks or installs required Homebrew packages
- `--install` mode installs toolchain

2. `scripts/macos/common.sh`
- prepares GNU toolchain PATH from Homebrew
- provides command checks before running build/test flows

3. `scripts/macos/build_full_macos.sh`
- mac-compatible wrapper for `scripts/build_full.sh`

4. `scripts/macos/build_floppy_macos.sh`
- mac-compatible wrapper for `scripts/build_floppy.sh`

5. `scripts/macos/qemu_run_full_macos.sh`
- mac-compatible wrapper for `scripts/qemu_run_full.sh`

6. `scripts/macos/qemu_run_floppy_macos.sh`
- mac-compatible wrapper for `scripts/qemu_run_floppy.sh`

7. `scripts/macos/qemu_test_all_macos.sh`
- combined smoke workflow for full+floppy on macOS

## 3) First-time setup on macOS 26
Run:

```bash
bash scripts/macos/setup_macos26.sh --install
```

Expected Homebrew packages:
- nasm
- qemu
- mtools
- coreutils
- gnu-sed
- findutils
- gawk

## 4) Daily commands on macOS 26
Build:

```bash
bash scripts/macos/build_full_macos.sh
bash scripts/macos/build_floppy_macos.sh
```

Run smoke tests:

```bash
bash scripts/macos/qemu_run_full_macos.sh --test
bash scripts/macos/qemu_run_floppy_macos.sh --test
```

Combined:

```bash
bash scripts/macos/qemu_test_all_macos.sh
```

## 5) Notes
1. Existing Linux scripts remain the source of truth; mac wrappers call into them.
2. If wrappers report missing commands, rerun setup script and verify Homebrew installation path.
3. For OpenGEM execution experiments, keep using existing env flags (for example `CIUKIOS_OPENGEM_TRY_EXEC`).

## 6) Recommended next step (Mac)
If you want fully native cross-host scripts, migrate shared helpers (`stat`/`timeout`) into a single repo-level compatibility library and source it from the original scripts directly.
