# Free Windows 95/98 API probes

`probe.c` is project-owned test code under GNU GPLv2 (SPDX header in
source). `bash scripts/build_win32_probes.sh build/tests/win32-probes` builds
two 32-bit PE files with Windows 4.0 subsystem headers, using Clang and LLD.
The `.def` files name the imported Windows APIs; no Windows DLL is packaged.

- `HELLO.EXE` tests PE loading, `GetStdHandle`, `WriteFile`, and `ExitProcess`.
- `SETUP.EXE` additionally tests directory and file creation and HKCU registry
  writes through `ADVAPI32`.

The CiukiOS QEMU status gate is `scripts/qemu_test_win32_status.py`. It
currently checks for a bounded **unsupported** message. Once a Win32 runtime
exists, change the gate to demand `WIN32: PASS` and `SETUP: PASS` from CiukiOS
and verify the installed file and registry value on its disk image.
