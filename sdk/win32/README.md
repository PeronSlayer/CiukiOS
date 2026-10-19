# f5-05A Win32 loader and ntdll core

This is freestanding i386 user-mode code under LGPL-2.1-or-later. It implements
the f5-02 PE checks and bounded directory walkers, relocation/binding/resource
handling, module lifetimes, initialization, static TLS, SEH state machines,
heaps, critical sections and the initial Rtl/Ldr helper surface. The same core
is compiled into a Linux i386 ELF, an MSVC-target PE DLL and a MinGW-target PE
DLL. It has no CRT imports and is built without SSE/SSE2/MMX.

`include/w32/services.h` is the private core backend boundary. Production
`services/ciuki.c` transports the frozen CIUKI_SYS_W32 records and f5-05B's
loader/wait-word extensions. `services/start.c` is the personality bootstrap;
it uses the kernel TEB/PEB and the original executable snapshot. The part A
fail-closed `ciuki_stub.c` remains an adapter regression fixture and is not
linked into the production builtin.

The Linux adapter remains a separate execution oracle. The kernel-backed H0
harness runs the shared loader through production syscall validation, VM,
image sections and COW, with explicit observer callbacks. It cannot execute
PE instructions or qualify a guest. `scripts/win32/builtins.py` builds the
production ntdll, census placeholders and JSON/binary store for `make apps`;
T1 validates hashes, version resources, ranges and payload readback.

```sh
nice -n 19 python3 tests/host/win32/adapter/build.py
nice -n 19 python3 scripts/win32/fixtures.py
nice -n 19 python3 -m unittest discover -s tests/host/win32 -p test_loader.py -v
nice -n 19 python3 -m unittest discover -s tests/host/win32 -p test_stubs.py -v
nice -n 19 python3 -m unittest discover -s tests/host/win32 -p test_wine.py -v
```

Run those suites sequentially, or all of them with
`ASAN_OPTIONS=detect_leaks=0 nice -n 19 python3 -B -m unittest discover -s tests/host/win32 -t .`. No image build or QEMU is part of this adapter.
The H0 native model compiles the actual C walkers/state machines with native
host pointers and an explicit TLS accessor. It cannot execute PE code or prove
i386 public layouts, FS behavior or calling conventions. Actual i386 builds
retain the fixed layout assertions and context assembly. SIGSYS or denial of
Wine's server socket is reported as a skipped execution gate, never a pass.

`w32host exec IMAGE DIR` starts IMAGE through `w32_ldr_start_process` with the
ELF-linked core; `w32host exec-core IMAGE CORE_DIR` registers the PE
`CORE_DIR/ntdll.dll` (and, if present, the H1 test-only `kernel32.dll` shim
from `tests/host/win32/adapter/kernel32_shim.c`) as builtins, binds the EXE to
them and delivers host faults to the PE `KiUserExceptionDispatcher` on the
user stack, as the kernel will (W7). `seh_probe.c` and `oracle.c` are
self-authored probes run both there and under Wine 11.18.

```sh
nice -n 19 build/host/win32/core/w32host validate build/host/win32/current/console.exe
nice -n 19 build/host/win32/core/w32host exec build/host/win32/current/seh-nested.exe build/host/win32/current
nice -n 19 python3 scripts/win32/stubs.py docs/design/research/win32-layer-2026-10-14.md --build build/host/win32/core/i686-pc-windows-msvc/ntdll.lib
nice -n 19 python3 scripts/win32/stubs.py --verify-store build/host/win32/stubs/builtin-store.json
nice -n 19 python3 tests/host/win32/adapter/wine_compare.py --probe seh_probe --adapter build/host/win32/core/w32host --core-directory build/host/win32/core/i686-pc-windows-msvc
```

Use the observer (`load IMAGE DLL_DIRECTORY observe`) for synthetic fixtures:
their entry points are structural probes, not valid DllMain/TLS routines. The
original cyclic/forwarder fixture graph collides without relocations; the tests
preserve that refusal and separately derive distinct-base graph images. DATAFILE
loads are separate module records, do not bind imports/relocate/run code, and
support resources through the common bounded tree walker. Every normal load
revalidates metadata after relocation before trusting import pointers.

Only existing EXE-rooted/dependency-rooted modules and pinned builtins survive
collection; dependency cycles are collectible. Initialization runs dependencies
first and detach runs in reverse initialization order. DllMain may query module
handles/exports under the recursive lock; its nested diagnostic result does not
replace the outer load result. FreeLibrary during initialization or notification
is explicitly refused (`unload-during-initialization`), avoiding invalidated
notification iterators. Thread hooks must be entered on the thread whose FS/TEB
they notify. Dynamic TLS installs separate templates for all registered threads.

Initial limitations requiring lead qualification are documented in the report:
bound IATs without names, overlap policy, external heap-region ownership/tuning
parameters, CP1252/Western Unicode helpers and per-drive cwd expansion, builtin
compatibility versions, signal/context restoration, and missing kernel section
sharing/validated continuation integration. This is not an F5 guest acceptance
claim. The generated stubs load but raise a noncontinuable STATUS_NOT_IMPLEMENTED
on first execution, with one bounded diagnostic per function. They do not yet
permit game startup through unimplemented APIs. `--implemented` accepts explicit
DLL/export-to-PE-forwarder mappings as implementations become available.
